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
├── src
│   ├── CMakeLists.txt
│   ├── include/ecmqtt/       # public headers (config, backend interface, ESI, MQTT, slave device)
│   └── src/
│       ├── main.cpp          # wiring + cycle loop
│       ├── config.cpp        # CLI parsing (cxxopts)
│       ├── logging.cpp       # spdlog setup
│       ├── esi_repository.cpp   # ESI XML parsing (tinyxml2)
│       ├── slave_device.cpp     # bit-level read/write + JSON conversion
│       ├── mqtt_client.cpp      # libmosquitto wrapper
│       └── backends/
│           ├── soem_backend.cpp  # compiled iff EC_BACKEND=SOEM
│           └── igh_backend.cpp   # compiled iff EC_BACKEND=IGH
└── gui-client                # PyQt5 desktop client, see gui-client/README.md
    ├── main.py
    ├── config.py
    ├── mqtt_connection.py
    └── gui/
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

`--use-reported-csa` uses each slave's persistent SII "Configured Station Alias" for its MQTT topic instead of its ring position, so the topic survives the slave being moved to a different port. See `--write-alias` below to set one. Slaves with no alias set fall back to ring position.

`--op-timeout <ms>` (default `2000`) is how long `activate()` waits for every slave to reach OPERATIONAL. SOEM fails startup on timeout; IGH just warns and continues (also applies after every `--hotplug` reconfigure). Raise it if slaves consistently need longer than 2s to come up on your bus.

### Non-default PDO assignment (`--pdo-config`)

Some terminals declare more than one PDO set in their ESI file (e.g. an EL3012's "AI Standard Channel 1" vs "AI Compact Channel 1") but come up using the default one. `--pdo-config <file.json>` selects a different one per device (matched by vendor/product/revision, not ring position):

```json
{
  "overrides": [
    {
      "vendorId": "#x2", "productCode": "#xbc43052", "revisionNo": "#x120000",
      "txPdo": ["AI Compact Channel 1"]
    },
    {
      "vendorId": "0x2", "productCode": "0x10883052", "revisionNo": "0x110000",
      "rxPdo": ["#x1601"],
      "txPdo": ["#x1a02", "Some Other TxPdo Name"]
    },
    {
      "vendorId": 2, "productCode": 277360722, "revisionNo": 1114112,
      "rxPdo": [5633]
    }
  ]
}
```

That covers every accepted form: `vendorId`/`productCode`/`revisionNo` each take an ESI-style hex string (`"#x2"`), a `0x`-prefixed hex string (`"0x2"`), a plain decimal string, or a bare JSON integer. Pick whichever's convenient (usually copy-pasted straight from a slave's own `vendorId`/`productCode`/`revisionNo` in its published metadata, which are already in `"0x..."` form). Each `rxPdo`/`txPdo` entry is independently either a PDO index in any of those same forms, or the PDO's ESI `<Name>` as a string (matched case-insensitively), and a single override can mix index- and name-based entries freely, as the second one above does.

`rxPdo`/`txPdo` list what to assign instead of the default for that direction (omit either to leave it alone). Each entry is either a hex/decimal PDO index or the PDO's declared ESI `<Name>` (matched case-insensitively, as in the EL3012 example above), whichever's more convenient; both can be mixed freely. Requires the device's ESI file to be present in `--esi`. Every slave's `vendorId`/`productCode`/`revisionNo` (as hex strings, ready to paste in here) are included in its own published `<topic>/<CSA>/metadata`, so you don't need to dig through ESI XML to find them. If a selector doesn't match, the resulting warning lists every PDO ESI declares for that device+direction (index and name) to help find the right one.

### Setting a persistent slave alias (`--write-alias`)

**Only built into SOEM builds.** IGH's userspace library has no SII/EEPROM write API, so `-DEC_BACKEND=IGH` binaries don't have this option at all. On IGH targets, use the target's own `ethercat alias -pPOSITION VALUE` tool instead (part of the standard IGH master install), then power-cycle the slave.

```sh
./ethercat-mqtt-gateway --iface eth0 --write-alias "3=100"
```

