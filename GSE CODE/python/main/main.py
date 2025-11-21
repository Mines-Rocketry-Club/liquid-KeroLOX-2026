# main.py
import threading
import time
import socket

from gui import Dashboard
from comms import ArduinoComms

PAD_IP = "192.168.88.254"
PAD_PORT = 8000
CHECK_INTERVAL = 0.5   # seconds


def check_pad_reachable(host: str, port: int, timeout: float = 0.1) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def start_network_thread(comms: ArduinoComms, gui: Dashboard):
    """
    Background thread:
      - Updates comms.net_ok
      - Only logs transitions
    """
    def run():
        last_state = None
        while True:
            ok = check_pad_reachable(PAD_IP, PAD_PORT)
            comms.net_ok = ok

            if last_state is None:
                last_state = ok
            else:
                if ok and not last_state:
                    gui.log_from_thread("Connection reestablished with pad host.")
                elif not ok and last_state:
                    gui.log_from_thread("Pad host unreachable.")

                last_state = ok

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
        on_prompt=lambda pid, q: gui.show_prompt_from_thread(pid, q),
    )

    # Start the background network monitor
    start_network_thread(comms, gui)

    # Wire GUI → Comms
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
