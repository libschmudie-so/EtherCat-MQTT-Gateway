# EtherCatMqttGateway

An EtherCAT ⇄ MQTT bridge implemented in C++.
It scans an EtherCAT ring, configures slaves, and exposes process data variables over MQTT topics.
Process data can be monitored and written through MQTT, enabling integration with automation systems and IoT platforms.

---

## Features

* EtherCAT master via **SOEM** or **IGH (Etherlab) master**, selected at compile time (`-DEC_BACKEND=SOEM|IGH`)
* Automatic slave scan and PDO mapping, cross-referenced with ESI XMLs for human-readable names/types
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
└── src
    ├── CMakeLists.txt
    ├── include/ecmqtt/       # public headers (config, backend interface, ESI, MQTT, slave device)
    └── src/
        ├── main.cpp          # wiring + cycle loop
        ├── config.cpp        # CLI parsing (cxxopts)
        ├── logging.cpp       # spdlog setup
        ├── esi_repository.cpp   # ESI XML parsing (tinyxml2)
        ├── slave_device.cpp     # bit-level read/write + JSON conversion
        ├── mqtt_client.cpp      # libmosquitto wrapper
        └── backends/
            ├── soem_backend.cpp  # compiled iff EC_BACKEND=SOEM
            └── igh_backend.cpp   # compiled iff EC_BACKEND=IGH
```

---

## Building

### Dependencies

Install via apt (Debian/Ubuntu):

```sh
sudo apt-get install cmake g++ pkg-config \
  nlohmann-json3-dev libspdlog-dev libcxxopts-dev libtinyxml2-dev libmosquitto-dev
```

The **SOEM** backend (default) is fetched and built automatically via CMake `FetchContent` — no extra install needed.

The **IGH (Etherlab) master** backend requires a matching kernel module and userspace library already built and installed on the machine (it can't be fetched generically since it's tied to your NIC driver) — see https://gitlab.com/etherlab.org/ethercat. CMake looks for `ecrt.h`/`libethercat` under `/usr/local` or `/opt/etherlab` by default; override with `-DIGH_INCLUDE_DIR=...` / `-DIGH_LIBRARY=...` if installed elsewhere.

### Build

```sh
cmake -B build -S src -DCMAKE_BUILD_TYPE=Release -DEC_BACKEND=SOEM
cmake --build build -j"$(nproc)"
# binary: build/ethercat-mqtt-gateway
```

Or build a Docker image (SOEM backend):

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
./ethercat-mqtt-gateway \
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

`--use-reported-csa` uses each slave's persistent SII "Configured Station Alias" for its MQTT topic instead of its ring position, so the topic survives the slave being moved to a different port -- see `--write-alias` below to set one. Slaves with no alias set fall back to ring position.

### Non-default PDO assignment (`--pdo-config`)

Some terminals declare more than one PDO set in their ESI file (e.g. "Standard"/"Compact"/"Enhanced" variants) but come up using the default one. `--pdo-config <file.json>` selects a different one per device (matched by vendor/product/revision, not ring position):

```json
{
  "overrides": [
    {
      "vendorId": "#x2", "productCode": "#x10883052", "revisionNo": "#x110000",
      "rxPdo": ["#x1601"],
      "txPdo": ["#x1a02"]
    }
  ]
}
```

`rxPdo`/`txPdo` list the PDO indices (as declared in that device's ESI `<RxPdo>`/`<TxPdo>` blocks) to assign instead of the default; omit either to leave that direction alone. Requires the device's ESI file to be present in `--esi`.

### Setting a persistent slave alias (`--write-alias`)

**Only built into SOEM builds** -- IGH's userspace library has no SII/EEPROM write API, so `-DEC_BACKEND=IGH` binaries don't have this option at all; on IGH targets use the target's own `ethercat alias -pPOSITION VALUE` tool instead (part of the standard IGH master install), then power-cycle the slave.

```sh
./ethercat-mqtt-gateway --iface eth0 --write-alias "3=100"
```

Writes alias `100` to the slave currently at ring position 3, then exits without running the bridge. **Power-cycle that slave** afterward -- the alias is latched by the EtherCAT slave controller at reset, not applied live. Once set, `--use-reported-csa` will address it by that alias regardless of where it sits in the ring -- handy for replacing a broken card: write the same alias to its replacement and the MQTT topic doesn't change. Multiple slaves can be set in one call: `--write-alias "1=100,2=101"`.

### Hot-plug / hot-unplug (`--hotplug`, only built into IGH builds)

`--hotplug` periodically checks whether the slaves physically present on the bus differ from what's currently configured, and reconfigures to pick up the change -- newly plugged cards get brought up and start publishing to MQTT, removed cards drop out of the active slave list (their retained MQTT topics are left as-is, not cleared). SOEM's classic API has no equivalent notion once `configure()` has run, so `-DEC_BACKEND=SOEM` binaries don't have this option at all.

This has a real cost: IGH's own API is explicit that slave configuration can't be altered once the master is activated, so applying a change means briefly deactivating and reactivating the whole master -- **every** slave (not just the one that changed) drops cyclic servicing for the duration of the reconfigure, not just the hot-plugged one. Each reconfigure also leaks one internal domain object (IGH's public API has no call to free one); fine for occasional hot-plug events, worth knowing if they happen very frequently in your setup.

---

## MQTT Topics

* `<topic>/<CSA>/metadata` – Metadata for each slave (includes `ringCsa` + `reportedCsa`)
* `<topic>/<CSA>/<Index>/<SubIndex>` – Process data variables
* `<topic>/bridge/status` – Online/offline bridge status
* `<topic>/bridge/info` – Startup info (interface, CSA mode, ESI path, frequency, slaves)

---

## Notes

* ESI XML files must be present in the configured ESI directory for slaves/PDOs to get human-readable names and descriptions (`description` in `<topic>/<CSA>/metadata`, from each device's/entry's `<Comment>`, falling back to its name); without a match, entries fall back to numeric `Index:SubIndex` names and an empty description. This applies equally to both backends.
* On IGH, a slave whose live PDO introspection comes back empty for a sync manager (typically a hardwired-mapping terminal with neither a CoE mailbox nor an SII PDO-assignment category) falls back to that device's ESI-declared default mapping for the direction, the same source SOEM's analogous opaque-buffer fallback uses -- requires the device's ESI file to be present in `--esi`.
* The container requires `CAP_NET_RAW` and `CAP_NET_ADMIN` to access EtherCAT interfaces **if pipework/macvlan is not used** (e.g. with host networking). This applies to the SOEM backend; the IGH backend instead requires its kernel module to be loaded on the host.
