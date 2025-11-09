# Sylvester Wired Technical Specification

## Purpose and Scope
Sylvester Wired is an ESP32 (WT32-ETH01) firmware that automates the discovery and polling of PLCs or other automation
controllers over Ethernet while exposing MQTT and HTTP interfaces for integration and observability. This specification
captures the functional behaviour, module responsibilities, global state, and every callable function so an engineer can
reimplement the firmware logic in an alternative stack.

---

## Functional Overview
1. **Boot and Hardware Bring-up**
   * Initialise serial logging, persistent storage (`Preferences`), wired interface, and task synchronisation primitives.
   * Restore saved configuration (Wi-Fi AP password, Ethernet mode, TCP ports, server target, scheduled commands).
   * Start an isolated Wi-Fi access point with a monitoring web server for technicians.
   * When Ethernet acquires an address, establish an MQTT session and spawn a multi-threaded IP sweep that searches for a PLC
     using a short banner handshake.
2. **Operation Loop**
   * Maintain MQTT connectivity, refresh the public IP periodically, and handle HTTP dashboard requests.
   * If Ethernet drops or a PLC cannot be found, gracefully tear down dependent services and restart scanning once conditions
     stabilise.
3. **Control Channels**
   * MQTT `command` topic: device management actions (reboot, factory reset, OTA, wired IP changes, Wi-Fi password updates,
     rescans, port lists).
   * MQTT `request` topic: declarative command scheduler. Payload metadata defines interval and publish policy.
   * Web UI: live log viewer and Ethernet configuration form for field technicians.
4. **Persistence**
   * Device remembers TCP target, port list, wired IP mode, Wi-Fi password, and scheduled request workers across reboots.
5. **Resilience**
   * Watchdog-friendly loops, bounded retries for MQTT/TCP, and automatic restart of scans when connectivity changes.

---

## Global State and Shared Resources
| Symbol | Type | Owner | Purpose |
|--------|------|-------|---------|
| `logBuffer[MAX_LOG_LINES]`, `logIndex`, `logCount` | Circular buffer | `log.ino` | Stores most recent log lines for MQTT/web streaming. |
| `mqttClient`, `mqttTopicBase`, `mqtt_broker`, `mqtt_port`, `mqtt_user`, `mqtt_password` | MQTT session | `sylvester-wired.ino`/`mqtt_functions.ino` | Broker configuration and active client instance. |
| `preferences` | `Preferences` | `sylvester-wired.ino` | Flash-backed key-value store for configuration. |
| `server` | `WebServer` | `sylvester-wired.ino`/`webpage.ino` | HTTP management endpoint. |
| `client` | `WiFiClient` | `sylvester-wired.ino`/`mqtt_functions.ino` | TCP socket to the detected PLC. |
| `serverIP`, `serverPort`, `serverFound` | Shared state | `network.ino` | Details of discovered PLC target. |
| `ports[10]`, `numPorts` | Arrays | `network.ino` | Configurable TCP port list for scanning. |
| `commandSlots[MAX_COMMAND_SLOTS]`, `commandSlotsMutex` | Scheduler state | `mqtt_functions.ino` | Metadata and protection for MQTT request workers. |
| `serverMutex`, `scanMutex` | Semaphores | `sylvester-wired.ino` | Serialise access to server discovery and scan cursors. |
| `wiredDhcpEnabled`, `wiredStatic*` | Wired config | `network.ino` | Persisted Ethernet addressing. |
| `ap_ssid`, `ap_password` | Wi-Fi AP config | `sylvester-wired.ino` | Credentials for local management network. |
| `deviceMac`, `macAddress`, `wiredIP`, `internetAddress` | Identity | `sylvester-wired.ino` | Derived addresses shown in UI/MQTT. |
| `lastCommandReceived`, `lastRequestSent`, `lastResponseReceived` | Telemetry | `sylvester-wired.ino` | Surfaced in logs and dashboard. |

---

## Module and Function Reference
All functions are grouped by compilation unit. Unless stated otherwise, functions operate on the global state defined above.

### 1. `log.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `addLog(String message)` | Appends a message to the circular log buffer, prints to serial, and publishes to the MQTT `log` topic when connected. | `message`: text to append. | Mutates `logBuffer`, `logIndex`, `logCount`; writes to serial; publishes MQTT. Called by almost every module for observability. |

