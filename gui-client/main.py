#!/usr/bin/env python3
"""Main entry point for the ECGW GUI application."""

import sys
import logging
from PyQt5.QtWidgets import QApplication
from gui.main_window import MainWindow

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s - %(name)s - %(levelname)s - %(message)s'
)

if __name__ == "__main__":
    app = QApplication(sys.argv)
    window = MainWindow()
    window.show()
    sys.exit(app.exec_())
