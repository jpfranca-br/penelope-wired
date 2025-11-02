# Penelope Wired

Penelope Wired is a WT32-ETH01 firmware that combines Ethernet backhaul, a Wi-Fi access point, MQTT telemetry, and a resilient TCP polling loop to locate and monitor PLCs or other automation controllers on the local network. The sketch exposes a real-time web dashboard, publishes structured MQTT topics, and stores critical settings in non-volatile preferences so the scanner can recover automatically after power loss.

## Hardware and Network Overview
- **Platform:** WT32-ETH01 with LAN8720 Ethernet PHY (`ETH.begin` in `penelope-wired.ino`).
- **Ethernet role:** Primary uplink for MQTT and TCP server communication. The firmware waits for `ARDUINO_EVENT_ETH_GOT_IP` before starting MQTT and scanning where possible.
- **Wi-Fi role:** Local management access point named `penelope-<mac>` that mirrors the device MAC address without colons.

## Startup Flow
1. Initialise serial logging and Ethernet, tracking link state transitions with `WiFi.onEvent` (`onEvent` in `penelope-wired.ino`).
2. Derive the device MAC, MQTT topic base (`penelope/<mac>/`), and access point SSID.
3. Open the `wifi-config` namespace in `Preferences`, restore the stored Wi-Fi password and port list (`loadWifiSettings`, `loadPorts`).
4. Start the management access point and HTTP server (`setupAccessPoint`).
5. Once Ethernet is ready the device connects to MQTT, launches eight parallel scan tasks, and begins polling any discovered server.

## Multi-threaded Searching
- `startNetworkScan` (in `network_scanner.ino`) spawns `NUM_SCAN_TASKS = 8` FreeRTOS tasks to sweep the entire /24 subnet derived from the Ethernet IP and mask.
- Each task attempts TCP connections against the configured ports list, defaults `{2001, 1771, 854}`. Successful probes send `(&V)` and wait for a banner response to confirm a target.
- Progress and discoveries are streamed to the log buffer, MQTT log topic, and the web dashboard.
- When a server is found the shared state halts all other scan tasks and establishes a persistent TCP client (`client.connect`).
- Commands can be sent to switch ports dynamically via the `port` MQTT command, which saves the list to flash and restarts the scan automatically.

## Resilience and Self-healing
- Ethernet event callbacks clear cached IPs and trigger reconnect logic when the wired link drops (`onEvent`).
- MQTT connection attempts are retried up to five times with watchdog-friendly delays (`connectMQTT`).
- The TCP client uses bounded retries for both reconnects and command polling (`ensureServerConnection`, `sendCommand` in `tcp_server.ino`).
- Logs are buffered in `logBuffer` (200 entries) and always mirrored to MQTT when available, providing traceability after transient faults.
- If the TCP server or Ethernet link disappears the scanner automatically restarts the discovery process.

## Wi-Fi Access Point Management
- Default password: `12345678`. The current password is persisted in `Preferences` under the `apPassword` key (`loadWifiSettings`).
- Command `wifipassword <password>` stores a new WPA2 password (8–63 characters), disconnects all connected stations, and restarts the SoftAP with the updated credentials while keeping the web server running.
- Factory resets clear the stored password, returning the SoftAP to the default credentials on the next boot.

## MQTT Topics
Topics are namespaced by the sanitized MAC address: `penelope/<mac>/`. Key topics include:

| Topic suffix | Direction | Payload | Source |
|--------------|-----------|---------|--------|
| `command`    | Subscribe | String commands listed below | Cloud/controller |
| `request`    | Subscribe | Raw TCP payload to forward to the discovered server | Cloud/controller |
| `response`   | Publish   | Single-line response from the TCP server after forwarding a request | Device |
| `running`    | Publish   | Periodic heartbeat with the latest `(&V)` poll response | Device |
| `log`        | Publish   | Human-readable status lines from `addLog` | Device |

## Command Reference
Commands are received on `penelope/<mac>/command` and are case-insensitive.

| Command | Parameters | Effect |
|---------|------------|--------|
| `boot` | — | Reboots the device immediately. |
| `factoryreset` | — | Clears all persisted settings (ports and Wi-Fi password), disconnects Wi-Fi clients, and restarts so defaults are restored. |
| `scan` | — | Stops any active TCP session and restarts the network scan from address `.1`. |
| `port <p1> [p2 ...]` | 1–10 integers | Saves up to 10 TCP ports, restarts scanning with the new list. |
| `wifipassword <password>` | 8–63 character string | Updates the SoftAP password, saves it to flash, disconnects existing stations, and restarts the access point. |

Invalid parameters are rejected with descriptive log entries that surface both on the dashboard and the MQTT log topic.

## Monitoring Interfaces
- **Web dashboard (`/`):** Live log viewer with automatic scrolling, status summary cards, and Ethernet connection indicators (`handleMonitor`, `handleLogs`, `handleCSS` in `webpage.ino`).
- **MQTT log stream:** Every `addLog` call is echoed to `penelope/<mac>/log`, allowing remote monitoring without the web UI.
- **Stored metadata:** The dashboard and MQTT logs include the last command received, last request forwarded, and last response captured for quick troubleshooting.

## Persistence and Factory Reset
- `Preferences` namespace `wifi-config` keeps the port list (`numPorts`, `port0...`) and SoftAP password across reboots.
- The `factoryreset` command (or clearing the namespace manually) wipes the stored keys, applies the default Wi-Fi password, and forces a reboot so scanning restarts with default ports.

## Development Notes
- All tasks yield frequently to feed the watchdog (`yield()` calls within loops) ensuring stability even during long scans.
- The firmware gracefully handles missing Ethernet at boot by deferring MQTT and scanning until a link becomes available.
- HTTP handlers and MQTT callbacks reuse the shared logging utilities, so extending features should mirror the existing `addLog` patterns for consistency.