### 2. `sylvester-wired.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `logMessage(const String &message)` | Inline helper that forwards to `addLog`. | `message`. | Convenience wrapper used during setup. |
| `setup()` | Firmware entry point. Sets up serial, preferences, Ethernet, MQTT, access point, HTTP server, semaphores, command scheduler, and initiates scanning if Ethernet is live. | None. | Configures hardware, restores persisted state via `loadWiredConfig`, `loadPersistedServerDetails`, `loadWifiSettings`, `loadPorts`, and kicks off MQTT/scan flows. |
| `loop()` | Main execution loop. Services HTTP requests, monitors Ethernet link transitions, reconnects MQTT, triggers public IP refreshes, restarts scans, and throttles execution. | None. | Calls `server.handleClient`, `connectMQTT`, `refreshPublicIP`, `startNetworkScan`; controls `serverFound` lifecycle. |

Forward declarations at top tie this module to handlers implemented elsewhere, enforcing linkage between the orchestrator and supporting modules.

### 3. `network.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `onEvent(arduino_event_id_t event)` | Callback registered with `WiFi.onEvent`. Updates Ethernet status flags, logs transitions, and triggers public IP refresh. | `event`: Wi-Fi/Ethernet event ID. | Mutates `eth_connected`, `wiredIP`, `internetAddress`, `publicIPRefreshRequested`, `lastPublicIPCheck`; writes log entries. |
| `parseIPAddress(const String &value, IPAddress &out)` | Validates and parses dotted IP text. | `value`: string; `out`: reference for parsed result. | Returns `true` on success and populates `out`; otherwise leaves `out` unchanged. |
| `isWiredDhcp()`/`getWired*Setting()` | Accessors exposed to web layer. | None. | Return cached configuration strings or DHCP flag. |
| `loadPersistedServerDetails()` | Restores last detected PLC target from `Preferences`. | None. | Sets `serverIP`, `serverPort`, logs status. Invalid entries are cleared. |
| `persistServerDetails(const String &ip, int port)` | Saves PLC target information. | `ip`, `port`. | Writes to `Preferences` and logs. |
| `loadWiredConfig()` | Pulls wired Ethernet mode and addressing from flash, validating IP octets. | None. | Updates `wiredDhcpEnabled` and static address strings/IPAddress instances. Invalid static config is reset to DHCP. |
| `applyWiredConfigToDriver(bool logOutcome)` | Pushes DHCP or static config into the ESP32 Ethernet driver. | `logOutcome`: whether to emit logs about the result. | Calls `ETH.config`, logs success/failure. |
| `reconnectEthernetWithConfig()` | Reboots the Ethernet interface using current settings and resets dependent services. | None. | Stops MQTT/TCP if active, restarts `ETH.begin`, re-applies config, waits for link up to 10 seconds, and returns `true` on success. |
| `setWiredConfiguration(bool useDhcp, const String &ip, const String &mask, const String &gateway, const String &dns, String &errorMessage)` | Validates technician-supplied wired settings, persists them, and reapplies to the interface. | Mode flag plus optional IP parameters; `errorMessage` output. | Returns `true` on success. On static-IP failure, logs and falls back to DHCP while reporting the error. |
| `loadWifiSettings()` | Loads SoftAP password, falling back to default if invalid. | None. | Sets `ap_password` and logs the outcome. |
| `setupAccessPoint()` | Starts the ESP32 SoftAP, configures HTTP route handlers, and logs the management IP. | None. | Calls `WiFi.softAP`, registers server callbacks, and begins `server`. |
| `refreshPublicIP()` | Queries `http://api.ipify.org` to discover the WAN address. | None. | Updates `internetAddress` or logs errors. Defers if Ethernet offline. |
| `startNetworkScan()` | Launches a parallel scan across the /24 subnet for PLC ports. | None. | Validates Ethernet state, logs range, spawns `NUM_SCAN_TASKS` FreeRTOS tasks executing `scanTask`. |
| `scanTask(void *parameter)` | FreeRTOS worker routine that probes IP addresses for configured ports and records the first responsive server. | None. | Iteratively increments `currentScanIP`, attempts TCP connect/send `(&V)`, and when a response is found sets `serverFound`, `serverIP`, `serverPort`, starts persistent `client`, persists details, and terminates all scan workers. |
| `loadPorts()` | Restores up to 10 TCP ports from flash with defaults. | None. | Populates `ports[]`, `numPorts`, logs selection. |
| `beginHttpDownload(const String &url, HTTPClient &http, WiFiClient *&clientOut, String &errorMessage, bool &usedSecureTransport)` | Prepares an HTTP(S) client with timeout, redirect, and CA settings for OTA downloads, tracking whether TLS is in use. | URL plus HTTP client references. | Returns `true` on success, sets `clientOut` to the correct (secure/plain) client, and flags secure usage. |
| `downloadTextFile(const String &url, String &content, String &errorMessage)` | Synchronously downloads a small text resource (e.g., MD5). | URL; output `content`. | Returns `true` and fills `content` on HTTP 200 success. |

