#!/usr/bin/env python3
"""Live PySide6 view of the HC6800-ES simulator: ports, 7-segment digits,
LCD text, and UART log, refreshed on a QTimer.

Deliberately poll-driven rather than callback-driven: sim_step() is
synchronous and returns immediately (native code, no I/O), so the Qt main
thread can just call it directly from a QTimer tick and repaint from
whatever state comes back -- no cross-thread signaling, no GIL juggling,
matching the "keep the integration tight" brief. See pysim/__init__.py and
sim/capi.h for why this project didn't reach for an async/callback API.

Usage: python sim_gui.py [hexfile] [options] -- see --help.
"""
import argparse
import math
import sys
import time

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QFileDialog,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from pysim import Simulator, Exception8051

MONO = QFont("Consolas", 11)
MONO.setStyleHint(QFont.Monospace)


class PortView(QWidget):
    """One byte's worth of bit indicators, MSB to LSB."""

    def __init__(self, label):
        super().__init__()
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(QLabel(label))
        self._bits = []
        for _ in range(8):
            bit = QLabel("0")
            bit.setFixedSize(18, 18)
            bit.setAlignment(Qt.AlignCenter)
            bit.setAutoFillBackground(True)
            layout.addWidget(bit)
            self._bits.append(bit)

    def set_value(self, value: int):
        for i in range(8):
            bit = self._bits[7 - i]
            on = bool(value & (1 << i))
            bit.setText("1" if on else "0")
            pal = bit.palette()
            pal.setColor(bit.backgroundRole(), QColor("#3a3") if on else QColor("#333"))
            pal.setColor(bit.foregroundRole(), QColor("white"))
            bit.setPalette(pal)


