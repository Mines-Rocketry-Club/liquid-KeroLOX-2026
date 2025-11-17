# main.py
from gui import Dashboard
from comms import ArduinoComms


def main():
    gui = Dashboard()

    comms = ArduinoComms(
        on_log=gui.log_from_thread,
        on_state=gui.apply_state_from_thread,
        on_metric=gui.apply_metric_from_thread,
        on_message=gui.show_message_from_thread,
        on_prompt=lambda pid, q: gui.show_prompt_from_thread(pid, q),
    )

    # Wire GUI → Comms
    gui.on_state_request = comms.request_state
    gui.on_prompt_answer = comms.send_answer

    # Start telemetry polling at 2 Hz, skip around control commands
    gui.start_telemetry_loop(comms, interval_ms=500, guard_ms=300)

    def on_close():
        comms.close()
        gui.destroy()

    gui.protocol("WM_DELETE_WINDOW", on_close)
    gui.mainloop()


if __name__ == "__main__":
    main()
