# ESP32-S3 Bosch eBike Live Data Accessory

ESP-IDF firmware for an ESP32-S3 DevKitC-1 that behaves as a Bosch smart-system Live Data accessory. The ESP32 pairs with the bike over BLE, subscribes to Live Data notifications, decodes known fields, provides a local administration web UI, and can expose or push telemetry as JSON.

This project is intended for developers and experimentation. It is not an official Bosch product.

Licensed under the Apache License 2.0. See [LICENSE](LICENSE).
Third-party dependency and trademark notes are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Hardware Target

- Board: ESP32-S3 DevKitC-1 class board
- PlatformIO board id: `esp32-s3-devkitc1-n16r8`
- Flash: 16 MB
- PSRAM: 8 MB
- USB: ESP32-S3 USB Serial/JTAG
- Status LED: onboard RGB LED, used for connection state

The firmware is configured for an ESP32-S3 target. Other ESP32 variants will need board, pin, partition, and BLE configuration review.

## What The Device Does

- Advertises as a BLE accessory for the Bosch Live Data interface.
- Accepts the bike-initiated BLE connection.
- Uses NimBLE with bonding, LE Secure Connections, and Just Works pairing.
- Discovers the Bosch Live Data service and characteristic after connection.
- Enables notifications and decodes known protobuf fields.
- Stores the latest decoded bike state in RAM.
- Writes throttled bike data and connection events to a small rotated SPIFFS log.
- Provides a Wi-Fi setup/admin UI.
- Provides local JSON APIs for polling.
- Optionally pushes logs and bike data to configured HTTP(S) endpoints.
- Optionally publishes bike data to Home Assistant through MQTT Discovery.
- Discovers Philips Hue Bridges on the LAN as the first step toward Hue smart plug integration.

## Bosch Live Data Mapping

Protocol reference:

- Bosch eBike Systems Open Live Data Interface PDF: <https://www.bosch-ebike.com/fileadmin/EBC/Service/Downloads/LiveData/20260501_LiveDataInterface_V1_28042026.pdf?_=1777357728>

- Service solicitation UUID: `0000eb20-eaa2-11e9-81b4-2a2ae2dbcce4`
- Live Data characteristic UUID: `0000eb21-eaa2-11e9-81b4-2a2ae2dbcce4`
- GAP mode: limited discoverable, undirected connectable, BR/EDR unsupported
- Security: bonding enabled, LE Secure Connections, Just Works IO capability
- ATT MTU: requests `247`
- LE data length: requests 251-octet data length
- CCCD: writes `0x0001` to enable notifications

The protobuf decoder is dependency-free and hand-written for the known public Live Data fields. Unknown fields are skipped for forward compatibility.

## Repository Layout

- `main/app_main.c`: boot sequence and subsystem startup
- `main/ble_gap.c`: BLE advertising, pairing, connection handling
- `main/gatt_client.c`: service/characteristic discovery and notification handling
- `main/live_data_decode.c`: Live Data protobuf decoding and latest-state formatting
- `main/wifi_admin.c`: setup AP, web UI, local APIs, Wi-Fi config
- `main/accessory_config.c`: persisted device/export configuration in NVS
- `main/telemetry_export.c`: optional push export worker
- `main/home_assistant.c`: optional MQTT Discovery and state publisher for Home Assistant
- `main/hue_integration.c`: Philips Hue Bridge discovery
- `main/persistent_log.c`: SPIFFS-backed rotated bike/connection log
- `main/log_store.c`: in-memory runtime log ring
- `main/status_led.c`: RGB status LED driver
- `tools/`: Windows-friendly PlatformIO build/upload helpers

## Build

Install PlatformIO, then from the repository root:

```powershell
.\tools\pio-build.ps1 -Environment esp32s3
```

The helper script sets a local temp directory and `IDF_PATH` before direct Ninja builds. This avoids recurring Windows/PlatformIO regeneration issues in this project.

Force CMake/PlatformIO regeneration:

```powershell
.\tools\pio-build.ps1 -Environment esp32s3 -Configure
```

Clean the generated build directory:

```powershell
.\tools\pio-build.ps1 -Environment esp32s3 -Clean
```

## Flash And Monitor

Find the serial port:

