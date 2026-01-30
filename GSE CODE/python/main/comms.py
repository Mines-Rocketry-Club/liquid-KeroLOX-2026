# comms.py
import time
import threading
from typing import Optional, Callable
import socket

import serial
from serial.tools import list_ports

BAUD = 115200
RETRY_DELAY = 2.0


def find_arduino_port() -> Optional[str]:
    ports = list_ports.comports()
    candidates = []

    for p in ports:
        desc = (p.description or "").lower()
        dev = (p.device or "").lower()

        if "arduino" in desc:
            candidates.append(p.device)
        elif "ttyacm" in dev or "ttyusb" in dev or "usbmodem" in dev:
            candidates.append(p.device)
        elif dev.startswith("com") and "serial" in desc:
            candidates.append(p.device)

    return candidates[0] if candidates else None


class ArduinoComms:
    def __init__(
        self,
        on_log: Callable[[str], None],
        on_state: Callable[[str], None],
        on_metric: Callable[[str, float], None],
        on_message: Optional[Callable[[str], None]] = None,
        on_prompt: Optional[Callable[[str, str], None]] = None,
        on_snapshot: Optional[Callable[[], None]] = None,
        csv_path: str = "telemetryFile.csv",
    ):
        self.on_log = on_log
        self.on_state = on_state
        self.on_metric = on_metric
        self.on_message = on_message
        self.on_prompt = on_prompt
        self.on_snapshot = on_snapshot

        self.csv_path = csv_path

        self.ser: Optional[serial.Serial] = None
        self.stop_flag = False
        self.reader_thread: Optional[threading.Thread] = None
        self.conn_thread: Optional[threading.Thread] = None

        self.last_cmd_sent_time: float = 0.0
        self.last_ping_ms: Optional[float] = None

        self._last_get_event_time: Optional[float] = None
        self.telemetry_interval_ms: Optional[float] = None

        self._waiting_for_snapshot: bool = False
        self._config_sent: bool = False

        self._net_host: str = "192.168.88.254"
        self._net_port: int = 8000
        self._last_net_ok: Optional[bool] = None

        self.net_ok: bool = True

        self.conn_thread = threading.Thread(target=self._connection_loop, daemon=True)
        self.conn_thread.start()

    def request_state(self, text: str):
        line = (text or "").strip()
        if not line:
            return

        if not self._serial_ok():
            self.on_log(f"(no serial) Would send: {line}")
            return

        try:
            self._start_ping()
            self.ser.write((line + "\n").encode("utf-8"))
            self.on_log(f"> {line}")
        except Exception as e:
            self.on_log(f"Error sending line: {e}")
            self._handle_serial_failure()

    def send_answer(self, prompt_id: str, answer: str):
        ans = (answer or "").strip()

        if not self._serial_ok():
            self.on_log(f"(no serial) Would send answer: {ans}")
            return

        try:
            self._start_ping()
            line = f"ANSWER {ans}\n"
            self.ser.write(line.encode("utf-8"))
            self.on_log(f"> {line.strip()}")
        except Exception as e:
            self.on_log(f"Error sending answer: {e}")
            self._handle_serial_failure()

    def request_telemetry(self):
        if not self._serial_ok():
            return

        if not getattr(self, "net_ok", True):
            return

        if self._waiting_for_snapshot:
            return

        try:
            now = time.time()
            if self._last_get_event_time is not None:
                self.telemetry_interval_ms = (now - self._last_get_event_time) * 1000.0
            self._last_get_event_time = now

            self._start_ping()
            self._waiting_for_snapshot = True
            self.ser.write(b"GET\n")
        except Exception as e:
            self.on_log(f"Error sending GET: {e}")
            self._handle_serial_failure()

    def close(self):
        self.stop_flag = True
        if self.reader_thread and self.reader_thread.is_alive():
            self.reader_thread.join(timeout=1.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.on_log("Serial closed.")

    def _check_pad_reachable(self, timeout: float = 0.1) -> bool:
        ok = False
        try:
            with socket.create_connection((self._net_host, self._net_port), timeout=timeout):
                ok = True
        except OSError:
            ok = False

        if self._last_net_ok is None:
            self._last_net_ok = ok
        else:
            if ok and self._last_net_ok is False:
                self.on_log("Connection reestablished with pad host.")
            elif not ok and self._last_net_ok is True:
                self.on_log("(net down) Skipping GET; pad host unreachable.")
            self._last_net_ok = ok

        return ok

    def _connection_loop(self):
        while not self.stop_flag:
            if not self._serial_ok():
                port = find_arduino_port()
                if not port:
                    self.on_log("No Arduino detected. Retrying...")
                    time.sleep(RETRY_DELAY)
                    continue

                try:
                    self.ser = serial.Serial(
                        port,
                        BAUD,
                        timeout=0.05,
                        write_timeout=0.2,
                    )
                    time.sleep(2)
                    self.on_log(f"Connected to Arduino on {port} @ {BAUD} baud.")
                    self._config_sent = False

                    self.reader_thread = threading.Thread(target=self._reader, daemon=True)
                    self.reader_thread.start()

                    self._send_csv_config()

                except Exception as e:
                    self.on_log(f"Failed to open {port}: {e}")
                    self.ser = None

            time.sleep(RETRY_DELAY)

    def _serial_ok(self) -> bool:
        return bool(self.ser and self.ser.is_open)

    def _handle_serial_failure(self):
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self._config_sent = False
        self.on_log("Serial connection lost. Will retry...")

    def _send_csv_config(self):
        if self._config_sent:
            return
        if not self._serial_ok():
            return

        try:
            with open(self.csv_path, "r", encoding="utf-8") as f:
                self.on_log(f"Uploading telemetry config from {self.csv_path}...")
                self.ser.write(b"CFGBEGIN\n")
                for line in f:
                    line = line.rstrip("\r\n")
                    if not line:
                        continue
                    self.ser.write(line.encode("utf-8") + b"\n")
                    time.sleep(0.005)
                self.ser.write(b"CFGEND\n")

            self._config_sent = True
            self.on_log("Telemetry config upload complete.")
        except Exception as e:
            self.on_log(f"Error sending telemetry CSV: {e}")
            self._config_sent = True

    def _reader(self):
        while not self.stop_flag and self._serial_ok():
            try:
                raw = self.ser.readline()
                if not raw:
                    time.sleep(0.01)
                    continue

                line = raw.decode(errors="ignore").strip()
                if line:
                    self._parse(line)

            except Exception as e:
                self.on_log(f"Serial read error: {e}")
                self._handle_serial_failure()
                break

    def _start_ping(self):
        self.last_cmd_sent_time = time.time()

    def _stop_ping(self):
        if self.last_cmd_sent_time > 0:
            dt_ms = (time.time() - self.last_cmd_sent_time) * 1000.0
            self.last_ping_ms = dt_ms
            self.last_cmd_sent_time = 0.0

    def _parse(self, line: str):
        self._stop_ping()

        parts = line.split()
        if not parts:
            return

        key = parts[0].upper()

        if key == "END":
            self._waiting_for_snapshot = False
            if self.on_snapshot:
                self.on_snapshot()
            return

        if key == "STATE" and len(parts) > 1:
            state_name = " ".join(parts[1:])
            self.on_state(state_name)
            return

        if key == "MSG" and len(parts) > 1:
            msg = " ".join(parts[1:])
            if self.on_message:
                self.on_message(msg)
            else:
                self.on_log(f"[Arduino] {msg}")
            return

        if key == "PROMPT" and len(parts) > 1:
            question = " ".join(parts[1:])
            if self.on_prompt:
                self.on_prompt("__PROMPT__", question)
            else:
                self.on_log(f"[Arduino PROMPT] {question}")
            return

        if len(parts) == 2:
            name = parts[0]
            try:
                value = float(parts[1])
                self.on_metric(name, value)
            except ValueError:
                pass
            return
