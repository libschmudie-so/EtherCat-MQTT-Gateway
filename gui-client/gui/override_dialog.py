"""Dialog for generating a --pdo-config override snippet for a device."""

import json
import logging
from typing import List, Optional

from PyQt5.QtWidgets import (
    QDialog, QVBoxLayout, QHBoxLayout, QGroupBox, QLabel, QListWidget,
    QListWidgetItem, QPlainTextEdit, QPushButton, QFileDialog, QMessageBox,
    QApplication, QDialogButtonBox
)
from PyQt5.QtCore import Qt

from gui.models import Device, PdoOption

logger = logging.getLogger(__name__)


class GenerateOverrideDialog(QDialog):
    """Lets the user pick a RxPdo/TxPdo mapping for one device (by ESI name
    or index -- see AvailablePdo in the gateway) and generates the matching
    --pdo-config JSON snippet, ready to copy or save.

    Most terminals only ever have one PDO active per direction (the options
    within a direction are typically mutually exclusive on the wire -- e.g.
    an EL3012's "AI Standard Channel 1" vs "AI Compact Channel 1"), but the
    picker allows multiple, since --pdo-config itself accepts a list per
    direction and some slaves do combine several.
    """

    def __init__(self, device: Device, parent=None):
        super().__init__(parent)
        self.device = device
        self.setWindowTitle(f"Generate PDO Override -- {device.name}")
        self.resize(640, 560)
        self._build_ui()
        self._regenerate()

    def _build_ui(self):
        layout = QVBoxLayout(self)

        identity_box = QGroupBox("Device")
        identity_layout = QVBoxLayout(identity_box)
        identity_layout.addWidget(QLabel(f"<b>{self.device.name}</b> -- {self.device.description}"))
        id_line = (
            f"vendorId: {self.device.vendor_id or '(unknown -- device metadata not received yet)'}    "
            f"productCode: {self.device.product_code or '?'}    "
            f"revisionNo: {self.device.revision_no or '?'}"
        )
        identity_layout.addWidget(QLabel(id_line))
        layout.addWidget(identity_box)

        lists_layout = QHBoxLayout()
        self.rx_list = self._make_pdo_list(self.device.available_rx_pdos)
        self.tx_list = self._make_pdo_list(self.device.available_tx_pdos)
        lists_layout.addWidget(self._wrap_list("rxPdo (Output)", self.rx_list))
        lists_layout.addWidget(self._wrap_list("txPdo (Input)", self.tx_list))
        layout.addLayout(lists_layout)

        if not self.device.available_rx_pdos and not self.device.available_tx_pdos:
            layout.addWidget(QLabel(
                "No ESI-declared PDO options known for this device yet -- either its ESI file isn't in the "
                "gateway's --esi directory, or its metadata hasn't arrived. You can still save/copy an empty "
                "override and fill in rxPdo/txPdo by hand."
            ))

        layout.addWidget(QLabel("Generated --pdo-config snippet:"))
        self.output = QPlainTextEdit()
        self.output.setReadOnly(True)
        self.output.setFont(self._mono_font())
        layout.addWidget(self.output)

        button_row = QHBoxLayout()
        copy_btn = QPushButton("Copy to Clipboard")
        copy_btn.clicked.connect(self._copy_to_clipboard)
        save_btn = QPushButton("Save As New File...")
        save_btn.clicked.connect(self._save_as)
        append_btn = QPushButton("Append to Existing File...")
        append_btn.clicked.connect(self._append_to_file)
        button_row.addWidget(copy_btn)
        button_row.addWidget(save_btn)
        button_row.addWidget(append_btn)
        button_row.addStretch()
        layout.addLayout(button_row)

        close_buttons = QDialogButtonBox(QDialogButtonBox.Close)
        close_buttons.rejected.connect(self.reject)
        close_buttons.accepted.connect(self.accept)
        layout.addWidget(close_buttons)

    def _make_pdo_list(self, options: List[PdoOption]) -> QListWidget:
        listbox = QListWidget()
        for opt in options:
            item = QListWidgetItem(opt.label())
            item.setData(Qt.UserRole, opt.selector())
            item.setFlags(item.flags() | Qt.ItemIsUserCheckable)
            item.setCheckState(Qt.Unchecked)
            listbox.addItem(item)
        listbox.itemChanged.connect(lambda _: self._regenerate())
        return listbox

    @staticmethod
    def _wrap_list(title: str, listbox: QListWidget) -> QGroupBox:
        box = QGroupBox(title)
        box_layout = QVBoxLayout(box)
        box_layout.addWidget(listbox)
        return box

    @staticmethod
    def _mono_font():
        from PyQt5.QtGui import QFont
        font = QFont("monospace")
        font.setStyleHint(QFont.Monospace)
        return font

    def _selected_selectors(self, listbox: QListWidget) -> List[str]:
        result = []
        for i in range(listbox.count()):
            item = listbox.item(i)
            if item.checkState() == Qt.Checked:
                result.append(item.data(Qt.UserRole))
        return result

    def _build_override(self) -> dict:
        override = {
            "vendorId": self.device.vendor_id or "0x0",
            "productCode": self.device.product_code or "0x0",
            "revisionNo": self.device.revision_no or "0x0",
        }
        rx = self._selected_selectors(self.rx_list)
        tx = self._selected_selectors(self.tx_list)
        if rx:
            override["rxPdo"] = rx
        if tx:
            override["txPdo"] = tx
        return override

    def _regenerate(self):
        doc = {"overrides": [self._build_override()]}
        self.output.setPlainText(json.dumps(doc, indent=2))

    def _copy_to_clipboard(self):
        QApplication.clipboard().setText(self.output.toPlainText())

    def _save_as(self):
        path, _ = QFileDialog.getSaveFileName(self, "Save --pdo-config file", "pdo-config.json",
                                               "JSON files (*.json)")
        if not path:
            return
        try:
            with open(path, "w") as f:
                f.write(self.output.toPlainText())
        except OSError as ex:
            QMessageBox.warning(self, "Save failed", str(ex))
            return
        QMessageBox.information(self, "Saved", f"Wrote {path}")

    def _append_to_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "Append to --pdo-config file", "", "JSON files (*.json)")
        if not path:
            return
        try:
            with open(path, "r") as f:
                doc = json.load(f)
        except (OSError, json.JSONDecodeError) as ex:
            QMessageBox.warning(self, "Append failed", f"Couldn't read/parse {path}: {ex}")
            return

        doc.setdefault("overrides", [])
        new_override = self._build_override()
        # Replace any existing override for the same device rather than
        # duplicating it -- vendorId/productCode/revisionNo together are
        # the override's identity, same as the gateway matches on.
        doc["overrides"] = [
            ov for ov in doc["overrides"]
            if not (ov.get("vendorId") == new_override["vendorId"]
                    and ov.get("productCode") == new_override["productCode"]
                    and ov.get("revisionNo") == new_override["revisionNo"])
        ]
        doc["overrides"].append(new_override)

        try:
            with open(path, "w") as f:
                json.dump(doc, f, indent=2)
        except OSError as ex:
            QMessageBox.warning(self, "Append failed", str(ex))
            return
        QMessageBox.information(self, "Saved", f"Updated {path} ({len(doc['overrides'])} override(s))")