```powershell
pio device list
```

Recommended deployment command:

```powershell
.\deploy.ps1 -Port COMx
```

By default this builds the app, flashes only the app partition, preserves local NVS state such as Wi-Fi credentials and Hue pairing, uses a conservative `460800` baud upload speed, and verifies the web UI at `http://boschldi.local/config`.

Full flash for a fresh board:

```powershell
.\deploy.ps1 -Port COMx -Full -Configure
```

Useful deployment options:

- `-Baud 230400`: use an even slower upload speed
- `-SkipBuild`: flash the existing build artifact
- `-Clean` or `-Configure`: regenerate build state before flashing
- `-NoVerify`: skip the post-flash web check
- `-VerifyUrl http://192.168.x.y/config`: verify by IP when mDNS is unavailable

Low-level build and flash:

```powershell
.\tools\pio-upload.ps1 -Port COMx -Environment esp32s3
```

Flash an already-built image:

```powershell
.\tools\pio-upload.ps1 -Port COMx -Environment esp32s3 -SkipBuild
```

Serial monitor:

```powershell
pio device monitor --port COMx --baud 115200
```

Replace `COMx` with your actual port.

## Wi-Fi Administration

On boot, the ESP32 tries saved Wi-Fi credentials from NVS. If no credentials exist, or the saved network cannot be reached, it starts a temporary setup access point.

Setup AP:

- SSID: `BoschLDI-Setup-XXXX`
- Password: `boschldi`
- URL: `http://192.168.4.1`
- mDNS name when available: `http://boschldi.local/`

The setup page scans nearby Wi-Fi networks and saves credentials. Saving Wi-Fi reconnects without reboot. If changing to a different network, the browser device must also be on the new network to reach the ESP32 again.

When the ESP32 is already connected, the Configuration tab shows the current SSID and does not scan automatically. Press **Change Wi-Fi** to scan nearby networks. Before switching networks, the UI warns that the ESP32 will disconnect from the current network. New credentials are tested before they are saved; if the connection fails, the ESP32 falls back to the previously saved Wi-Fi.

When connected to Wi-Fi, the dashboard is served at:

```text
http://boschldi.local/
```

If mDNS does not resolve on a client, use the IP address printed in the logs or shown by your router.

## Web UI

The dashboard has six tabs:

- **Bike data**: latest decoded bike state and persistent bike log downloads
- **Logs**: searchable/filterable runtime log viewer
- **Hue**: Hue Bridge discovery, pairing, and a readable device list
- **Automation**: bike-data-to-Hue rule editor, shown when a Hue Bridge is paired
- **Configuration**: BLE accessory name, optional push export, Home Assistant MQTT, internal RGB LED, Wi-Fi configuration
- **Documentation**: built-in endpoint and configuration reference

The configured device name is used for the BLE accessory relationship with the bike. It does not change DNS; the network hostname remains `boschldi`.

## Local HTTP APIs

Read endpoints:

- `GET /api/bike`: latest decoded bike state as JSON
- `GET /api/logs`: runtime log ring as JSON
- `GET /api/state`: bike state and runtime logs in one JSON response
- `GET /api/automation/rules`: current bike-to-Hue automation rules
- `GET /api/hue/bridges`: Hue Bridge discovery via mDNS
- `GET /api/hue/status`: local Hue pairing status without exposing the stored app key
- `GET /api/hue/pair/progress`: current background Hue pairing status
- `GET /api/hue/devices`: Hue Bridge `lights` resource, including lamps and smart plugs exposed as controllable light resources
- `GET /api/hue/resources`: full local Hue Bridge v1 state for debugging
- `GET /config`: current device name, push export, Home Assistant MQTT, LED, and Wi-Fi status settings
- `GET /scan`: nearby Wi-Fi networks as JSON
- `GET /logs`: runtime logs as plain text
- `GET /latest`: latest decoded bike state as plain text
- `GET /bike-log`: current persistent bike/connection log file
- `GET /bike-log/previous`: previous rotated persistent log file

Configuration endpoints use `application/x-www-form-urlencoded` form bodies:

- `POST /device-name`
  - `device_name`: 1-24 printable ASCII characters