class ServoView(QWidget):
    """A rotating arrow over a 0-180 degree arc, plus a numeric readout --
    not an HC6800-ES peripheral (see sim/README.md's "plugin vs board
    definition" note), so this only appears when the Servo checkbox is on."""

    def __init__(self):
        super().__init__()
        self.setMinimumSize(160, 110)
        self._angle_deg = 0.0

    def set_angle_decidegrees(self, decidegrees: int):
        self._angle_deg = decidegrees / 10.0
        self.update()

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w // 2, h - 14
        radius = min(w // 2, h) - 20

        painter.setPen(QPen(QColor("#666"), 2))
        painter.drawArc(cx - radius, cy - radius, radius * 2, radius * 2, 0, 180 * 16)

        theta = math.radians(180 - self._angle_deg)  # 0deg points right, 180deg points left
        x2 = cx + radius * math.cos(theta)
        y2 = cy - radius * math.sin(theta)
        painter.setPen(QPen(QColor("#e33"), 3))
        painter.drawLine(cx, cy, int(x2), int(y2))

        painter.setBrush(QColor("#444"))
        painter.setPen(QPen(QColor("#888"), 1))
        painter.drawEllipse(cx - 8, cy - 8, 16, 16)

        painter.setPen(QColor("white"))
        painter.drawText(0, 4, w, 16, Qt.AlignCenter, f"{self._angle_deg:.0f}°")


class FanView(QWidget):
    """A spinning 4-blade glyph whose rotation rate reflects RPM, plus a
    numeric readout -- not an HC6800-ES peripheral (see sim/README.md's
    "plugin vs board definition" note), so this only appears when the Fan
    checkbox is on. The spin rate is visually proportional to RPM, not
    frame-accurate to any real elapsed time (see sim/devices/fan.h for why
    RPM itself is already a simplified model, not a real fan curve)."""

    def __init__(self):
        super().__init__()
        self.setMinimumSize(140, 140)
        self._rpm = 0
        self._blade_angle = 0.0

    def set_rpm(self, rpm: int):
        self._rpm = rpm
        self._blade_angle = (self._blade_angle + rpm / 60.0) % 360.0
        self.update()

    def paintEvent(self, _event):
        painter = QPainter(self)
        painter.setRenderHint(QPainter.Antialiasing)
        w, h = self.width(), self.height()
        cx, cy = w // 2, (h - 20) // 2
        radius = min(w, h - 20) // 2 - 10

        painter.translate(cx, cy)
        painter.rotate(self._blade_angle)
        painter.setBrush(QColor("#37a"))
        painter.setPen(QPen(QColor("#8bd"), 1))
        for i in range(4):
            painter.save()
            painter.rotate(i * 90)
            painter.drawEllipse(-radius // 4, -radius, radius // 2, radius)
            painter.restore()
        painter.resetTransform()

        painter.setBrush(QColor("#333"))
        painter.setPen(QPen(QColor("#888"), 1))
        painter.drawEllipse(cx - 6, cy - 6, 12, 12)

        painter.setPen(QColor("white"))
        painter.drawText(0, h - 18, w, 18, Qt.AlignCenter, f"{self._rpm} RPM")


class MainWindow(QMainWindow):
    def __init__(self, args):
        super().__init__()
        self.setWindowTitle("emu8051 sim - HC6800-ES")
        self.sim = None
        self._digit_count = args.digits if args.digits else 8
        self._clock_hz = args.clock_hz
        self._last_wall_time = None
        self._last_enc28j60_buffer_bytes = 0

        # External peripherals -- not part of the HC6800-ES board, so
        # (unlike enable_lcd()/enable_ds1302()/etc) there's no board
        # constant to default to. --servo/--enc28j60 pick a pin; absent
        # that, default to whichever pin this project's own ported demos
        # happen to assume (05_enc_servo.hex's P3.7, 09_ethernet.hex's
        # P0.0-P0.3) since checking the box with nothing else configured
        # is the common case of "I'm looking at one of those two".
        self._servo_pin = tuple(args.servo[:2]) if args.servo else (3, 7)
        self._servo_range = tuple(args.servo[2:]) if args.servo and len(args.servo) > 2 else (1000, 2000)
        self._enc28j60_pins = args.enc28j60 if args.enc28j60 else (0, 3, 0, 2, 0, 0, 0, 1)
        self._fan_pins = args.fan if args.fan else (1, 6, 1, 7)

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)

        # --- controls ---
        controls = QHBoxLayout()
        open_btn = QPushButton("Open .hex...")
        open_btn.clicked.connect(self.open_hex)
        controls.addWidget(open_btn)

        self.run_btn = QPushButton("Run")
        self.run_btn.setCheckable(True)
        self.run_btn.toggled.connect(self.toggle_run)
        controls.addWidget(self.run_btn)

        step_btn = QPushButton("Step 10k instr")
        step_btn.clicked.connect(lambda: self.advance(10_000))
        controls.addWidget(step_btn)

        reset_btn = QPushButton("Reset")
        reset_btn.clicked.connect(self.reset_sim)
        controls.addWidget(reset_btn)

        controls.addWidget(QLabel("instr/tick:"))
        self.instr_per_tick = QSpinBox()
        self.instr_per_tick.setRange(100, 5_000_000)
        self.instr_per_tick.setValue(args.instr_per_tick)
        self.instr_per_tick.setSingleStep(10_000)
        controls.addWidget(self.instr_per_tick)

        self.cb_realtime = QCheckBox("Real-time")
        self.cb_realtime.setToolTip(
            "Paces execution against the board's own oscillator (clock_hz/12 "
            "ticks per second) instead of a fixed instruction count per "
            "refresh, so on-screen changes track wall-clock time -- e.g. the "
            "DS1302 demos' seconds display actually takes a real second per "
            "tick. 'instr/tick' above is ignored while this is checked."
        )
        self.cb_realtime.toggled.connect(self.instr_per_tick.setDisabled)
        self.cb_realtime.toggled.connect(self._realtime_toggled)
        controls.addWidget(self.cb_realtime)

        self.status_label = QLabel("no file loaded")
        controls.addWidget(self.status_label)
        controls.addStretch(1)
        root.addLayout(controls)

        # --- peripheral enable checkboxes ---
        peri = QHBoxLayout()
        self.cb_lcd = QCheckBox("LCD")
        self.cb_digits = QCheckBox(f"7-segment ({self._digit_count} digit)")
        self.cb_ds1302 = QCheckBox("DS1302")
        self.cb_adc = QCheckBox("XPT2046 ADC")
        self.cb_servo = QCheckBox(f"Servo (P{self._servo_pin[0]}.{self._servo_pin[1]})")
        self.cb_servo.setToolTip("Not an HC6800-ES peripheral -- an assumed pin, see --servo.")
        self.cb_enc28j60 = QCheckBox("ENC28J60")
        self.cb_enc28j60.setToolTip("Not an HC6800-ES peripheral -- assumed pins, see --enc28j60.")
        self.cb_fan = QCheckBox(f"Fan (P{self._fan_pins[0]}.{self._fan_pins[1]}/P{self._fan_pins[2]}.{self._fan_pins[3]})")
        self.cb_fan.setToolTip("Not an HC6800-ES peripheral -- assumed pins, see --fan.")
        for cb in (self.cb_lcd, self.cb_digits, self.cb_ds1302, self.cb_adc, self.cb_servo, self.cb_enc28j60, self.cb_fan):
            cb.toggled.connect(self.apply_peripherals)
            peri.addWidget(cb)
        peri.addStretch(1)
        root.addLayout(peri)

        # --- ports ---
        port_box = QGroupBox("Ports")
        port_layout = QVBoxLayout(port_box)
        self.ports = [PortView(f"P{i}") for i in range(4)]
        for p in self.ports:
            port_layout.addWidget(p)
        root.addWidget(port_box)

        # --- digits + LCD ---
        mid = QHBoxLayout()

        self.digit_box = QGroupBox("7-segment")
        digit_layout = QHBoxLayout(self.digit_box)
        self.digit_label = QLabel("--------")
        self.digit_label.setFont(QFont("Consolas", 24))
        digit_layout.addWidget(self.digit_label)
        mid.addWidget(self.digit_box)

        self.lcd_box = QGroupBox("LCD (1602)")
        lcd_layout = QVBoxLayout(self.lcd_box)
        self.lcd_lines = [QLabel(" " * 16), QLabel(" " * 16)]
        for lbl in self.lcd_lines:
            lbl.setFont(MONO)
            lbl.setStyleSheet("background:#042; color:#9f9; padding:4px;")
            lcd_layout.addWidget(lbl)
        mid.addWidget(self.lcd_box)

        self.servo_box = QGroupBox("Servo")
        servo_layout = QVBoxLayout(self.servo_box)
        self.servo_view = ServoView()
        servo_layout.addWidget(self.servo_view)
        self.servo_pulse_label = QLabel("pulse: -- us")
        self.servo_pulse_label.setAlignment(Qt.AlignCenter)
        servo_layout.addWidget(self.servo_pulse_label)
        mid.addWidget(self.servo_box)

        self.enc28j60_box = QGroupBox("ENC28J60 (SPI protocol only)")
        enc_layout = QVBoxLayout(self.enc28j60_box)
        self.enc28j60_opcode_label = QLabel("opcode: --")
        self.enc28j60_bank_label = QLabel("bank: --")
        self.enc28j60_buffer_label = QLabel("buffer bytes: --")
        self.enc28j60_activity_label = QLabel("●")  # dot, flashes green on buffer activity
        self.enc28j60_activity_label.setAlignment(Qt.AlignCenter)
        for lbl in (self.enc28j60_opcode_label, self.enc28j60_bank_label, self.enc28j60_buffer_label):
            lbl.setFont(MONO)
            enc_layout.addWidget(lbl)
        enc_layout.addWidget(self.enc28j60_activity_label)
        mid.addWidget(self.enc28j60_box)

        self.fan_box = QGroupBox("Fan")
        fan_layout = QVBoxLayout(self.fan_box)
        self.fan_view = FanView()
        fan_layout.addWidget(self.fan_view)
        self.fan_duty_label = QLabel("duty: -- %")
        self.fan_duty_label.setAlignment(Qt.AlignCenter)
        fan_layout.addWidget(self.fan_duty_label)
        mid.addWidget(self.fan_box)

        root.addLayout(mid)

        # Panels for a peripheral that isn't enabled get grayed out rather
        # than just showing blank content -- otherwise "not enabled" and
        # "enabled but nothing captured yet" look identical.
        self.digit_box.setEnabled(self.cb_digits.isChecked())
        self.lcd_box.setEnabled(self.cb_lcd.isChecked())
        self.servo_box.setEnabled(self.cb_servo.isChecked())
        self.enc28j60_box.setEnabled(self.cb_enc28j60.isChecked())
        self.fan_box.setEnabled(self.cb_fan.isChecked())
        self.cb_digits.toggled.connect(self.digit_box.setEnabled)
        self.cb_lcd.toggled.connect(self.lcd_box.setEnabled)
        self.cb_servo.toggled.connect(self.servo_box.setEnabled)
        self.cb_enc28j60.toggled.connect(self.enc28j60_box.setEnabled)
        self.cb_fan.toggled.connect(self.fan_box.setEnabled)

        # --- UART log ---
        uart_box = QGroupBox("UART TX log")
        uart_layout = QVBoxLayout(uart_box)
        self.uart_log = QPlainTextEdit()
        self.uart_log.setReadOnly(True)
        self.uart_log.setFont(MONO)
        self.uart_log.setMaximumBlockCount(200)
        uart_layout.addWidget(self.uart_log)
        root.addWidget(uart_box)
        self._uart_seen = 0

        self.timer = QTimer(self)
        self.timer.setInterval(args.interval_ms)
        self.timer.timeout.connect(self.on_tick)

        self.cb_lcd.setChecked(args.lcd)
        self.cb_digits.setChecked(args.digits is not None)
        self.cb_ds1302.setChecked(args.ds1302)
        self.cb_adc.setChecked(args.adc)
        self.cb_servo.setChecked(args.servo is not None)
        self.cb_enc28j60.setChecked(args.enc28j60 is not None)
        self.cb_fan.setChecked(args.fan is not None)
        self.cb_realtime.setChecked(args.realtime)

        if args.hexfile:
            self.load(args.hexfile)

    def open_hex(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open Intel HEX", "", "Hex files (*.hex);;All files (*)")
        if path:
            self.load(path)

    def load(self, path):
        if self.sim:
            self.sim.close()
        self.sim = Simulator("hc6800_es", path)
        if self._clock_hz:
            self.sim.set_clock_hz(self._clock_hz)
        self._uart_seen = 0
        self.uart_log.clear()
        self.apply_peripherals()
        self.status_label.setText(path)
        self.refresh()

    def apply_peripherals(self):
        if not self.sim:
            return
        if self.cb_lcd.isChecked():
            self.sim.enable_lcd()
        if self.cb_digits.isChecked():
            self.sim.enable_digit_display(self._digit_count)
        if self.cb_ds1302.isChecked():
            self.sim.enable_ds1302()
        if self.cb_adc.isChecked():
            self.sim.enable_xpt2046()
        if self.cb_servo.isChecked():
            self.sim.enable_servo(*self._servo_pin, *self._servo_range)
        if self.cb_enc28j60.isChecked():
            cs, sck, mosi, miso = self._enc28j60_pins[0:2], self._enc28j60_pins[2:4], \
                self._enc28j60_pins[4:6], self._enc28j60_pins[6:8]
            self.sim.enable_enc28j60(cs, sck, mosi, miso)
        if self.cb_fan.isChecked():
            pwm = self._fan_pins[0:2]
            tach = self._fan_pins[2:4]
            self.sim.enable_fan(pwm, tach)

    def reset_sim(self):
        if not self.sim:
            return
        self.sim.reset()
        self._uart_seen = 0
        self.uart_log.clear()
        self._last_wall_time = time.perf_counter()
        self.refresh()

    def toggle_run(self, checked):
        if checked:
            self.run_btn.setText("Pause")
            self._last_wall_time = time.perf_counter()
            self.timer.start()
        else:
            self.run_btn.setText("Run")
            self.timer.stop()

    def _realtime_toggled(self, _checked):
        # Reset the pacing reference point whenever the mode changes while
        # already running, so the next tick doesn't see a stale delta from
        # before the switch and try to "catch up" with a huge step.
        if self.run_btn.isChecked():
            self._last_wall_time = time.perf_counter()

    def on_tick(self):
        if self.cb_realtime.isChecked():
            self.advance_realtime()
        else:
            self.advance(self.instr_per_tick.value())

    def advance(self, count):
        if not self.sim:
            return
        self.sim.step_instructions(count)
        self.refresh()

    def advance_realtime(self):
        if not self.sim:
            return
        now = time.perf_counter()
        elapsed = 0.0 if self._last_wall_time is None else (now - self._last_wall_time)
        self._last_wall_time = now
        # clock_hz/12: tick() is one machine cycle (12 oscillator periods),
        # see sim/devices/ds1302.c's own derivation of this from
        # hd44780.c's busy-timing constants.
        ticks = max(1, round(elapsed * (self.sim.clock_hz // 12)))
        self.sim.step(ticks)
        self.refresh()

    def refresh(self):
        if not self.sim:
            return
        for i, p in enumerate(self.ports):
            p.set_value(self.sim.port(i))

        if self.cb_digits.isChecked():
            self.digit_label.setText(self.sim.digits_text())

        if self.cb_lcd.isChecked():
            self.lcd_lines[0].setText(self.sim.lcd_line(0) or " " * 16)
            self.lcd_lines[1].setText(self.sim.lcd_line(1) or " " * 16)

        if self.cb_servo.isChecked():
            self.servo_view.set_angle_decidegrees(self.sim.servo_angle_decidegrees())
            self.servo_pulse_label.setText(f"pulse: {self.sim.servo_pulse_us()} us")

        if self.cb_enc28j60.isChecked():
            self.enc28j60_opcode_label.setText(f"opcode: 0x{self.sim.enc28j60_last_opcode():02x}")
            self.enc28j60_bank_label.setText(f"bank: {self.sim.enc28j60_bank()}")
            buffer_bytes = self.sim.enc28j60_buffer_byte_count()
            self.enc28j60_buffer_label.setText(f"buffer bytes: {buffer_bytes}")
            active = buffer_bytes != self._last_enc28j60_buffer_bytes
            self._last_enc28j60_buffer_bytes = buffer_bytes
            self.enc28j60_activity_label.setStyleSheet(f"color: {'#3f3' if active else '#333'}")

        if self.cb_fan.isChecked():
            self.fan_view.set_rpm(self.sim.fan_rpm())
            self.fan_duty_label.setText(f"duty: {self.sim.fan_duty_percent()} %")

        tx = self.sim.uart_tx_bytes()
        if len(tx) > self._uart_seen:
            new = tx[self._uart_seen :]
            self._uart_seen = len(tx)
            self.uart_log.appendPlainText(" ".join(f"0x{b:02x}" for b in new))

        excs = self.sim.exceptions()
        if excs:
            self.uart_log.appendPlainText(f"[exception] {', '.join(excs)}")


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("hexfile", nargs="?", default=None, help="Intel HEX file to load at startup")
    parser.add_argument(
        "--instr-per-tick", type=int, default=50_000, metavar="N",
        help="instructions to run per GUI refresh in fixed-rate mode (default: %(default)s)",
    )
    parser.add_argument(
        "--interval-ms", type=int, default=30, metavar="N",
        help="GUI refresh interval in milliseconds (default: %(default)s)",
    )
    parser.add_argument(
        "--realtime", action="store_true",
        help="start paced against the board's own clock instead of a fixed instruction count per refresh",
    )
    parser.add_argument(
        "--clock-hz", type=int, default=None, metavar="HZ",
        help="override the board's oscillator frequency (default: hc6800_es's stock 12000000; "
             "still assumes a classic 12-clocks-per-machine-cycle core)",
    )
    parser.add_argument("--lcd", action="store_true", help="enable the LCD panel at startup")
    parser.add_argument(
        "--digits", type=int, nargs="?", const=8, default=None, metavar="N",
        help="enable the 7-segment display at startup (default 8 digits if N omitted)",
    )
    parser.add_argument("--ds1302", action="store_true", help="enable the DS1302 RTC at startup")
    parser.add_argument("--adc", action="store_true", help="enable the XPT2046 ADC at startup")

    def pin_list(count):
        def parse(s):
            fields = tuple(int(x) for x in s.split(","))
            if len(fields) not in count:
                raise argparse.ArgumentTypeError(f"expected {' or '.join(map(str, count))} comma-separated ints")
            return fields
        return parse

    parser.add_argument(
        "--servo", type=pin_list((2, 4)), default=None, metavar="PORT,BIT[,MIN_US,MAX_US]",
        help="enable the servo at startup on this PWM input pin -- not an HC6800-ES peripheral, "
             "see sim/README.md (default if the box is checked with no --servo: P3.7, 1000-2000us, "
             "matching the sibling demo repo's 05_enc_servo.hex)",
    )
    parser.add_argument(
        "--enc28j60", type=pin_list((8,)), default=None,
        metavar="CS_P,CS_B,SCK_P,SCK_B,MOSI_P,MOSI_B,MISO_P,MISO_B",
        help="enable the ENC28J60 at startup on these SPI pins -- not an HC6800-ES peripheral, "
             "see sim/README.md (default if the box is checked with no --enc28j60: P0.3/P0.2/P0.0/P0.1, "
             "matching the sibling demo repo's 09_ethernet.hex)",
    )
    parser.add_argument(
        "--fan", type=pin_list((4,)), default=None, metavar="PWM_P,PWM_B,TACH_P,TACH_B",
        help="enable the fan at startup on these PWM/tach pins -- not an HC6800-ES peripheral, "
             "see sim/README.md (default if the box is checked with no --fan: P1.6/P1.7, "
             "matching the sibling demo repo's 06_fan_tach.hex)",
    )
    return parser.parse_args(argv)


def main():
    args = parse_args(sys.argv[1:])
    app = QApplication(sys.argv[:1])
    win = MainWindow(args)
    win.resize(720, 640)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
