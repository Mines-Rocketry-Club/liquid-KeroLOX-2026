import csv
import time
import tkinter as tk
from tkinter import ttk

BG_COLOR = "#A9A9A9"
DEFAULT_STATE_COLOR = "#D3D3D3"


class Dashboard(tk.Tk):
    def __init__(self, telemetry_csv: str = "telemetryFile.csv"):
        super().__init__()
        self.title("System Dashboard")
        self.geometry("700x540")
        self.configure(bg=BG_COLOR)

        self.state = "LEVEL_CHECK"
        self.state_colors = {self.state: DEFAULT_STATE_COLOR}

        self.special_state_colors = {
            "ABORT": "#FF6F61",
            "PRESSURIZE_FUEL": "#FF6F61",
            "DEPRESSURIZE_FUEL": "#FF6F61",
            "ARMED": "#FF6F61",
            "FIRE": "#FF3333",
            "MISFIRE": "#FF6F61",
            "FIRED": "#FF6F61",
            "CONFIRM_VENT": "#FF6F61",
            "VENT": "#FF6F61",
            "VENTING_COMPLETE": "#FF6F61",
            "SAFE_UNLOADED": "#98FB98",
        }

        self.telemetry_enabled = True
        self.last_control_cmd_time = 0.0

        self.recording = False

        self.metrics = {}
        self.meta = self._load_telemetry_csv(telemetry_csv)

        self.on_state_request = None
        self.on_prompt_answer = None
        self.pending_prompt_id = None

        self.state_label = None
        self.telemetry_freq_label = None
        self.ping_label = None
        self.tree = None
        self.cmd = None
        self.log = None
        self.record_button = None

        self._build_ui()
        self._apply_state(self.state)

        self._log("GUI ready. Using telemetryFile.csv definitions.")
        self._log("Type commands. Prompts will ask for answers.")

    def _load_telemetry_csv(self, path: str):
        meta = {}
        try:
            with open(path, newline="") as f:
                reader = csv.DictReader(f)
                for row in reader:
                    name = (row.get("name") or "").strip()
                    if not name:
                        continue

                    def to_float(key):
                        val = row.get(key, "")
                        try:
                            return float(val)
                        except ValueError:
                            return None

                    meta[name] = {
                        "owner": (row.get("owner") or "").strip(),
                        "unit": (row.get("unit") or "").strip(),
                        "low_red": to_float("low_red"),
                        "high_red": to_float("high_red"),
                        "low_yellow": to_float("low_yellow"),
                        "high_yellow": to_float("high_yellow"),
                        "partnumber": (row.get("partnumber") or "").strip(),
                    }
        except FileNotFoundError:
            pass

        return meta

    def log_from_thread(self, msg: str):
        self.after(0, lambda: self._log(msg))

    def apply_state_from_thread(self, state: str):
        self.after(0, lambda: self._apply_state(state))

    def apply_metric_from_thread(self, name: str, value: float):
        self.after(0, lambda: self._apply_metric(name, value))

    def show_message_from_thread(self, text: str):
        self.after(0, lambda: self._log(f"[Arduino] {text}"))

    def show_prompt_from_thread(self, pid: str, question: str):
        def _p():
            self.pending_prompt_id = pid
            self._log(f"[Arduino PROMPT] {question}")
        self.after(0, _p)

    def _build_ui(self):
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background=BG_COLOR)
        style.configure("TLabel", background=BG_COLOR, foreground="black")

        state_bar = ttk.Frame(self)
        state_bar.pack(fill="x", padx=10, pady=(10, 6))

        ttk.Label(state_bar, text="System State:").pack(side="left")

        self.state_label = tk.Label(
            state_bar,
            text=self.state,
            width=24,
            relief="groove",
            bd=2,
            bg=DEFAULT_STATE_COLOR,
            fg="black",
            font=("Arial", 14, "bold"),
        )
        self.state_label.pack(side="left", padx=10)

        tele_frame = ttk.Frame(self)
        tele_frame.pack(fill="x", padx=10, pady=(0, 6))

        self.telemetry_freq_label = tk.Label(
            tele_frame, text="Telemetry: -- Hz", bg=BG_COLOR, fg="black"
        )
        self.telemetry_freq_label.pack(side="left")

        self.ping_label = tk.Label(
            tele_frame, text="Ping: -- ms", bg=BG_COLOR, fg="black"
        )
        self.ping_label.pack(side="left", padx=20)

        ttk.Separator(self, orient="horizontal").pack(fill="x", padx=10, pady=5)

        style.configure(
            "RedBig.TButton",
            font=("Arial", 16, "bold"),
            padding=(18, 10),
            background="#FF3333",
            foreground="black",
            borderwidth=3,
            relief="raised",
        )
        style.map(
            "RedBig.TButton",
            background=[("active", "#E62A2A"), ("pressed", "#CC0000")],
        )

        style.configure(
            "Big.TButton",
            font=("Arial", 16, "bold"),
            padding=(18, 10),
            borderwidth=3,
            relief="raised",
        )

        style.configure(
            "RecordOn.Big.TButton",
            font=("Arial", 16, "bold"),
            padding=(18, 10),
            borderwidth=3,
            relief="raised",
            background="#98FB98",
            foreground="black",
        )
        style.map(
            "RecordOn.Big.TButton",
            background=[("active", "#7CFF7C"), ("pressed", "#5FE65F")],
        )

        btn_frame = ttk.Frame(self)
        btn_frame.pack(fill="x", padx=10, pady=3)

        self.record_button = ttk.Button(
            btn_frame,
            text="Not Recording",
            style="Big.TButton",
            command=self.toggle_recording,
        )
        self.record_button.pack(side="left", expand=True, padx=10)

        ttk.Button(
            btn_frame,
            text="ABORT",
            style="RedBig.TButton",
            command=lambda: self._send_raw("ABORT"),
        ).pack(side="left", expand=True, padx=10)

        ttk.Button(
            btn_frame,
            text="FIRE",
            style="RedBig.TButton",
            command=lambda: self._send_raw("ANSWER FIRE"),
        ).pack(side="right", expand=True, padx=20)

        self.tree = ttk.Treeview(
            self,
            columns=("Name", "Owner", "Value", "Unit"),
            show="headings",
            height=10,
        )
        self.tree.heading("Name", text="Measurement")
        self.tree.heading("Owner", text="Owner")
        self.tree.heading("Value", text="Value")
        self.tree.heading("Unit", text="Unit")

        self.tree.column("Name", width=220, anchor="center")
        self.tree.column("Owner", width=160, anchor="center")
        self.tree.column("Value", width=140, anchor="center")
        self.tree.column("Unit", width=80, anchor="center")

        self.tree.tag_configure("red", background="#FF6F61", foreground="black")
        self.tree.tag_configure("yellow", background="#FFD700", foreground="black")
        self.tree.tag_configure("green", background="#98FB98", foreground="black")
        self.tree.tag_configure("gray", background="#D3D3D3", foreground="black")

        self.tree.pack(fill="both", expand=True, padx=10, pady=5)

        cmd_frame = ttk.Frame(self)
        cmd_frame.pack(fill="x", padx=10, pady=5)

        ttk.Label(cmd_frame, text="Command / Answer:").pack(side="left")

        self.cmd = ttk.Entry(cmd_frame)
        self.cmd.pack(side="left", fill="x", expand=True, padx=5)
        self.cmd.bind("<Return>", self.on_command)

        ttk.Button(cmd_frame, text="Run", command=self.on_command).pack(side="left")

        self.log = tk.Text(
            self, height=6, state="disabled", bg="#C0C0C0", fg="black"
        )
        self.log.pack(fill="both", padx=10, pady=5)

    def toggle_recording(self):
        self.recording = not self.recording

        self.record_button.configure(
            text="Recording" if self.recording else "Not Recording",
            style="RecordOn.Big.TButton" if self.recording else "Big.TButton",
        )

        self._log(f"Recording {'enabled' if self.recording else 'disabled'}")

    def _send_raw(self, text: str):
        self.last_control_cmd_time = time.time()
        if self.on_state_request:
            self.on_state_request(text)

    def _apply_state(self, new_state: str):
        upper = (new_state or "").strip().upper()
        color = self.special_state_colors.get(upper, DEFAULT_STATE_COLOR)

        self.state_colors[upper] = color
        self.state = upper

        self.state_label.config(text=upper, bg=color)
        self._log(f"Arduino is now in state: {upper}")

    def _apply_metric(self, name: str, value: float):
        self.metrics[name] = {"value": value}
        self.refresh_table()

    def _metric_tag(self, name: str, value: float) -> str:
        m = self.meta.get(name)
        if m is None:
            return "gray"

        lr = m["low_red"]
        hr = m["high_red"]
        ly = m["low_yellow"]
        hy = m["high_yellow"]

        try:
            v = float(value)
        except (TypeError, ValueError):
            return "gray"

        if lr is not None and v < lr:
            return "red"
        if hr is not None and v > hr:
            return "red"
        if ly is not None and v < ly:
            return "yellow"
        if hy is not None and v > hy:
            return "yellow"

        return "green"

    def refresh_table(self):
        self.tree.delete(*self.tree.get_children())
        for name, data in sorted(self.metrics.items()):
            val = data.get("value")

            m = self.meta.get(name, {})
            owner = m.get("owner", "")
            unit = m.get("unit", "")

            val_str = "--" if val is None else f"{val:.2f}"
            tag = self._metric_tag(name, val)

            self.tree.insert(
                "",
                "end",
                values=(name, owner, val_str, unit),
                tags=(tag,),
            )

    def on_command(self, event=None):
        text = self.cmd.get().strip()
        self.cmd.delete(0, "end")

        if not text:
            return

        upper = text.upper().strip()

        if self.pending_prompt_id is not None:
            if upper == "ABORT" or upper.startswith("ELEVATED "):
                self._send_raw(text)
                return

            pid = self.pending_prompt_id
            self.pending_prompt_id = None
            self.last_control_cmd_time = time.time()

            if self.on_prompt_answer:
                self.on_prompt_answer(pid, text)
            return

        self._send_raw(text)

    def start_telemetry_loop(self, comms, interval_ms=500, guard_ms=300):
        def _poll():
            try:
                now = time.time()
                dt_ms = (now - self.last_control_cmd_time) * 1000.0
                if dt_ms >= guard_ms:
                    comms.request_telemetry()
            finally:
                self.after(interval_ms, _poll)

        def _stats():
            interval = getattr(comms, "telemetry_interval_ms", None)
            ping = getattr(comms, "last_ping_ms", None)

            freq = 1000.0 / interval if interval and interval > 0 else None
            self.update_telemetry_status(freq, ping)

            self.after(1000, _stats)

        self.after(interval_ms, _poll)
        self.after(1000, _stats)

    def update_telemetry_status(self, freq, ping):
        if freq is None:
            self.telemetry_freq_label.config(text="Telemetry: -- Hz")
        else:
            self.telemetry_freq_label.config(text=f"Telemetry: {freq:.2f} Hz")

        if ping is None:
            self.ping_label.config(text="Ping: -- ms")
        else:
            self.ping_label.config(text=f"Ping: {ping:.0f} ms")

    def _log(self, msg: str):
        self.log.configure(state="normal")
        self.log.insert("end", msg + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")