- `POST /save`
  - `ssid`
  - `pass`
  - The firmware connects first, saves only after success, and falls back to the previous saved Wi-Fi if the new connection fails.
- `POST /export-config`
  - `logs_url`
  - `logs_interval_sec`
  - `bike_url`
  - `bike_interval_sec`
- `POST /ha-config`
  - `ha_enabled`: optional checkbox value; omitted means off
  - `ha_host`: MQTT broker host or IP
  - `ha_port`: MQTT broker port, default `1883`
  - `ha_username`: optional MQTT username
  - `ha_password`: optional MQTT password; blank keeps the stored password
  - `ha_discovery_prefix`: default `homeassistant`
  - `ha_topic_base`: default `boschldi`
  - `ha_interval_sec`: clamped to 2-3600 seconds
- `POST /led-config`
  - `led_enabled`: optional checkbox value; omitted means off
  - `led_brightness_percent`: clamped to 1-100
  - `led_boot_color`, `led_advertising_color`, `led_connected_color`, `led_secured_color`, `led_ready_color`, `led_activity_color`, `led_error_color`: `#RRGGBB`
- `POST /api/hue/pair`
  - `bridge_host`: optional bridge IP/host; if empty, the first discovered bridge is used
- `POST /api/hue/pair/start`
  - `bridge_host`: optional bridge IP/host; starts a one-minute pairing window that retries automatically
- `POST /api/hue/light/state`
  - Internal automation action endpoint.
  - `light_id`: numeric Hue v1 light id from `/api/hue/devices`
  - `on`: `true`, `1`, or `on` switches on; any other value switches off
- `POST /api/automation/rule`
  - `enabled`: optional checkbox value
  - `field`: decoded bike field such as `speed_kmh`, `bike_light`, or `bike_not_driving`
  - `op`: `<`, `<=`, `==`, `>=`, or `>`
  - `value`: numeric comparison value; boolean fields use `0` or `1`, bike light uses `1` for off and `2` for on
  - `light_id`: numeric Hue v1 light id from `/api/hue/devices`
  - `action_on`: `true`, `1`, or `on` turns on; any other value turns off
  - `cooldown_sec`: clamped to 5-3600
  - Stored JSON may also contain up to 3 `conditions` entries. All conditions are combined with AND.
- `POST /api/automation/default`
  - `light_id`: numeric Hue v1 light id from `/api/hue/devices`
  - Adds the built-in example rule: `battery_soc < 35` AND `battery_soc < 70` AND `bike_not_driving == 1`, then turn the selected Hue smart plug/device on.
- `POST /api/automation/clear`
  - Clears all stored automation rules
- `POST /api/hue/clear`
  - Clears the locally stored Hue Bridge host and app key

Empty push URLs disable push export.

## Home Assistant

Home Assistant support uses MQTT Discovery, which is the native Home Assistant path for MQTT devices. Configure an MQTT broker in Home Assistant, keep discovery enabled, then enter the broker settings in the ESP Configuration tab.

Reference:

- Home Assistant MQTT integration and discovery: <https://www.home-assistant.io/integrations/mqtt/>

Default MQTT topics:

- Discovery configs: `homeassistant/<component>/boschldi_<entity>/config`
- Availability: `boschldi/status`
- Latest bike state: `boschldi/state`

Discovery payloads are retained so Home Assistant can recreate entities after its own restart. The ESP publishes the latest decoded bike JSON at the configured interval and exposes individual Home Assistant entities through value templates. The firmware does not store a Home Assistant REST API token.

## Polling Guidance

For fast live clients, prefer pull APIs over push:

- Poll `/api/bike` every 1-2 seconds for dashboards.
- Poll `/api/logs` only while a user is actively viewing logs.
- Use `/api/state` when a client wants both datasets in a single request.

Push export is optional and intended for slower background forwarding. The firmware currently clamps push intervals to:

- Logs: minimum 60 seconds
- Bike data: minimum 10 seconds
- Maximum interval: 3600 seconds

## Philips Hue Integration

Hue smart plugs are usually reached through a Hue Bridge on the LAN, not as direct Wi-Fi plugs. The current integration discovers bridges with mDNS service `_hue._tcp.local` and exposes the result at:

```text
GET /api/hue/bridges
```

