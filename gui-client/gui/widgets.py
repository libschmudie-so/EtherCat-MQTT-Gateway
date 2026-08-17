"""Custom widgets for the ECGW GUI."""

from PyQt5.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QLineEdit, 
    QSpinBox, QDoubleSpinBox, QCheckBox, QPushButton, QComboBox
)
from PyQt5.QtCore import Qt, pyqtSignal
import re


class SignalValueWidget(QWidget):
    """Widget for displaying and editing a signal value."""
    
    value_changed = pyqtSignal(str)  # Emits the new value as string
    
    def __init__(self, data_type: str, current_value: str = "", read_only: bool = True, parent=None):
        super().__init__(parent)
        self.data_type = data_type.lower()
        self.read_only = read_only
        self.bit_checkboxes = []  # For BITn types
        self.setup_ui(current_value)
    
    def setup_ui(self, current_value: str):
        """Setup the appropriate input widget based on data type."""
        layout = QHBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        
        # Check for BITn pattern (e.g., BIT8, BIT16)
        bit_match = re.match(r"bit(\d+)", self.data_type)
        
        if bit_match:
            # Bitfield with n bits
            num_bits = int(bit_match.group(1))
            self.setup_bitfield(layout, current_value, num_bits)
        
        elif self.data_type == "boolean":
            self.input_widget = QCheckBox()
            self.input_widget.setChecked(current_value.lower() == "true")
            self.input_widget.setEnabled(not self.read_only)
            if not self.read_only:
                self.input_widget.toggled.connect(self._on_value_changed)
            layout.addWidget(self.input_widget)
        
        elif self.data_type in ["integer", "int", "int8", "int16", "int32", "int64"]:
            self.input_widget = QSpinBox()
            self.input_widget.setMinimum(-2147483648)
            self.input_widget.setMaximum(2147483647)
            self.input_widget.setReadOnly(self.read_only)
            self.input_widget.setEnabled(not self.read_only)
            try:
                self.input_widget.setValue(int(current_value) if current_value else 0)
            except ValueError:
                self.input_widget.setValue(0)
            if not self.read_only:
                self.input_widget.valueChanged.connect(self._on_value_changed)
            layout.addWidget(self.input_widget)
        
        elif self.data_type in ["float", "double", "real"]:
            self.input_widget = QDoubleSpinBox()
            self.input_widget.setDecimals(4)
            self.input_widget.setMinimum(-1e10)
            self.input_widget.setMaximum(1e10)
            self.input_widget.setReadOnly(self.read_only)
            self.input_widget.setEnabled(not self.read_only)
            try:
                self.input_widget.setValue(float(current_value) if current_value else 0.0)
            except ValueError:
                self.input_widget.setValue(0.0)
            if not self.read_only:
                self.input_widget.valueChanged.connect(self._on_value_changed)
            layout.addWidget(self.input_widget)
        
        else:
            # Default to text input for string types
            self.input_widget = QLineEdit()
            self.input_widget.setText(current_value)
            self.input_widget.setReadOnly(self.read_only)
            if not self.read_only:
                self.input_widget.editingFinished.connect(self._on_value_changed)
            layout.addWidget(self.input_widget)
    
    def setup_bitfield(self, layout: QHBoxLayout, current_value: str, num_bits: int):
        """Setup checkboxes for a bitfield."""
        # Parse current value as integer
        try:
            value_int = int(current_value) if current_value else 0
        except ValueError:
            value_int = 0
        
        # Set minimal spacing
        layout.setSpacing(2)
        
        # Create checkboxes for each bit
        for bit_idx in range(num_bits):
            checkbox = QCheckBox()
            checkbox.setToolTip(f"Bit {bit_idx}")
            # Check if this bit is set
            is_set = (value_int >> bit_idx) & 1
            checkbox.setChecked(is_set)
            checkbox.setEnabled(not self.read_only)
            if not self.read_only:
                checkbox.toggled.connect(self._on_value_changed)
            self.bit_checkboxes.append(checkbox)
            layout.addWidget(checkbox)
        
        layout.addStretch()
    
    def _on_value_changed(self):
        """Handle value changes."""
        value = self.get_value()
        self.value_changed.emit(value)
    
    def get_value(self) -> str:
        """Get the current value as a string."""
        if self.bit_checkboxes:
            # Reconstruct integer from bitfield
            value_int = 0
            for bit_idx, checkbox in enumerate(self.bit_checkboxes):
                if checkbox.isChecked():
                    value_int |= (1 << bit_idx)
            return str(value_int)
        elif self.data_type == "boolean":
            return str(self.input_widget.isChecked()).lower()
        elif isinstance(self.input_widget, (QSpinBox, QDoubleSpinBox)):
            return str(self.input_widget.value())
        else:
            return self.input_widget.text()
    
    def set_value(self, value: str):
        """Set the widget value from a string."""
        if self.bit_checkboxes:
            # Set bitfield from integer
            try:
                value_int = int(value)
                for bit_idx, checkbox in enumerate(self.bit_checkboxes):
                    checkbox.setChecked((value_int >> bit_idx) & 1)
            except ValueError:
                pass
        elif self.data_type == "boolean":
            self.input_widget.setChecked(value.lower() == "true")
        elif isinstance(self.input_widget, QSpinBox):
            try:
                self.input_widget.setValue(int(value))
            except ValueError:
                pass
        elif isinstance(self.input_widget, QDoubleSpinBox):
            try:
                self.input_widget.setValue(float(value))
            except ValueError:
                pass
        else:
            self.input_widget.setText(value)


class StatusIndicator(QLabel):
    """Simple status indicator label."""
    
    def __init__(self, text: str = "", parent=None):
        super().__init__(text, parent)
        self.setStyleSheet("padding: 5px;")
    
    def set_status(self, connected: bool):
        """Update the status indicator."""
        if connected:
            self.setText("● Connected")
            self.setStyleSheet("color: green; padding: 5px;")
        else:
            self.setText("● Disconnected")
            self.setStyleSheet("color: red; padding: 5px;")