### 4. `mqtt_functions.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `connectMQTT()` | Attempts to connect to the MQTT broker, with up to five retries and watchdog-friendly delays. Subscribes to `request` and `command` topics on success. | None. | On success updates logs, ensures `wiredIP`, sets subscriptions; on failure logs and returns. |
| `mqttCallback(char* topic, byte* payload, unsigned int length)` | PubSubClient callback. Routes messages to command or request handlers based on topic suffix. | Topic and raw payload. | Invokes `handleCommand` or `handleMqttRequest`. |
| `initializeCommandScheduler()` | Allocates the scheduler mutex and clears all command slots. | None. | Initializes `commandSlots` array; logs error if mutex allocation fails. |
| `persistCommandSlots()` | Writes slot metadata (in-use flag, interval, send-always, active state, command text) to `Preferences`. | None. | Iterates through slots under mutex and updates flash. |
| `loadPersistedCommandSlots()` | Restores previously persisted request workers and relaunches intervals that were active. | None. | Rehydrates `commandSlots`, logs each restored entry, starts workers for intervalled commands, and repersists to clean stale data. |
| `parseUnsignedLong(const String &value, unsigned long &result)` | Validates numeric metadata tokens. | Numeric string. | Returns `true` on success and sets `result`. |
| `parseRequestPayload(const String &payload, String &command, unsigned long &intervalMs, bool &sendAlways, bool &hasMetadata)` | Parses scheduler metadata from MQTT payloads in the form `COMMAND|interval|flag`. | Raw payload string. | Fills `command`, optional interval, `sendAlways`, `hasMetadata`; returns `false` for empty or malformed input. |
| `reserveSlotForCommand(const String &command)` | Allocates or reuses a scheduler slot for a command. Evicts inactive slots if necessary. | Command text. | Returns slot index (`-1` on failure) after marking slot in use and resetting response cache. |
| `getActiveWorkerCount()` | Counts active command workers. | None. | Returns integer under mutex. |
| `startCommandWorker(int slotIndex)` | Launches a FreeRTOS task that repeatedly executes a scheduled command. | Slot index. | Sets slot active, spawns `commandTask`. Logs failure to create task. |
| `commandTask(void *param)` | Worker routine that sends the command immediately and on each interval, handles termination flags, and persists slot state on exit. | Slot index via `param`. | Calls `sendCommandAndMaybePublish`, updates `nextRun`, honours `terminateAfterNext`, persists slots, and deletes the task. |
| `calculateCommandCrc32(const String &command)` | Generates CRC32 used to namespace MQTT response topics per command. | Command string. | Returns 32-bit checksum. |
| `buildResponseTopic(const String &command)` | Composes MQTT topic `sylvester/<mac>/response/<CRC32>`. | Command string. | Returns topic string. |
| `sendCommandToTcpServer(const String &command, String &responseOut)` | Sends a raw request to the PLC over the persistent TCP socket, waiting up to 2 seconds for a newline response. | Command string; output response. | Updates `lastRequestSent`, `lastResponseReceived`, logs errors, and returns `true` on reply. Utilises `ensureServerConnection`. |
| `sendCommandAndMaybePublish(int slotIndex, const String &command, bool sendAlways)` | Wraps TCP send plus MQTT publish logic with change-detection when `sendAlways` is false. | Slot index (or `-1`), command, flag. | Publishes to MQTT if policy allows; caches last response in the slot. |
| `handleMqttRequest(const String &payload)` | Entry point for messages on `request` topic. Configures or executes scheduler slots according to metadata. | Raw payload string. | May run command immediately, update intervals, start/stop workers, persist slots, and logs outcomes. |
| `handleCommand(String command)` | Parses human-oriented management commands delivered via MQTT. | Command string. | Dispatches to specialised handlers (`handleFactoryResetCommand`, `handleWifiPasswordCommand`, `handlePortCommand`, `handleIpConfigCommand`, `performOtaUpdate`, `startNetworkScan`) and updates `lastCommandReceived`. |
| `handlePortCommand(String command)` | Updates the port list used for scanning. | `port` command payload. | Validates integers, persists to `Preferences`, refreshes scan. |
| `handleWifiPasswordCommand(String command)` | Changes the SoftAP password and restarts the access point. | `wifipassword <pass>` string. | Updates `Preferences`, disconnects clients, restarts AP, logs result. |
| `handleFactoryResetCommand()` | Clears all persisted settings and reboots. | None. | Disconnects AP, wipes `Preferences`, resets wired config defaults, calls `ESP.restart`. |
| `handleIpConfigCommand(String command)` | Applies wired Ethernet DHCP or static configuration via MQTT command. | `ipconfig` payload. | Validates mode/tokens, delegates to `setWiredConfiguration`, logs outcome. |

