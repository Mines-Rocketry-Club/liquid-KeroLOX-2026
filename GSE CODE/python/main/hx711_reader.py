import sys
from pathlib import Path
from typing import Optional, Callable

HX711_DIR = Path(__file__).resolve().parent / "hx711py"
if str(HX711_DIR) not in sys.path:
    sys.path.insert(0, str(HX711_DIR))

try:
    from hx711 import HX711
except Exception as e:
    HX711 = None
    _HX711_IMPORT_ERROR = str(e)
else:
    _HX711_IMPORT_ERROR = None


class PiLoadCell:
    """
    HX711 helper.

    Scaled output is:
        (raw - tare_value) * calibration_factor
    """

    def __init__(
        self,
        name: str,
        dout_pin: int,
        pd_sck_pin: int,
        tare_value: float = 0.0,
        calibration_factor: float = 1.0,
        on_log: Optional[Callable[[str], None]] = None,
    ):
        self.name = name
        self.dout_pin = dout_pin
        self.pd_sck_pin = pd_sck_pin
        self.tare_value = float(tare_value)
        self.calibration_factor = float(calibration_factor)
        self.on_log = on_log

        self.hx = None
        self.available = False

        if HX711 is None:
            self._log(f"{self.name}: HX711 library not available: {_HX711_IMPORT_ERROR}")
            return

        try:
            self.hx = HX711(dout_pin, pd_sck_pin)
            self.hx.set_reading_format("MSB", "MSB")
            self.hx.set_reference_unit(1)
            self.hx.reset()
            self.available = True

            self._log(
                f"{self.name}: HX711 initialized on DOUT={dout_pin}, "
                f"SCK={pd_sck_pin}, tare={self.tare_value}, "
                f"cal={self.calibration_factor}"
            )
        except Exception as e:
            self.hx = None
            self.available = False
            self._log(f"{self.name}: HX711 init error: {e}")

    def _log(self, msg: str):
        if self.on_log:
            self.on_log(msg)

    def set_tare_value(self, tare_value: float):
        self.tare_value = float(tare_value)
        self._log(f"{self.name}: tare updated to {self.tare_value}")

    def set_calibration_factor(self, calibration_factor: float):
        self.calibration_factor = float(calibration_factor)
        self._log(f"{self.name}: calibration factor updated to {self.calibration_factor}")

    def read_raw(self, samples: int = 5) -> Optional[float]:
        if not self.available or self.hx is None:
            return None

        try:
            if hasattr(self.hx, "get_raw_data_mean"):
                return float(self.hx.get_raw_data_mean(samples))

            if hasattr(self.hx, "read_average"):
                return float(self.hx.read_average(samples))

            if hasattr(self.hx, "get_weight"):
                return float(self.hx.get_weight(samples))

            self._log(f"{self.name}: HX711 read method missing")
            return None

        except Exception as e:
            self._log(f"{self.name}: read error {e}")
            return None

    def raw_to_value(self, raw_value: float) -> float:
        return (float(raw_value) - self.tare_value) * self.calibration_factor

    def read_value(self, samples: int = 5) -> Optional[float]:
        raw = self.read_raw(samples=samples)
        if raw is None:
            return None
        return self.raw_to_value(raw)

    def cleanup(self):
        try:
            if self.hx is not None:
                self.hx.power_down()
        except Exception:
            pass