Writes alias `100` to the slave currently at ring position 3, then exits without running the bridge. **Power-cycle that slave** afterward: the alias is latched by the EtherCAT slave controller at reset, not applied live. Once set, `--use-reported-csa` will address it by that alias regardless of where it sits in the ring, which is handy for replacing a broken card: write the same alias to its replacement and the MQTT topic doesn't change. Multiple slaves can be set in one call: `--write-alias "1=100,2=101"`.

### Hot-plug / hot-unplug (`--hotplug[=cleanup]`, only built into IGH builds)

`--hotplug` periodically checks whether the slaves physically present on the bus differ from what's currently configured, and reconfigures to pick up the change: newly plugged cards get brought up and start publishing to MQTT, removed cards drop out of the active slave list (their retained MQTT topics are left as-is, not cleared, unless you pass `--hotplug=cleanup`). SOEM's classic API has no equivalent notion once `configure()` has run, so `-DEC_BACKEND=SOEM` binaries don't have this option at all.

This has a real cost: IGH's own API is explicit that slave configuration can't be altered once the master is activated, so applying a change means briefly deactivating and reactivating the whole master. **Every** slave (not just the one that changed) drops cyclic servicing for the duration of the reconfigure, not just the hot-plugged one. Each reconfigure also leaks one internal domain object (IGH's public API has no call to free one); fine for occasional hot-plug events, worth knowing if they happen very frequently in your setup. `<topic>/bridge/info`'s `state` field tracks the phases of a reconfigure in progress (see below).

`--hotplug=cleanup` additionally clears a removed slave's retained `<topic>/<CSA>/metadata` and process-data topics and unsubscribes from its output topics, instead of leaving them retained forever under a CSA nothing will publish to again. Plain `--hotplug` leaves them retained by default, since that staleness is sometimes exactly what you want (a card temporarily unplugged for maintenance keeping its last-known state visible).

Reconfiguring gets every surviving output variable's last known value re-applied once the new topology is up (a deactivate/reactivate cycle otherwise resets outputs to zero/default), so whatever's physically being controlled doesn't silently reset just because some unrelated slave elsewhere on the bus was added or removed.

---

## MQTT Topics

* `<topic>/<CSA>/metadata` – Metadata for each slave: `ringCsa` + `reportedCsa`; `vendorId`/`productCode`/`revisionNo` (hex, paste-ready into a `--pdo-config` override); a per-slave `state` (its live EtherCAT AL state, refreshed every cycle: one of `INIT`/`PREOP`/`BOOT`/`SAFEOP`/`OP`/`UNKNOWN`); `error` (bool, `alarmCode` != `"0x0000"`); `alarmCode` (hex, the slave's AL Status Code, ESC register 0x0134, ETG.1000.6) and `alarm` (its human-readable text, e.g. `alarmCode: "0x001E", alarm: "Invalid input configuration"` for a slave that refused a state change; `"0x0000"`/`"No error"` is itself a normal, valid value, not a placeholder); `pdos` (the currently-mapped signals); and `availableRxPdos`/`availableTxPdos` (every RxPdo/TxPdo ESI declares for this device, index + name, whether or not it's the one currently mapped: everything a client needs to build a `--pdo-config` selector without its own copy of ESI, see the GUI client below). `state`/`error`/`alarmCode`/`alarm` are kept fresh every cycle internally and republished automatically whenever any of them actually change (checked roughly every 100 cycles), not just at connect or after a hotplug reconfigure.
* `<topic>/<CSA>/<Index>/<SubIndex>` – Process data variables
* `<topic>/bridge/status` – Online/offline bridge status
* `<topic>/bridge/info` – Startup info (interface, CSA mode, ESI path, frequency, slaves) plus a `state` field: `running` normally; during a `--hotplug` reconfigure, briefly `rescanning` (bus rescan in progress) then `waiting` (slaves coming back to OPERATIONAL) before returning to `running`

---

## GUI Client

[`gui-client/`](gui-client/) is a PyQt5 desktop client that talks to a running gateway purely over the MQTT topics above, with no direct EtherCAT access. It shows a live device/signal tree (color-coded by bridge and per-slave state), lets you write to output signals with type-aware controls, and includes a **Generate PDO Override** picker: right-click a device to choose from its `availableRxPdos`/`availableTxPdos` (by ESI name or index) and get a ready-to-use `--pdo-config` JSON snippet, without needing your own copy of ESI or the device's vendor/product/revision memorized. See [`gui-client/README.md`](gui-client/README.md) for setup and usage.

---

## Notes

* ESI XML files must be present in the configured ESI directory for slaves/PDOs to get human-readable names and descriptions (`description` in `<topic>/<CSA>/metadata`, from each device's/entry's `<Comment>`, falling back to its name); without a match, entries fall back to numeric `Index:SubIndex` names and an empty description. This applies equally to both backends.
* On IGH, a slave whose live PDO introspection comes back empty for a sync manager (typically a hardwired-mapping terminal with neither a CoE mailbox nor an SII PDO-assignment category) falls back to that device's ESI-declared default mapping for the direction, the same source SOEM's analogous opaque-buffer fallback uses. Requires the device's ESI file to be present in `--esi`.
* The container requires `CAP_NET_RAW` and `CAP_NET_ADMIN` to access EtherCAT interfaces **if pipework/macvlan is not used** (e.g. with host networking). This applies to the SOEM backend; the IGH backend instead requires its kernel module to be loaded on the host.
* On IGH, a slave's identity (vendor/product/revision, read once from SII during the bus scan) can occasionally get stuck all-zero independently of its AL state. Observed staying at OP the whole time this happens, since state is tracked live every cycle via a completely separate path. Since this doesn't always clear on the first try, the gateway escalates across up to 4 attempts when it happens during a scan: `ethercat rescan` plus a settle delay (twice); then, if still stuck, deactivating and recreating the domain (the same "bus reinit" a hotplug reconfigure does) plus a longer delay; then, if still stuck even after that, releasing and re-requesting the master entirely (the same recovery a full process restart provides, confirmed on real hardware to clear a slave that survives every lighter attempt) before the final try. Requires the IGH command-line tool to be installed and on `PATH` for the rescan steps.
* On IGH, a `--pdo-config` selector that doesn't resolve against ESI (unknown name, or a numeric index ESI doesn't declare) is silently dropped rather than sent to the wire empty: IGH needs full entry content up front for any PDO it configures, unlike SOEM (which only ever writes the raw index and lets the slave supply its own entries). Sending an empty entry list to a Fixed-mapping slave (e.g. an EL3012, which can't have its PDO mapping changed at all) isn't a no-op: the slave refuses the resulting remap and can get stuck at `PREOP` with its AL error flag set instead of keeping its already-working mapping. Check the log for `... not found` warnings if an override doesn't seem to be taking effect.
* On IGH, `--pdo-config` overrides and the ESI-default fallback both correctly skip mailbox sync managers (SM0/SM1, for any slave that has CoE) rather than applying process-data PDOs to them. Mailbox SMs report the same `EC_DIR_OUTPUT`/`EC_DIR_INPUT` values as real process-data ones, since IGH's public API has no distinct "mailbox" direction, so this needs an explicit skip. Without it, a device whose process-data sync managers aren't SM0/SM1 (e.g. a multi-channel analog input terminal, where SM0/SM1 are mailbox and SM2/SM3 carry the actual channels) could get its CoE mailbox communication corrupted by a `--pdo-config` override intended for its real process-data SM, surfacing as the slave stuck at `PREOP` with AL status `0x001E` ("Invalid input configuration").
* Per-slave `state`/`alarmCode`/`alarm` are refreshed every cycle using calls each backend's API explicitly documents as realtime-safe: IGH uses `ecrt_slave_config_state()` for state and a register request (reading ESC register 0x0134 directly) for the alarm code; SOEM uses `ec_readstate()`, a single broadcast read covering every slave. Earlier revisions of this used IGH's `ecrt_master_get_slave()` for the same purpose, which carries no such realtime-safe guarantee. Calling it this often caused a real production incident (it started failing outright under sustained polling, which cascaded through the topology-change detection into a reconfigure loop that progressively dropped slaves). It's no longer called from any per-cycle or frequently-polled path.
