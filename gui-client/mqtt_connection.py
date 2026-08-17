"""MQTT connection handler for ECGW."""

import json
import logging
from typing import Callable, Dict, Any
import paho.mqtt.client as mqtt
from PyQt5.QtCore import QObject, pyqtSignal

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class MQTTConnection(QObject):
    """Handles MQTT connection and message handling."""
    
    # Signals
    connected = pyqtSignal()
    disconnected = pyqtSignal()
    bridge_status_changed = pyqtSignal(bool)
    bridge_info_received = pyqtSignal(dict)
    device_metadata_received = pyqtSignal(int, dict)  # device_id, metadata
    signal_value_changed = pyqtSignal(str, str)  # topic, value
    error_occurred = pyqtSignal(str)
    
    def __init__(self, broker_host: str, broker_port: int, base_topic: str):
        super().__init__()
        self.broker_host = broker_host
        self.broker_port = broker_port
        self.base_topic = base_topic
        self.client = mqtt.Client()
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.is_connected = False
        self.device_ids = set()
        self.subscribed_devices = set()  # Track which devices we've subscribed to
        
    def connect(self):
        """Connect to the MQTT broker."""
        try:
            logger.info(f"Connecting to {self.broker_host}:{self.broker_port}")
            self.client.connect(self.broker_host, self.broker_port, keepalive=60)
            self.client.loop_start()
        except Exception as e:
            logger.error(f"Failed to connect: {e}")
            self.error_occurred.emit(f"Connection failed: {e}")
    
    def disconnect(self):
        """Disconnect from the MQTT broker."""
        self.client.loop_stop()
        self.client.disconnect()
    
    def _on_connect(self, client, userdata, flags, rc):
        """Called when the client connects to the broker."""
        if rc == 0:
            msg = "Connected to MQTT broker"
            logger.info(msg)
            print(f"[MQTT] {msg}")
            self.is_connected = True
            self.connected.emit()
            self._subscribe_to_topics()
        else:
            msg = f"Connection failed with code {rc}"
            logger.error(msg)
            print(f"[MQTT] {msg}")
            self.error_occurred.emit(f"MQTT connection failed with code {rc}")
    
    def _on_disconnect(self, client, userdata, rc):
        """Called when the client disconnects from the broker."""
        self.is_connected = False
        self.disconnected.emit()
        if rc != 0:
            logger.warning(f"Unexpected disconnection with code {rc}")
    
    def _on_message(self, client, userdata, msg):
        """Called when a message is received."""
        topic = msg.topic
        payload = msg.payload.decode()
        
        print(f"[MQTT MSG] {topic} = {payload[:200]}")
        logger.info(f">>> RECEIVED MESSAGE: {topic} = {payload[:200]}")
        
        try:
            if topic.endswith("/bridge/status"):
                value = payload.lower() == "true"
                logger.info(f"  Emitting bridge_status_changed: {value}")
                self.bridge_status_changed.emit(value)
            
            elif topic.endswith("/bridge/info"):
                data = json.loads(payload)
                logger.info(f"  Emitting bridge_info_received with {len(data.get('slaves', {}))} slaves")
                self.bridge_info_received.emit(data)
                # Extract device IDs for subscription
                current_devices = set()
                if "slaves" in data:
                    for slave_id in data["slaves"].keys():
                        device_id = int(slave_id)
                        current_devices.add(device_id)
                        self.device_ids.add(device_id)
                    
                    # Subscribe to metadata for new devices
                    new_devices = current_devices - self.subscribed_devices
                    for device_id in new_devices:
                        self.client.subscribe(f"{self.base_topic}/{device_id}/metadata")
                        self.subscribed_devices.add(device_id)
                        logger.info(f"Subscribed to metadata for device {device_id}")
            
            elif "/metadata" in topic:
                # Extract device ID from topic
                parts = topic.split("/")
                if len(parts) >= 2:
                    try:
                        device_id = int(parts[-2])
                        data = json.loads(payload)
                        logger.info(f"  Emitting device_metadata_received for device {device_id}")
                        self.device_metadata_received.emit(device_id, data)
                    except (ValueError, json.JSONDecodeError) as e:
                        logger.warning(f"  Failed to parse metadata: {e}")
            
            else:
                # Regular signal value
                logger.info(f"  Emitting signal_value_changed: {topic} = {payload}")
                self.signal_value_changed.emit(topic, payload)
        
        except json.JSONDecodeError as e:
            logger.debug(f"Non-JSON message on {topic}: {payload}")
            self.signal_value_changed.emit(topic, payload)
        except Exception as e:
            logger.error(f"Error processing message on {topic}: {e}", exc_info=True)
    
    def _subscribe_to_topics(self):
        """Subscribe to initial topics."""
        self.client.subscribe(f"{self.base_topic}/bridge/status")
        self.client.subscribe(f"{self.base_topic}/bridge/info")
        self.client.subscribe(f"{self.base_topic}/#")  # Subscribe to everything for debugging
        logger.info(f"Subscribed to base topics under {self.base_topic}")
    
    def subscribe_to_signal(self, device_id: int, index: str, subindex: str):
        """Subscribe to a specific signal."""
        topic = f"{self.base_topic}/{device_id}/{index}/{subindex}"
        self.client.subscribe(topic)
    
    def publish_signal(self, device_id: int, index: str, subindex: str, value: str):
        """Publish a value to a signal."""
        topic = f"{self.base_topic}/{device_id}/{index}/{subindex}"
        self.client.publish(topic, value, retain=True)
        logger.info(f"Published {value} to {topic} (retained)")