### 5. `ota.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `performOtaUpdate(const String &binUrl, const String &md5Url)` | Drives the complete OTA workflow: download MD5, validate format, download firmware, stream to flash, verify MD5, finalise update, reboot. | Firmware and MD5 URLs. | Calls helper functions for HTTP downloads (`beginHttpDownload`, `downloadTextFile`), uses Arduino `Update` API, logs each stage, restarts device on success. |
| `describeUpdateError()` *(static)* | Extracts a human-readable error message from `Update`. | None. | Returns string for logging. |
| `parseMd5FromContent(String content)` *(static)* | Reduces MD5 file payload to the first token and normalises case. | Raw MD5 file contents. | Returns 32-character lowercase MD5 string. |

### 6. `tcp_server.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `ensureServerConnection()` | Reconnect helper for the PLC TCP socket with bounded retries. | None. | Attempts up to three reconnects with 500 ms delay, logging progress. Returns `true` on success. |

### 7. `webpage.ino`
| Function | Description | Inputs | Outputs / Side-effects |
|----------|-------------|--------|------------------------|
| `escapeJson(String value)` | Escapes JSON-sensitive characters for dashboard payloads. | String value. | Returns escaped string. |
| `handleMonitor()` | Generates the live monitoring HTML page, including JavaScript that polls `/logs`. | None. | Sends HTTP 200 with full page markup via `server.send`. |
| `handleLogs()` | Streams JSON containing buffered logs and connection metadata. | None. | Serialises `logBuffer`, `deviceMac`, `wiredIP`, `internetAddress`, `last*` variables and serves via HTTP. |
| `handleCSS()` | Serves CSS for both monitor and config pages. | None. | Sends HTTP 200 with stylesheet. |
| `sendConfigPage(const String &message, bool success)` | Shared renderer for the Ethernet configuration form with optional status banner. | Message text, success flag. | Pulls wired settings via `isWiredDhcp`/`getWired*`, builds HTML form, and serves response. |
| `handleConfigPage()` | GET handler that shows the configuration form. | None. | Calls `sendConfigPage` without status message. |
| `handleConfigSubmit()` | POST handler that processes technician submissions, validates mode, and applies wired configuration. | HTTP form data. | Calls `setWiredConfiguration`, shows success/error message via `sendConfigPage`. |

---

## Event and Data Flows
1. **Startup Sequence**
   * `setup()` → `preferences.begin`, `loadWiredConfig`, `loadPersistedServerDetails` → `ETH.begin` + `applyWiredConfigToDriver`
   → if link up: derive MAC/topic base → `loadWifiSettings`, `loadPorts` → `setupAccessPoint`
   → create semaphores → `initializeCommandScheduler`, `loadPersistedCommandSlots` → configure MQTT client → `connectMQTT`
   → `startNetworkScan`.
