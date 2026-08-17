"""Configuration for ECGW GUI application."""

# MQTT Broker settings -- match these to your gateway's --broker/--port/--topic
MQTT_BROKER_HOST = "10.0.1.100"
MQTT_BROKER_PORT = 1883
MQTT_BASE_TOPIC = "ecgw-cx9020"

# GUI Settings
WINDOW_TITLE = "EtherCAT Gateway GUI"
WINDOW_WIDTH = 1200
WINDOW_HEIGHT = 800

# Reconnection settings
RECONNECT_ATTEMPTS = 5
RECONNECT_DELAY = 2  # seconds
