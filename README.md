# EtherCatMqttGateway

An EtherCAT ⇄ MQTT bridge implemented in C#.
It scans an EtherCAT ring, configures slaves, and exposes process data variables over MQTT topics.
Process data can be monitored and written through MQTT, enabling integration with automation systems and IoT platforms.

---

## Features

* EtherCAT master using [EtherCAT.NET](https://github.com/)
* Automatic slave scan and PDO mapping from ESI XMLs
* Publishes process data to MQTT topics
* Subscribes to output variables for remote control
* Metadata publishing for each slave
* Configurable CSA mode for MQTT topics (`ringCsa` vs `reportedCsa`)
* Configurable cycle frequency with overrun detection
* Clean shutdown and reconnect handling for MQTT
* Configurable via CLI options or Docker environment variables

---

## Repository Structure

```
.
├── Dockerfile
├── entrypoint.sh
└── EtherCatMqttGateway
   ├── EtherCatMqttGateway.csproj
   ├── Program.cs
   └── SlaveDevice.cs
```

---

## Building

Build locally with the .NET SDK:

```sh
dotnet publish -c Release EtherCatMqttGateway/EtherCatMqttGateway.csproj -o out
```

Or build a Docker image:

```sh
docker build -t ethercat-mqtt .
```

---

## Running with Docker

### Example with host networking

```sh
docker run --rm \
  --network host \
  --cap-add NET_RAW \
  --cap-add NET_ADMIN \
  -v ./esi:/data/esi \
  -e IFACE=enx207bd22c6b91 \
  -e BROKER=192.168.1.100 \
  -e PORT=1883 \
  -e FREQ=20 \
  -e RETAIN=true \
  -e CLIENT_ID=mygateway \
  -e TOPIC=plant1/ethercat \
  -e USE_REPORTED_CSA=true \
  -e NO_OUTPUT=true \
  -e LOGLEVEL=debug \
  ethercat-mqtt
```

### Example with pipework/macvlan

```sh
docker run --rm \
  -v ./esi:/data/esi \
  -e IFACE=eth1 \
  -e BROKER=192.168.1.100 \
  ethercat-mqtt
```

Attach the interface using pipework:

```sh
pipework enx207bd22c6b91 <container_id> 0.0.0.0/24
```

---

## Configuration

### Environment variables

| Variable           | Default          | Description                                          |
| ------------------ | ---------------- | ---------------------------------------------------- |
| `IFACE`            | (required)       | Network interface for EtherCAT (e.g. `eth0`)         |
| `BROKER`           | `127.0.0.1`      | MQTT broker hostname or IP                           |
| `PORT`             | `1883`           | MQTT broker port                                     |
| `ESI_DIR`          | `/data/esi`      | Path to ESI XML directory                            |
| `FREQ`             | `10`             | Cycle frequency in Hz                                |
| `RETAIN`           | `false`          | Retain MQTT process data messages                    |
| `NO_OUTPUT`        | `false`          | Do not publish output variables                      |
| `CLIENT_ID`        | `EtherCATMaster` | MQTT client ID                                       |
| `TOPIC`            | `ethercat`       | Root MQTT topic                                      |
| `USE_REPORTED_CSA` | `false`          | Use reported CSA instead of ring CSA for MQTT topics |
| `LOGLEVEL`         | `info`           | One of: `debug`, `verbose`, `quiet`, `info`          |

### CLI options (if not using entrypoint.sh)

Run inside container or natively:

```sh
dotnet EtherCatMqttGateway.dll \
  --iface eth0 \
  --broker 192.168.1.100 \
  --port 1883 \
  --frequency 10 \
  --esi /data/esi \
  --client-id mygateway \
  --topic plant1/ethercat \
  --use-reported-csa \
  --no-output \
  --retain \
  --debug
```

---

## MQTT Topics

* `<topic>/<CSA>/metadata` – Metadata for each slave (includes `ringCsa` + `reportedCsa`)
* `<topic>/<CSA>/<Index>/<SubIndex>` – Process data variables
* `<topic>/bridge/status` – Online/offline bridge status
* `<topic>/bridge/info` – Startup info (interface, CSA mode, ESI path, frequency, slaves)

---

## Notes

* ESI XML files must be present in the configured ESI directory.
* The container requires `CAP_NET_RAW` and `CAP_NET_ADMIN` to access EtherCAT interfaces **if pipework/macvlan is not used** (e.g. with host networking).