2. **Network Discovery**
   * `startNetworkScan` spawns `scanTask` workers.
   * Each `scanTask` obtains next IP via `scanMutex`, tests each configured port, sends `(&V)` handshake, and on response:
     `serverMutex` protects `serverFound` transition, the persistent `client` connects, `persistServerDetails` records the target,
     and the task exits after flagging success.
3. **MQTT Command Handling**
   * `mqttCallback` routes `/command` messages to `handleCommand`, which fan out to specialised handlers (password, ports,
     scanning, OTA, wired config).
   * `/request` messages go to `handleMqttRequest`, which configures scheduler slots, launching `commandTask` via
     `startCommandWorker` as needed.
   * `commandTask` uses `sendCommandAndMaybePublish` → `sendCommandToTcpServer` → `ensureServerConnection` to interact with the PLC.
4. **HTTP Monitoring**
   * Browser loads `/` (`handleMonitor`), which polls `/logs` (`handleLogs`) each second for log lines and metadata.
   * `/config` GET returns the form; POST triggers `handleConfigSubmit`, which calls `setWiredConfiguration` to apply the
     requested mode, retrying with DHCP automatically if the static parameters fail.
5. **OTA Workflow**
   * MQTT `ota <binUrl> <md5Url>` → `handleCommand` → `performOtaUpdate`.
   * `performOtaUpdate` fetches MD5 (`downloadTextFile`), initiates firmware GET via `beginHttpDownload`, streams to flash with
     `Update.writeStream`, verifies MD5, and reboots.

---

## External Interfaces
* **MQTT Topics**
  * Subscribe: `sylvester/<mac>/command`, `sylvester/<mac>/request`.
  * Publish: `sylvester/<mac>/log`, `sylvester/<mac>/response/<CRC32>`.
* **HTTP Endpoints**
  * `/` (monitor), `/logs` (JSON), `/style.css` (CSS), `/config` (GET/POST).
* **SoftAP**
  * SSID: `sylvester-<mac-without-colons>`; password default `12345678`.
* **PLC TCP Session**
  * Maintained via `client`. Initial handshake message `(&V)` tests connectivity.

---

## Timing and Concurrency Considerations
* Eight `scanTask` workers share `scanMutex` and `serverMutex`. Once `serverFound` is set, all workers terminate.
* Command scheduler tasks inherit priority `1` and stack `4096` bytes. `maxCommandWorkers` caps concurrent workers at eight by
  default.
* `COMMAND_RESPONSE_TIMEOUT_MS` (2000 ms) bounds TCP response waits.
* `refreshPublicIP` runs every 5 minutes (`PUBLIC_IP_REFRESH_INTERVAL`) or when manually requested after link events.
* `ensureServerConnection` retries three times with 500 ms delay.

---

## Error Handling and Logging
* Every failure path invokes `addLog`, ensuring operators can trace issues via MQTT or the dashboard.
* Network configuration routines revert to safe defaults (DHCP) when invalid data is provided.
* OTA aborts cleanly on any validation error, leaving previous firmware intact.

---

## Persistence Layout (Preferences Namespace `wifi-config`)
| Key | Type | Description |
|-----|------|-------------|
| `apPassword` | String | Wi-Fi SoftAP password. |
| `numPorts`, `port0..port9` | Int | Port list for scanning. |
| `ethMode` | UChar | `0` for DHCP, `1` for static. |
| `ethIP`, `ethMask`, `ethGateway`, `ethDns` | String | Wired static configuration. |
| `srvIP`, `srvPort` | String / Int | Last discovered PLC target. |
| `reqSlot<i>InUse`, `reqSlot<i>Cmd`, `reqSlot<i>Interval`, `reqSlot<i>Always`, `reqSlot<i>Active` | Mixed | Scheduler slot persistence for up to eight commands. |

---

## Reimplementation Notes
* Any reimplementation must honour the concurrency model: shared state guarded by mutexes, and worker tasks triggered by
  MQTT request metadata.
* Logging should mirror `addLog` semantics to keep MQTT and dashboard experiences consistent.
* The handshake `(&V)` and newline-delimited responses are the only PLC protocol assumptions baked into the scanner.
* Ensure OTA downloads support HTTP and HTTPS, optionally skipping certificate validation (`otaRootCACertificate == nullptr`).
* Static configuration commands must validate IP syntax identically to avoid inconsistent persistence.

---

## Revision History
* Generated from repository commit inspected on container execution.
