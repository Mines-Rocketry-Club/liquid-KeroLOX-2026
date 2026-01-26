import threading
import time
import subprocess

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


def main():
    gui = Dashboard()

    comms = ArduinoComms(
        on_log=gui.log_from_thread,
        on_state=gui.apply_state_from_thread,
        on_metric=gui.apply_metric_from_thread,
        on_message=gui.show_message_from_thread,
        on_prompt=gui.show_prompt_from_thread,
    )

    start_network_thread(comms, gui)

    gui.on_state_request = comms.request_state
    gui.on_prompt_answer = comms.send_answer

    gui.start_telemetry_loop(comms, interval_ms=500, guard_ms=300)

    def on_close():
        comms.close()
        gui.destroy()

    gui.protocol("WM_DELETE_WINDOW", on_close)
    gui.mainloop()


if __name__ == "__main__":
    main()
