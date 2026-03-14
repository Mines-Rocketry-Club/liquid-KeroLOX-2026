import csv
import threading
import time
import subprocess
from pathlib import Path
from typing import Dict, List

from gui import Dashboard
from comms import ArduinoComms
from hx711_reader import PiLoadCell

PAD_IP = "192.168.88.254"
CHECK_INTERVAL = 0.5
LOAD_CELL_INTERVAL = 0.2

THRUST_NAME = "thrust"
LOX_NAME = "LOX_level"


def compute_calibration(raw0: float, raw10: float):
    if raw10 == raw0:
        raise ValueError("10 kg raw value cannot equal 0 kg raw value.")
    tare = float(raw0)
    cal = 10.0 / (float(raw10) - float(raw0))
    return tare, cal


# Enter your starting calibration raw values here.
# These can later be changed live from the GUI for LOX_level.
THRUST_RAW_0KG = 0.0
THRUST_RAW_10KG = 1.0

LOX_RAW_0KG = 0.0
LOX_RAW_10KG = 1.0


THRUST_TARE, THRUST_CAL = compute_calibration(
    THRUST_RAW_0KG,
    THRUST_RAW_10KG,
)

LOX_TARE, LOX_CAL = compute_calibration(
    LOX_RAW_0KG,
    LOX_RAW_10KG,
)


