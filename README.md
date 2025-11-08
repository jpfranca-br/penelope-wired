# Sylvester Wired

Sylvester Wired is firmware for the WT32-ETH01 module that hunts for industrial controllers on a wired LAN, bridges them to
MQTT, and exposes a technician-friendly web console. The device can be dropped into a cabinet, powered from 5 V, and will scan
the local /24 network for PLCs while streaming live logs to both the dashboard and an MQTT broker.

> Looking for developer-facing details? See [TECHNICAL_SPEC.md](TECHNICAL_SPEC.md).

## Hardware at a Glance
- **Controller:** WT32-ETH01 (ESP32 with LAN8720 PHY)
- **Power:** 5 V DC via the module header (≈300 mA peak during Wi-Fi operation)
- **Networking:** 10/100 Ethernet uplink plus local Wi-Fi access point for configuration
- **Field Interfaces:** MQTT broker, HTTP dashboard, persistent TCP tunnel to the discovered PLC

## First Boot Checklist
1. **Flash the firmware** using your preferred ESP32 toolchain (Arduino IDE, `esptool.py`, etc.).
2. **Connect Ethernet** to the plant network and power the module from a regulated 5 V supply.
3. **Wait for link-up:** the device waits for a wired IP before starting MQTT or scanning. Serial logs (115200 baud) mirror what
you will later see on the dashboard.
4. **Join the technician Wi-Fi:** SSID `sylvester-<mac without colons>` with default password `12345678`.
5. **Open the dashboard:** browse to `http://192.168.4.1/` to watch discovery progress and live logs.

## Daily Operation
### Web Dashboard
- **Monitor status:** `/` shows Ethernet state, last MQTT command, last PLC request/response, and a scrolling log buffer.
- **Configure wiring:** `/config` lets you switch between DHCP and static IP addressing for the wired port. Static fields are
pre-filled with the last saved values.
- **Logs API:** `/logs` returns JSON suitable for remote tooling if you need to ingest logs outside the UI.

### MQTT Integration
All MQTT topics are prefixed by `sylvester/<device-mac>/` where `<device-mac>` is the lowercase MAC without colons.

| Topic | Direction | Usage |
|-------|-----------|-------|
| `log` | Publish | Real-time log stream identical to the web console. |
| `response/<crc32>` | Publish | PLC responses keyed by the CRC32 of the originating command. |
| `command` | Subscribe | Device management actions (see below). |
| `request` | Subscribe | PLC polling scheduler; payload format `COMMAND` or `COMMAND|intervalMs|sendAlwaysFlag`. |

#### Supported Commands on `command`
- `boot` — Reboot the device.
- `factoryreset` — Wipe stored settings (ports, Wi-Fi password, wired profile) and restart.
- `scan` — Force a fresh network sweep for PLCs.
- `port <p1> [p2 ...]` — Replace the TCP port list (1–10 values). Scan restarts automatically.
- `ipconfig dhcp` — Return to DHCP on the wired interface.
- `ipconfig fixed <ip> <mask> <gateway> <dns>` — Apply a static wired address.
- `wifipassword <newpass>` — Update the SoftAP password (8–63 chars). AP restarts immediately.
- `ota <firmware.bin> <firmware.md5>` — Perform an OTA update with integrity checking.

#### PLC Request Scheduling on `request`
Send the raw PLC command as the payload. Optional metadata allows automation:
- `COMMAND|60000|1` → send every 60 s and always publish responses.
- `COMMAND|0|` → send once; scheduler slot is released after completion.
- If `sendAlwaysFlag` is omitted, the device only republishes when the response changes.

### Discovering and Tunnelling to the PLC
- The scanner sweeps the local `/24` range derived from the wired IP mask using up to eight parallel workers.
- On each candidate, it tests the configured port list and sends `(&V)` to confirm a PLC banner.
- Once a PLC replies, the device keeps a persistent TCP connection and forwards MQTT `request` traffic through it.
- The last known PLC IP/port is stored so the device can reconnect quickly after power loss.

## Maintenance and Troubleshooting
- **Watch the logs:** serial console, MQTT `log` topic, and the dashboard all show identical diagnostics.
- **Ethernet issues:** if DHCP/static misconfiguration prevents link-up, issue `ipconfig dhcp` from MQTT or use the Wi-Fi
config page to revert.
- **OTA safety:** the device validates the MD5 before applying firmware. Provide both `.bin` and matching `.md5` URLs.
- **Factory reset:** clears Wi-Fi password, port list, wired settings, and scheduled requests. Use when redeploying.

## Localization
A Brazilian Portuguese translation of this README is available in [README-ptBR.md](README-ptBR.md).
