# comms.py
# Handles serial communication with the Arduino:
# - auto-detect USB serial port and reconnect on failure
# - sends STATE, GET, and ANS commands
# - pushes telemetryFile.csv at startup:
#       CFGBEGIN
#       <csv lines>
#       CFGEND
# - parses STATE, MSG, ASK and dynamic metrics "<name> <value>"
# - tracks telemetry interval and a unified ping time

import time
import threading
from typing import Optional, Callable

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
        csv_path: str = "telemetryFile.csv",
    ):
        self.on_log = on_log
        self.on_state = on_state
        self.on_metric = on_metric
        self.on_message = on_message
        self.on_prompt = on_prompt

        self.csv_path = csv_path

        self.ser: Optional[serial.Serial] = None
        self.stop_flag = False
        self.reader_thread: Optional[threading.Thread] = None
        self.conn_thread: Optional[threading.Thread] = None

        # Ping timing
        self.last_cmd_sent_time: float = 0.0
        self.last_ping_ms: Optional[float] = None

        # Telemetry request interval (based on GET cadence)
        self._last_get_event_time: Optional[float] = None
        self.telemetry_interval_ms: Optional[float] = None

        # Track if we're waiting for an END from the last snapshot
        self._waiting_for_snapshot: bool = False

        # Whether CSV config has been pushed for this connection
        self._config_sent: bool = False

        # Start connection manager
        self.conn_thread = threading.Thread(target=self._connection_loop, daemon=True)
        self.conn_thread.start()

    # ------------------------------------------------------
    # Public commands
    # ------------------------------------------------------
    def request_state(self, state: str):
        """
        Send a state command to the Arduino (SAFE, ABORT, FIRE, ARM, FILL, etc).
        Always uppercases before sending so Arduino sees canonical strings.
        """
        s = state.strip().upper()
        if not s:
            return

        if not self._serial_ok():
            self.on_log(f"(no serial) Would request state '{s}'")
            return

        try:
            self._start_ping()
            self.ser.write((s + "\n").encode("utf-8"))
            self.on_log(f"> REQUESTED STATE = {s}")
        except Exception as e:
            self.on_log(f"Error sending state: {e}")
            self._handle_serial_failure()

    def send_answer(self, prompt_id: str, answer: str):
        if not self._serial_ok():
            self.on_log(f"(no serial) Would send answer {prompt_id}: {answer}")
            return

        try:
            self._start_ping()
            line = f"ANS {prompt_id} {answer}\n"
            self.ser.write(line.encode("utf-8"))
            self.on_log(f"> {line.strip()}")
        except Exception as e:
            self.on_log(f"Error sending answer: {e}")
            self._handle_serial_failure()

    def request_telemetry(self):
        if not self._serial_ok():
            return

        if self._waiting_for_snapshot:
            # still waiting on END from the previous snapshot
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

    # ------------------------------------------------------
    # Connection management
    # ------------------------------------------------------
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
                    time.sleep(2)  # Arduino reset on open
                    self.on_log(f"Connected to Arduino on {port} @ {BAUD} baud.")
                    self._config_sent = False

                    # Start reader
                    self.reader_thread = threading.Thread(
                        target=self._reader, daemon=True
                    )
                    self.reader_thread.start()

                    # Send telemetry CSV for this connection
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

    # ------------------------------------------------------
    # CSV configuration upload
    # ------------------------------------------------------
    def _send_csv_config(self):
        if self._config_sent:
            return
        if not self._serial_ok():
            return

        try:
            with open(self.csv_path, "r", encoding="utf-8") as f:
                self.on_log(f"Uploading telemetry config from {self.csv_path}...")
                # Config upload isn't part of ping timing
                self.ser.write(b"CFGBEGIN\n")
                for line in f:
                    line = line.rstrip("\r\n")
                    if not line:
                        continue
                    self.ser.write(line.encode("utf-8") + b"\n")
                    time.sleep(0.005)  # small delay so Arduino's buffer keeps up
                self.ser.write(b"CFGEND\n")

            self._config_sent = True
            self.on_log("Telemetry config upload complete.")
        except Exception as e:
            self.on_log(f"Error sending telemetry CSV: {e}")
            # Mark as sent so we don't spam attempts every 2 seconds
            self._config_sent = True

    # ------------------------------------------------------
    # Reader thread
    # ------------------------------------------------------
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

    # ------------------------------------------------------
    # Ping helpers
    # ------------------------------------------------------
    def _start_ping(self):
        self.last_cmd_sent_time = time.time()

    def _stop_ping(self):
        if self.last_cmd_sent_time > 0:
            dt_ms = (time.time() - self.last_cmd_sent_time) * 1000.0
            self.last_ping_ms = dt_ms
            self.last_cmd_sent_time = 0.0

    # ------------------------------------------------------
    # Parse inbound lines
    # ------------------------------------------------------
    def _parse(self, line: str):
        # ANY line counts as a response for ping timing
        self._stop_ping()

        parts = line.split()
        if not parts:
            return

        key = parts[0].upper()

        if key == "END":
            self._waiting_for_snapshot = False
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

        if key == "ASK" and len(parts) > 2:
            prompt_id = parts[1]
            question = " ".join(parts[2:])
            if self.on_prompt:
                self.on_prompt(prompt_id, question)
            else:
                self.on_log(f"[Arduino ASK {prompt_id}] {question}")
            return

        # Dynamic metric: "<Name> <value>"
        if len(parts) == 2:
            name = parts[0]
            try:
                value = float(parts[1])
                self.on_metric(name, value)
            except ValueError:
                pass
            return

        # Otherwise ignore or optionally log
        # self.on_log(f"(unparsed) {line}")
