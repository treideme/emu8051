#!/usr/bin/env python3
"""Live PySide6 view of the HC6800-ES simulator: ports, 7-segment digits,
LCD text, and UART log, refreshed on a QTimer.

Deliberately poll-driven rather than callback-driven: sim_step() is
synchronous and returns immediately (native code, no I/O), so the Qt main
thread can just call it directly from a QTimer tick and repaint from
whatever state comes back -- no cross-thread signaling, no GIL juggling,
matching the "keep the integration tight" brief. See pysim/__init__.py and
sim/capi.h for why this project didn't reach for an async/callback API.

Usage: python sim_gui.py [hexfile]
"""
import sys

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QColor, QFont
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


class MainWindow(QMainWindow):
    def __init__(self, hexfile=None):
        super().__init__()
        self.setWindowTitle("emu8051 sim - HC6800-ES")
        self.sim = None

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

        controls.addWidget(QLabel("instr/tick:"))
        self.instr_per_tick = QSpinBox()
        self.instr_per_tick.setRange(100, 5_000_000)
        self.instr_per_tick.setValue(50_000)
        self.instr_per_tick.setSingleStep(10_000)
        controls.addWidget(self.instr_per_tick)

        self.status_label = QLabel("no file loaded")
        controls.addWidget(self.status_label)
        controls.addStretch(1)
        root.addLayout(controls)

        # --- peripheral enable checkboxes ---
        peri = QHBoxLayout()
        self.cb_lcd = QCheckBox("LCD")
        self.cb_digits = QCheckBox("7-segment (8 digit)")
        self.cb_ds1302 = QCheckBox("DS1302")
        self.cb_adc = QCheckBox("XPT2046 ADC")
        for cb in (self.cb_lcd, self.cb_digits, self.cb_ds1302, self.cb_adc):
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

        digit_box = QGroupBox("7-segment")
        digit_layout = QHBoxLayout(digit_box)
        self.digit_label = QLabel("--------")
        self.digit_label.setFont(QFont("Consolas", 24))
        digit_layout.addWidget(self.digit_label)
        mid.addWidget(digit_box)

        lcd_box = QGroupBox("LCD (1602)")
        lcd_layout = QVBoxLayout(lcd_box)
        self.lcd_lines = [QLabel(" " * 16), QLabel(" " * 16)]
        for lbl in self.lcd_lines:
            lbl.setFont(MONO)
            lbl.setStyleSheet("background:#042; color:#9f9; padding:4px;")
            lcd_layout.addWidget(lbl)
        mid.addWidget(lcd_box)

        root.addLayout(mid)

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
        self.timer.setInterval(30)
        self.timer.timeout.connect(self.on_tick)

        if hexfile:
            self.load(hexfile)

    def open_hex(self):
        path, _ = QFileDialog.getOpenFileName(self, "Open Intel HEX", "", "Hex files (*.hex);;All files (*)")
        if path:
            self.load(path)

    def load(self, path):
        if self.sim:
            self.sim.close()
        self.sim = Simulator("hc6800_es", path)
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
            self.sim.enable_digit_display(8)
        if self.cb_ds1302.isChecked():
            self.sim.enable_ds1302()
        if self.cb_adc.isChecked():
            self.sim.enable_xpt2046()

    def toggle_run(self, checked):
        if checked:
            self.run_btn.setText("Pause")
            self.timer.start()
        else:
            self.run_btn.setText("Run")
            self.timer.stop()

    def on_tick(self):
        self.advance(self.instr_per_tick.value())

    def advance(self, count):
        if not self.sim:
            return
        self.sim.step_instructions(count)
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

        tx = self.sim.uart_tx_bytes()
        if len(tx) > self._uart_seen:
            new = tx[self._uart_seen :]
            self._uart_seen = len(tx)
            self.uart_log.appendPlainText(" ".join(f"0x{b:02x}" for b in new))

        excs = self.sim.exceptions()
        if excs:
            self.uart_log.appendPlainText(f"[exception] {', '.join(excs)}")


def main():
    app = QApplication(sys.argv)
    win = MainWindow(sys.argv[1] if len(sys.argv) > 1 else None)
    win.resize(720, 640)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
