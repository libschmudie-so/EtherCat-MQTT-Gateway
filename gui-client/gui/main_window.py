"""Main window for the ECGW GUI application."""

import logging
from typing import Optional, Dict, Tuple
from PyQt5.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QTreeWidget, QTreeWidgetItem,
    QHeaderView, QLabel, QPushButton, QLineEdit, QStatusBar, QMessageBox, QMenu
)
from PyQt5.QtCore import Qt, pyqtSlot, QTimer
from PyQt5.QtGui import QFont, QColor

from mqtt_connection import MQTTConnection
from gui.models import Bridge, Device, Signal
from gui.widgets import SignalValueWidget, StatusIndicator
from gui.override_dialog import GenerateOverrideDialog
import config

logger = logging.getLogger(__name__)


class MainWindow(QMainWindow):
    """Main application window."""
    
    def __init__(self):
        super().__init__()
        self.setWindowTitle(config.WINDOW_TITLE)
        self.setGeometry(100, 100, config.WINDOW_WIDTH, config.WINDOW_HEIGHT)
        
        # Data model
        self.bridge = Bridge()
        self.signal_widgets: Dict[Tuple[int, str, str], SignalValueWidget] = {}
        self.last_device_ids = set()  # Track previous device set for hot-plug detection
        
        # MQTT connection
        self.mqtt = MQTTConnection(
            config.MQTT_BROKER_HOST,
            config.MQTT_BROKER_PORT,
            config.MQTT_BASE_TOPIC
        )
        
        # Setup UI
        self.setup_ui()
        self.connect_signals()
        
        # Auto-connect to broker
        QTimer.singleShot(500, self.connect_to_broker)
    
    def setup_ui(self):
        """Setup the user interface."""
        central_widget = QWidget()
        self.setCentralWidget(central_widget)
        
        main_layout = QVBoxLayout(central_widget)
        
        # Top bar with connection info
        top_layout = QHBoxLayout()
        top_layout.addWidget(QLabel("EtherCAT Gateway:"))
        self.broker_input = QLineEdit()
        self.broker_input.setText(f"{config.MQTT_BROKER_HOST}:{config.MQTT_BROKER_PORT}")
        self.broker_input.setMaximumWidth(200)
        top_layout.addWidget(self.broker_input)
        
        self.topic_input = QLineEdit()
        self.topic_input.setText(config.MQTT_BASE_TOPIC)
        self.topic_input.setMaximumWidth(300)
        top_layout.addWidget(QLabel("Topic:"))
        top_layout.addWidget(self.topic_input)
        
        self.connect_button = QPushButton("Connect")
        self.connect_button.clicked.connect(self.connect_to_broker)
        top_layout.addWidget(self.connect_button)
        
        self.status_indicator = StatusIndicator()
        top_layout.addWidget(self.status_indicator)
        top_layout.addStretch()
        
        main_layout.addLayout(top_layout)
        
        # Table widget for devices and signals
        self.tree = QTreeWidget()
        self.tree.setColumnCount(5)
        self.tree.setHeaderLabels([
            "Device / Signal",
            "Description",
            "Direction",
            "Type",
            "Value"
        ])
        
        # Configure tree
        header = self.tree.header()
        header.setSectionResizeMode(0, QHeaderView.Stretch)
        header.setSectionResizeMode(1, QHeaderView.Stretch)
        header.setSectionResizeMode(2, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(3, QHeaderView.ResizeToContents)
        header.setSectionResizeMode(4, QHeaderView.ResizeToContents)

        self.tree.setContextMenuPolicy(Qt.CustomContextMenu)
        self.tree.customContextMenuRequested.connect(self._on_tree_context_menu)

        main_layout.addWidget(self.tree)
        
        # Status bar
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)
        self.statusBar.showMessage("Disconnected")
    
    def connect_signals(self):
        """Connect MQTT signals to slots."""
        self.mqtt.connected.connect(self._on_connected)
        self.mqtt.disconnected.connect(self._on_disconnected)
        self.mqtt.bridge_status_changed.connect(self._on_bridge_status_changed)
        self.mqtt.bridge_info_received.connect(self._on_bridge_info_received)
        self.mqtt.device_metadata_received.connect(self._on_device_metadata_received)
        self.mqtt.signal_value_changed.connect(self._on_signal_value_changed)
        self.mqtt.error_occurred.connect(self._on_error)
    
    def connect_to_broker(self):
        """Connect to the MQTT broker."""
        try:
            # Parse broker address
            broker_str = self.broker_input.text()
            if ":" in broker_str:
                host, port = broker_str.split(":")
                port = int(port)
            else:
                host = broker_str
                port = config.MQTT_BROKER_PORT
            
            # Update MQTT connection settings
            self.mqtt.broker_host = host
            self.mqtt.broker_port = port
            self.mqtt.base_topic = self.topic_input.text()
            
            self.connect_button.setEnabled(False)
            self.connect_button.setText("Connecting...")
            
            if self.mqtt.is_connected:
                self.mqtt.disconnect()
            else:
                self.mqtt.connect()
        except ValueError:
            QMessageBox.warning(self, "Invalid Input", "Please enter a valid broker address")
            self.connect_button.setEnabled(True)
            self.connect_button.setText("Connect")
    
    @pyqtSlot()
    def _on_connected(self):
        """Handle MQTT connection."""
        logger.info("Connected to MQTT broker")
        self.status_indicator.set_status(True)
        self.statusBar.showMessage("Connected to broker")
        self.connect_button.setText("Disconnect")
        try:
            self.connect_button.clicked.disconnect()
        except TypeError:
            pass  # No previous connection
        self.connect_button.clicked.connect(self.disconnect_from_broker)
        self.broker_input.setEnabled(False)
        self.topic_input.setEnabled(False)
    
    @pyqtSlot()
    def _on_disconnected(self):
        """Handle MQTT disconnection."""
        logger.info("Disconnected from MQTT broker")
        self.status_indicator.set_status(False)
        self.statusBar.showMessage("Disconnected")
        self.connect_button.setText("Connect")
        try:
            self.connect_button.clicked.disconnect()
        except TypeError:
            pass  # No previous connection
        self.connect_button.clicked.connect(self.connect_to_broker)
        self.connect_button.setEnabled(True)
        self.broker_input.setEnabled(True)
        self.topic_input.setEnabled(True)
    
    def disconnect_from_broker(self):
        """Disconnect from the MQTT broker."""
        logger.info("Disconnecting from broker...")
        self.mqtt.disconnect()
    
    @pyqtSlot(bool)
    def _on_bridge_status_changed(self, status: bool):
        """Handle bridge status change."""
        self.bridge.update_status(status)
        logger.info(f"Bridge status: {status}")
    
    @pyqtSlot(dict)
    def _on_bridge_info_received(self, info: dict):
        """Handle bridge info reception."""
        logger.info(f"Bridge info received with {len(info.get('slaves', {}))} slaves")
        
        # Track device changes before update
        prev_device_ids = set(self.bridge.devices.keys())
        
        # Update bridge info (this handles adding/removing devices)
        self.bridge.update_info(info)
        
        # Detect changes
        current_device_ids = set(self.bridge.devices.keys())
        removed_devices = prev_device_ids - current_device_ids
        added_devices = current_device_ids - prev_device_ids
        
        if removed_devices:
            logger.info(f"Devices removed: {removed_devices}")
        
        if added_devices:
            logger.info(f"Devices added: {added_devices}")
        
        self.last_device_ids = current_device_ids
        self.refresh_table()
    
    @pyqtSlot(int, dict)
    def _on_device_metadata_received(self, device_id: int, metadata: dict):
        """Handle device metadata reception."""
        logger.info(f"Device metadata received for device {device_id} with {len(metadata.get('pdos', []))} signals")
        self.bridge.update_device_metadata(device_id, metadata)
        
        # Subscribe to all signals in this device
        if "pdos" in metadata:
            for pdo in metadata["pdos"]:
                index = pdo.get("index", "")
                subindex = pdo.get("subIndex", "")
                if index and subindex:
                    self.mqtt.subscribe_to_signal(device_id, index, subindex)
                    logger.debug(f"Subscribed to signal {device_id}/{index}/{subindex}")
        
        self.refresh_table()
    
    @pyqtSlot(str, str)
    def _on_signal_value_changed(self, topic: str, value: str):
        """Handle signal value change."""
        # Parse topic to extract device_id, index, subindex
        parts = topic.split("/")
        if len(parts) >= 3:
            try:
                device_id = int(parts[-3])
                index = parts[-2]
                subindex = parts[-1]
                
                # Update model
                device = self.bridge.get_device(device_id)
                if device:
                    signal = device.get_signal(index, subindex)
                    if signal:
                        signal.value = value
                        
                        # Update widget if exists
                        key = (device_id, index, subindex)
                        if key in self.signal_widgets:
                            self.signal_widgets[key].blockSignals(True)
                            self.signal_widgets[key].set_value(value)
                            self.signal_widgets[key].blockSignals(False)
            except (ValueError, IndexError):
                pass
    
    @pyqtSlot(str)
    def _on_error(self, error_message: str):
        """Handle error messages."""
        logger.error(error_message)
        self.statusBar.showMessage(f"Error: {error_message}")
    
    def refresh_table(self):
        """Refresh the tree with current data."""
        self.tree.clear()
        self.signal_widgets.clear()
        
        # Root bridge item
        bridge_item = QTreeWidgetItem(self.tree)
        bridge_state = self.bridge.info.get('state', 'unknown') if self.bridge.info else 'unknown'
        bridge_item.setText(0, f"Bridge ({bridge_state})")
        
        bridge_font = QFont()
        bridge_font.setBold(True)
        bridge_item.setFont(0, bridge_font)
        self._color_item_by_state(bridge_item, bridge_state)
        
        # Add devices
        for device_id in sorted(self.bridge.devices.keys()):
            device = self.bridge.devices[device_id]
            
            # Device item
            device_item = QTreeWidgetItem(bridge_item)
            device_item.setData(0, Qt.UserRole, device_id)
            csa = device.metadata.get('ringCsa', 'N/A')
            # Show the actual alarm (e.g. "0x001E: Invalid input
            # configuration"), not just a bare "something's wrong" marker.
            alarm_suffix = f" ⚠ {device.alarm_code}: {device.alarm}" if device.error else ""
            device_item.setText(0, f"{csa}: {device.name} ({device.state}){alarm_suffix}")
            if device.error:
                device_item.setToolTip(0, f"AL Status Code {device.alarm_code}: {device.alarm}")

            device_font = QFont()
            device_font.setBold(True)
            device_item.setFont(0, device_font)
            if device.error:
                # An AL error takes priority over the state color -- e.g. a
                # slave stuck at PREOP+ERROR should read as alarming, not
                # just "pre-operational" (which alone is a normal transient
                # state, not something to flag).
                is_dark = self.palette().base().color().lightness() < 128
                device_item.setForeground(0, QColor(255, 60, 60) if is_dark else QColor(178, 0, 0))
            else:
                self._color_item_by_state(device_item, device.state)
            
            # Add signals
            if device.signals:
                for signal_key in sorted(device.signals.keys()):
                    signal = device.signals[signal_key]
                    
                    signal_item = QTreeWidgetItem(device_item)
                    signal_item.setText(0, f"0x{signal.index}/{signal.subindex}: {signal.name}")
                    signal_item.setText(1, signal.description)
                    signal_item.setText(2, signal.data_direction)
                    signal_item.setText(3, signal.data_type)
                    
                    # Add value widget
                    value_widget = SignalValueWidget(
                        signal.data_type, 
                        signal.value,
                        read_only=(signal.data_direction == "Input")
                    )
                    key = (device_id, signal.index, signal.subindex)
                    self.signal_widgets[key] = value_widget
                    value_widget.value_changed.connect(
                        lambda val, d=device_id, i=signal.index, s=signal.subindex: 
                        self._on_signal_write(d, i, s, val)
                    )
                    self.tree.setItemWidget(signal_item, 4, value_widget)
        
        self.tree.expandAll()
    
    def _color_item_by_state(self, item: QTreeWidgetItem, state: str):
        """Color an item based on its state (darkmode aware). Handles both bridge and slave states."""
        palette = self.palette()
        bg_color = palette.base().color()
        is_dark = bg_color.lightness() < 128
        
        state_upper = state.upper()
        
        # Bridge states
        if state_upper == "RUNNING":
            color = QColor(0, 150, 0) if is_dark else QColor(34, 139, 34)
        elif state_upper == "WAITING":
            color = QColor(255, 165, 0) if is_dark else QColor(218, 165, 32)
        elif state_upper == "RESCANNING":
            color = QColor(255, 140, 0) if is_dark else QColor(255, 140, 0)
        # EtherCAT slave states
        elif state_upper == "OP":  # Operational
            color = QColor(0, 150, 0) if is_dark else QColor(34, 139, 34)
        elif state_upper == "PREOP":  # Pre-operational
            color = QColor(255, 165, 0) if is_dark else QColor(218, 165, 32)
        elif state_upper == "SAFEOP":  # Safe-operational
            color = QColor(255, 140, 0) if is_dark else QColor(255, 140, 0)
        elif state_upper == "INIT":  # Init
            color = QColor(173, 216, 230) if is_dark else QColor(135, 206, 235)
        elif state_upper == "BOOT":  # Bootstrap (firmware update in progress)
            color = QColor(186, 85, 211) if is_dark else QColor(148, 0, 211)
        else:  # Unknown/Error
            color = QColor(255, 69, 0) if is_dark else QColor(220, 20, 60)
        
        item.setForeground(0, color)
    
    def _on_signal_write(self, device_id: int, index: str, subindex: str, value: str):
        """Handle signal write from UI."""
        self.mqtt.publish_signal(device_id, index, subindex, value)
        logger.info(f"Wrote {value} to signal {device_id}/{index}/{subindex}")

    def _on_tree_context_menu(self, pos):
        """Right-click menu on the tree -- offers 'Generate PDO Override...' for device rows."""
        item = self.tree.itemAt(pos)
        if item is None:
            return
        device_id = item.data(0, Qt.UserRole)
        if device_id is None or device_id not in self.bridge.devices:
            return  # not a device row (bridge root or a signal row)

        menu = QMenu(self)
        action = menu.addAction("Generate PDO Override...")
        action.triggered.connect(lambda: self._open_override_dialog(device_id))
        menu.exec_(self.tree.viewport().mapToGlobal(pos))

    def _open_override_dialog(self, device_id: int):
        """Open the --pdo-config generator for one device."""
        device = self.bridge.devices.get(device_id)
        if device is None:
            return
        dialog = GenerateOverrideDialog(device, self)
        dialog.exec_()

    def closeEvent(self, event):
        """Handle window close event."""
        if self.mqtt.is_connected:
            self.mqtt.disconnect()
        event.accept()