def check_ip_exists(ip: str) -> bool:
    try:
        result = subprocess.run(
            ["ping", "-c", "1", "-W", "1", ip],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        return result.returncode == 0
    except Exception:
        return False


def start_network_thread(comms: ArduinoComms, gui: Dashboard):
    def run():
        last = None
        while True:
            ok = check_ip_exists(PAD_IP)
            comms.net_ok = ok

            if last is None:
                last = ok
            else:
                if ok and not last:
                    gui.log_from_thread("Laptop detected (.254). Telemetry enabled.")
                elif not ok and last:
                    gui.log_from_thread("Laptop missing (.254). Telemetry paused.")
                last = ok

            time.sleep(CHECK_INTERVAL)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def start_thrust_load_cell_thread(
    load_cell: PiLoadCell,
    gui: Dashboard,
    recorder,
):
    def run():
        while True:
            try:
                raw_val = load_cell.read_raw(samples=3)
                if raw_val is not None:
                    scaled_val = load_cell.raw_to_value(raw_val)
                    recorder.update_metric(THRUST_NAME, scaled_val)
                    gui.apply_metric_from_thread(THRUST_NAME, scaled_val)
            except Exception as e:
                gui.log_from_thread(f"{THRUST_NAME}: HX711 thread error: {e}")

            time.sleep(LOAD_CELL_INTERVAL)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def start_lox_load_cell_thread(
    load_cell: PiLoadCell,
    gui: Dashboard,
    recorder,
):
    def run():
        while True:
            try:
                raw_val = load_cell.read_raw(samples=3)
                if raw_val is not None:
                    gui.update_raw_lox_from_thread(raw_val)

                    scaled_val = load_cell.raw_to_value(raw_val)
                    recorder.update_metric(LOX_NAME, scaled_val)
                    gui.apply_metric_from_thread(LOX_NAME, scaled_val)
            except Exception as e:
                gui.log_from_thread(f"{LOX_NAME}: HX711 thread error: {e}")

            time.sleep(LOAD_CELL_INTERVAL)

    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def load_metric_names_from_csv(path: str) -> List[str]:
    names: List[str] = []

    try:
        with open(path, newline="", encoding="utf-8") as f:
            reader = csv.DictReader(f)
            for row in reader:
                n = (row.get("name") or "").strip()
                if n:
                    names.append(n)
    except FileNotFoundError:
        pass

    injected = [THRUST_NAME, LOX_NAME]
    names = [n for n in names if n not in injected]

    return injected + names


def next_recording_path(
    folder: str,
    base_name: str = "recording",
    ext: str = ".csv",
) -> str:
    Path(folder).mkdir(parents=True, exist_ok=True)

    p0 = Path(folder) / f"{base_name}{ext}"
    if not p0.exists():
        return str(p0)

    i = 1
    while True:
        p = Path(folder) / f"{base_name}_{i:03d}{ext}"
        if not p.exists():
            return str(p)
        i += 1


class TelemetryRecorderWide:
    def __init__(self, path: str, t0: float, metric_names: List[str]):
        self.path = path
        self.t0 = t0
        self.lock = threading.Lock()
        self.columns = ["time"] + list(metric_names)
        self.latest: Dict[str, float] = {}

        self.f = open(self.path, "w", newline="", encoding="utf-8")
        self.w = csv.writer(self.f)
        self.w.writerow(self.columns)
        self.f.flush()

    def update_metric(self, name: str, value: float):
        with self.lock:
            if name not in self.columns:
                return
            self.latest[name] = value

    def snapshot(self):
        with self.lock:
            t_s = time.time() - self.t0
            row = [f"{t_s:.3f}"]

            for name in self.columns[1:]:
                v = self.latest.get(name, None)
                row.append("" if v is None else f"{v:.6g}")

            self.w.writerow(row)
            self.f.flush()

    def close(self):
        try:
            self.f.close()
        except Exception:
            pass


def main():
    program_start = time.time()

    gui = Dashboard()
    metric_names = load_metric_names_from_csv("telemetryFile.csv")

    recording_path = next_recording_path(
        "recordings",
        base_name="recording",
        ext=".csv",
    )
    recorder = TelemetryRecorderWide(recording_path, program_start, metric_names)

    gui.log_from_thread(f"Recording output file: {recording_path}")

    thrust_cell = PiLoadCell(
        name=THRUST_NAME,
        dout_pin=24,
        pd_sck_pin=25,
        tare_value=THRUST_TARE,
        calibration_factor=THRUST_CAL,
        on_log=gui.log_from_thread,
    )

    lox_cell = PiLoadCell(
        name=LOX_NAME,
        dout_pin=22,
        pd_sck_pin=23,
        tare_value=LOX_TARE,
        calibration_factor=LOX_CAL,
        on_log=gui.log_from_thread,
    )

    gui.set_lox_calibration_inputs(LOX_RAW_0KG, LOX_RAW_10KG)

    initial_thrust_raw = thrust_cell.read_raw(samples=5)
    if initial_thrust_raw is not None:
        initial_thrust_value = thrust_cell.raw_to_value(initial_thrust_raw)
        recorder.update_metric(THRUST_NAME, initial_thrust_value)
        gui.apply_metric_from_thread(THRUST_NAME, initial_thrust_value)
        gui.log_from_thread(f"{THRUST_NAME}: startup reading {initial_thrust_value:.2f}")
    else:
        gui.log_from_thread(f"{THRUST_NAME}: startup reading unavailable.")

    initial_lox_raw = lox_cell.read_raw(samples=5)
    if initial_lox_raw is not None:
        gui.update_raw_lox_from_thread(initial_lox_raw)
        initial_lox_value = lox_cell.raw_to_value(initial_lox_raw)
        recorder.update_metric(LOX_NAME, initial_lox_value)
        gui.apply_metric_from_thread(LOX_NAME, initial_lox_value)
        gui.log_from_thread(f"{LOX_NAME}: startup reading {initial_lox_value:.2f}")
    else:
        gui.log_from_thread(f"{LOX_NAME}: startup reading unavailable.")

    def apply_lox_calibration(raw0: float, raw10: float):
        try:
            tare, cal = compute_calibration(raw0, raw10)
            lox_cell.set_tare_value(tare)
            lox_cell.set_calibration_factor(cal)
            gui.log_from_thread(
                f"{LOX_NAME} calibration updated: tare={tare:.2f}, cal={cal:.8f}"
            )
        except Exception as e:
            gui.log_from_thread(f"{LOX_NAME} calibration update failed: {e}")

    gui.on_apply_lox_calibration = apply_lox_calibration

    start_thrust_load_cell_thread(thrust_cell, gui, recorder)
    start_lox_load_cell_thread(lox_cell, gui, recorder)

    def on_metric(name: str, value: float):
        gui.apply_metric_from_thread(name, value)
        if gui.recording:
            try:
                recorder.update_metric(name, value)
            except Exception as e:
                gui.log_from_thread(f"Recorder error: {e}")

    def on_snapshot():
        if gui.recording:
            try:
                recorder.snapshot()
            except Exception as e:
                gui.log_from_thread(f"Recorder error: {e}")

    comms = ArduinoComms(
        on_log=gui.log_from_thread,
        on_state=gui.apply_state_from_thread,
        on_metric=on_metric,
        on_message=gui.show_message_from_thread,
        on_prompt=gui.show_prompt_from_thread,
        on_snapshot=on_snapshot,
    )

    start_network_thread(comms, gui)

    gui.on_state_request = comms.request_state
    gui.on_prompt_answer = comms.send_answer
    gui.start_telemetry_loop(comms, interval_ms=500, guard_ms=300)

    def on_close():
        thrust_cell.cleanup()
        lox_cell.cleanup()
        recorder.close()
        comms.close()
        gui.destroy()

    gui.protocol("WM_DELETE_WINDOW", on_close)
    gui.mainloop()


if __name__ == "__main__":
    main()