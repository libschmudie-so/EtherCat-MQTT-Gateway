# EtherCAT Gateway GUI

A PyQt5 desktop client for monitoring and controlling slaves exposed by the
[EtherCAT ⇄ MQTT gateway](../README.md) in this repo, over the same MQTT
topics the gateway publishes. No direct connection to the EtherCAT bus
itself; everything goes through the broker.

## Features

- **Device discovery**: populates from `<topic>/bridge/info` and each
  slave's own `<topic>/<CSA>/metadata`, live (subscribes as new slaves show
  up, e.g. from a `--hotplug` reconfigure)
- **Real-time monitoring**: signal values update as the gateway publishes them
- **Signal control**: type-aware widgets (checkbox, spin box, bitfield
  checkboxes for `BITn`, text) for writing to output signals
- **State-aware coloring**: bridge state (`running`/`rescanning`/`waiting`)
  and per-slave AL state (`INIT`/`PREOP`/`BOOT`/`SAFEOP`/`OP`/`UNKNOWN`),
  dark-mode aware
- **Alarms**: a slave with its AL error flag set (metadata's `error: true`,
  e.g. stuck at `PREOP` after refusing a state change) shows its actual
  AL Status Code and message (e.g. "⚠ 0x001E: Invalid input
  configuration") in red, regardless of its state color. Both are live,
  refreshed every cycle on the gateway side, not just at connect
- **Generate PDO Override**: right-click a device for a picker over its
  ESI-declared RxPdo/TxPdo options (by name, e.g. an EL3012's "AI Standard
  Channel 1" vs "AI Compact Channel 1"), producing a ready-to-use
  `--pdo-config` JSON snippet. Copy to clipboard, save as a new file, or
  merge into an existing one

## Installation

```sh
cd gui-client
pip install -r requirements.txt
```

## Configuration

Edit `config.py` to match your gateway (the broker address/port and root
`--topic` it was started with):

```python
MQTT_BROKER_HOST = "127.0.0.1"
MQTT_BROKER_PORT = 1883
MQTT_BASE_TOPIC = "ethercat"
```

The broker address and topic are also editable from the top bar at runtime,
without touching this file.

## Usage

```sh
python3 main.py
```

The window auto-connects on startup using `config.py`'s defaults. Once
connected it subscribes to `bridge/status`, `bridge/info`, and each slave's
`metadata`/process-data topics as they appear, and populates the device/
signal tree.

To generate a `--pdo-config` override: right-click a device row → **Generate
PDO Override...**. Requires that device's metadata to have already arrived
(its `vendorId`/`productCode`/`revisionNo` and `availableRxPdos`/
`availableTxPdos` come from `<topic>/<CSA>/metadata`, see the main
README's MQTT Topics section). If a device shows no available PDO options,
either its ESI file isn't in the gateway's `--esi` directory, or its
metadata hasn't been received yet.

## Architecture

- `main.py`: application entry point
- `config.py`: default broker/topic settings
- `mqtt_connection.py`: MQTT client, parses gateway topics into Qt signals
- `gui/models.py`: data models (`Bridge`, `Device`, `Signal`, `PdoOption`)
- `gui/widgets.py`: `SignalValueWidget` (type-aware read/write control), `StatusIndicator`
- `gui/main_window.py`: main window, device/signal tree
- `gui/override_dialog.py`: the `--pdo-config` generator

## Troubleshooting

- **No devices appearing**: confirm the gateway is actually running and
  reachable at the configured broker/topic, and that `<topic>/bridge/info`
  is being published (check with `mosquitto_sub -t '<topic>/#' -v`).
- **A device has no PDO options in the override dialog**: its ESI file
  isn't in the gateway's `--esi` directory, or the gateway hasn't published
  fresh metadata for it since restarting.
- Written values are published retained. A value that doesn't seem to take
  usually means the target variable is an `Input` (read-only) or the
  gateway rejected the write; check the gateway's own log output.