The Hue tab is intentionally an overview and pairing surface, not a Hue management app. It discovers bridges, shows the paired bridge name when mDNS provides one, and lists reachable Hue devices in a table so automation targets are easy to identify.

Pairing creates a local Hue application key. The preferred UI flow starts a one-minute pairing window:

```text
POST /api/hue/pair/start
GET /api/hue/pair/progress
```

After starting the pairing window, press the physical Bridge button. The ESP32 retries automatically every two seconds until the Bridge authorizes it or the window times out.

For manual/testing use, press the physical Bridge button, then call:

```text
POST /api/hue/pair
```

The optional form field `bridge_host` can force a specific bridge IP or hostname. If it is empty, the firmware uses the first bridge found by mDNS. The application key is stored in NVS and is not returned by the status endpoint.

The local Hue API is bridge-local. One paired bridge exposes only the devices paired to that bridge. If the Hue mobile app shows devices from multiple bridges, that is app-level aggregation; this firmware currently stores one bridge pairing and does not proxy devices from other bridges.

After pairing:

```text
GET /api/hue/status
GET /api/hue/devices
GET /api/hue/resources
POST /api/hue/light/state
```

The first implementation uses the local Hue v1 HTTP API because it is reliable on ESP32 without adding Hue Bridge certificate handling. Smart plugs typically appear in the Hue `lights` resource as on/off controllable devices. Direct on/off actions are kept in the automation flow, not the Hue overview tab.

## Automation

Automation rules are stored in NVS as a small JSON array, capped at six rules. The automation worker starts at boot, but evaluation requests are queued only when at least one enabled automation rule exists. Rules are evaluated only when fresh Bosch Live Data arrives from the bike. Each rule has a cooldown, so a matching condition cannot toggle the same Hue target continuously on every notification.

The first rule engine supports numeric comparisons against decoded bike fields and Hue on/off actions. Supported fields currently include `speed_kmh`, `cadence_rpm`, `rider_power_w`, `ambient_brightness_lux`, `battery_soc`, `odometer_m`, `bike_light`, `system_locked`, `charger_connected`, `light_reserve_state`, `diagnosis_program_active`, and `bike_not_driving`.

Automation runs in its own FreeRTOS worker task. Live Data decoding only queues a lightweight evaluation request, so BLE packet handling is not blocked by JSON parsing or network I/O. If station Wi-Fi is not connected, automation actions are skipped because Hue devices cannot be reached.

Next steps for plug control and automation:

- Add multi-bridge pairing storage if controlling more than one Hue Bridge is needed.
- Add delete/edit per rule instead of clear-all plus append.
- Add richer condition types once more bike data fields are decoded.
- Move resource listing/control to Hue API v2 HTTPS once certificate handling is implemented.

## Storage

Partition layout:

- NVS: 24 KB
- PHY init: 4 KB
- App partition: 4 MB
- SPIFFS storage: 256 KB

SPIFFS stores persistent bike/connection logs. The firmware keeps a current file and one previous rotated file. Runtime logs shown in the UI are primarily from an in-memory ring buffer, not flash. Optional JSON push export for runtime logs and bike data is skipped while station Wi-Fi is disconnected.

Generated firmware binaries, build output, managed components, runtime logs, and local/private files are ignored by git.

## Status LED

The onboard RGB LED represents high-level connection state. Colors, enabled state, and brightness are configurable in the Configuration tab and stored in NVS. Expected states include boot, bike pairing/advertising, connected, secured, Live Data ready, data activity, and error.

## Development Notes

- Keep public defaults generic. Do not commit real Wi-Fi credentials, endpoint URLs, captured bike data, MAC addresses, serial ports, or local logs.
- Before committing, follow `AGENTS.md` and inspect staged files and staged diff for sensitive data.
- Put local-only notes or secrets under ignored folders such as `.local/`, `local/`, `private/`, or `secrets/`.
- `managed_components/` is generated by ESP-IDF component management and intentionally ignored.

## Known Limits

- Device Information Service and Battery Service are not implemented.
- The decoder covers known Live Data fields and skips unknown fields.
- The web UI is intentionally static and served directly from firmware strings.
- Push export is best-effort. Failed sends are skipped and retried on the next configured interval; the firmware does not build an unbounded offline queue.
