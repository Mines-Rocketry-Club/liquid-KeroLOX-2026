# main.py
import csv
import os
import threading
import time
import subprocess
from pathlib import Path
from typing import Dict, List, Optional

from gui import Dashboard
from comms import ArduinoComms

PAD_IP = "192.168.88.254"
CHECK_INTERVAL = 0.5  # seconds


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
    return names


def next_recording_path(folder: str, base_name: str = "recording", ext: str = ".csv") -> str:
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
    recording_path = next_recording_path("recordings", base_name="recording", ext=".csv")
    recorder = TelemetryRecorderWide(recording_path, program_start, metric_names)
    gui.log_from_thread(f"Recording output file: {recording_path}")

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
        recorder.close()
        comms.close()
        gui.destroy()

    gui.protocol("WM_DELETE_WINDOW", on_close)
    gui.mainloop()


if __name__ == "__main__":
    main()
