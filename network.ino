// network.ino

#include <ETH.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WebServer.h>
#include <PubSubClient.h>

extern volatile bool eth_connected;
extern bool ethPreviouslyConnected;
extern bool serverFound;
extern bool publicIPRefreshRequested;
extern volatile int currentScanIP;
extern volatile bool scanComplete;
extern unsigned long lastPublicIPCheck;
extern unsigned long lastCommandTime;
extern String wiredIP;
extern String internetAddress;
extern String runningTotal;
extern String serverIP;
extern int serverPort;
extern String deviceMac;
extern Preferences preferences;
extern WebServer server;
extern WiFiClient client;
extern PubSubClient mqttClient;
extern SemaphoreHandle_t serverMutex;
extern SemaphoreHandle_t scanMutex;
extern String ap_ssid;
extern const char* const DEFAULT_AP_PASSWORD;
extern String ap_password;
extern int ports[10];
extern int numPorts;
extern const int defaultPorts[];
extern const int defaultNumPorts;
extern bool wiredDhcpEnabled;
extern IPAddress wiredStaticIP;
extern IPAddress wiredStaticMask;
extern IPAddress wiredStaticGateway;
extern IPAddress wiredStaticDns;
extern String wiredStaticIPStr;
extern String wiredStaticMaskStr;
extern String wiredStaticGatewayStr;
extern String wiredStaticDnsStr;
extern void persistServerDetails(const String &ip, int port);

void addLog(String message);
void handleMonitor();
void handleLogs();
void handleCSS();
void handleConfigPage();
void handleConfigSubmit();

static void scanTask(void *parameter);

#define NUM_SCAN_TASKS 8

void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname("sylvester-eth");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH Connected");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH Got IP");
      Serial.print("IP Address: ");
      Serial.println(ETH.localIP());
      Serial.print("MAC: ");
      Serial.println(ETH.macAddress());
      eth_connected = true;
      deviceMac = ETH.macAddress();
      wiredIP = ETH.localIP().toString();
      publicIPRefreshRequested = true;
      addLog("Ethernet connected - IP: " + ETH.localIP().toString());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      runningTotal = "None";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet lost IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      runningTotal = "None";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet disconnected");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      runningTotal = "None";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet stopped");
      break;
    default:
      break;
  }
}

bool parseIPAddress(const String &value, IPAddress &out) {
  String trimmed = value;
  trimmed.trim();
  if (trimmed.length() == 0) {
    return false;
  }

  IPAddress parsed;
  if (!parsed.fromString(trimmed)) {
    return false;
  }

  out = parsed;
  return true;
}

bool isWiredDhcp() {
  return wiredDhcpEnabled;
}

String getWiredIpSetting() {
  return wiredStaticIPStr;
}

String getWiredMaskSetting() {
  return wiredStaticMaskStr;
}

String getWiredGatewaySetting() {
  return wiredStaticGatewayStr;
}

String getWiredDnsSetting() {
  return wiredStaticDnsStr;
}

void loadWiredConfig() {
  uint8_t mode = preferences.getUChar("ethMode", 0);
  wiredDhcpEnabled = (mode == 0);

  wiredStaticIPStr = preferences.getString("ethIP", "");
  wiredStaticMaskStr = preferences.getString("ethMask", "");
  wiredStaticGatewayStr = preferences.getString("ethGateway", "");
  wiredStaticDnsStr = preferences.getString("ethDns", "");

  if (!wiredDhcpEnabled) {
    bool ipValid = parseIPAddress(wiredStaticIPStr, wiredStaticIP);
    bool maskValid = parseIPAddress(wiredStaticMaskStr, wiredStaticMask);
    bool gatewayValid = parseIPAddress(wiredStaticGatewayStr, wiredStaticGateway);
    bool dnsValid = parseIPAddress(wiredStaticDnsStr, wiredStaticDns);

    if (!ipValid || !maskValid || !gatewayValid || !dnsValid) {
      addLog("Stored Ethernet configuration invalid. Reverting to DHCP.");
      wiredDhcpEnabled = true;
      wiredStaticIP = IPAddress(0, 0, 0, 0);
      wiredStaticMask = IPAddress(255, 255, 255, 0);
      wiredStaticGateway = IPAddress(0, 0, 0, 0);
      wiredStaticDns = IPAddress(0, 0, 0, 0);
      wiredStaticIPStr = "";
      wiredStaticMaskStr = "";
      wiredStaticGatewayStr = "";
      wiredStaticDnsStr = "";
      preferences.putUChar("ethMode", 0);
      preferences.putString("ethIP", "");
      preferences.putString("ethMask", "");
      preferences.putString("ethGateway", "");
      preferences.putString("ethDns", "");
    }
  } else {
    wiredStaticIP = IPAddress(0, 0, 0, 0);
    wiredStaticMask = IPAddress(255, 255, 255, 0);
    wiredStaticGateway = IPAddress(0, 0, 0, 0);
    wiredStaticDns = IPAddress(0, 0, 0, 0);
  }
}

void applyWiredConfigToDriver(bool logOutcome) {
  if (wiredDhcpEnabled) {
    bool result = ETH.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
    if (logOutcome) {
      if (result) {
        addLog("Ethernet configured for DHCP");
      } else {
        addLog("Failed to configure Ethernet for DHCP");
      }
    }
  } else {
    bool result = ETH.config(wiredStaticIP, wiredStaticGateway, wiredStaticMask, wiredStaticDns);
    if (logOutcome) {
      if (result) {
        addLog("Ethernet configured with static IP " + wiredStaticIPStr);
      } else {
        addLog("Failed to apply static Ethernet configuration");
      }
    }
  }
}

void reconnectEthernetWithConfig() {
  addLog("Reinitializing Ethernet interface to apply configuration...");

  bool wasConnected = eth_connected;
  eth_connected = false;
  wiredIP = "";
  internetAddress = "";
  runningTotal = "None";
  publicIPRefreshRequested = false;
  lastPublicIPCheck = 0;

  if (wasConnected) {
    mqttClient.disconnect();
    if (client.connected()) {
      client.stop();
    }
  }

  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
  applyWiredConfigToDriver(true);

  ethPreviouslyConnected = false;

  int eth_wait = 0;
  while (!eth_connected && eth_wait < 40) {
    delay(250);
    yield();
    eth_wait++;
  }

  if (eth_connected) {
    wiredIP = ETH.localIP().toString();
    addLog("Ethernet ready - IP: " + wiredIP);
    publicIPRefreshRequested = true;
  } else {
    addLog("Ethernet reconfiguration timed out");
  }
}

bool setWiredConfiguration(bool useDhcp, const String &ip, const String &mask, const String &gateway, const String &dns, String &errorMessage) {
  if (useDhcp) {
    wiredDhcpEnabled = true;
    wiredStaticIP = IPAddress(0, 0, 0, 0);
    wiredStaticMask = IPAddress(255, 255, 255, 0);
    wiredStaticGateway = IPAddress(0, 0, 0, 0);
    wiredStaticDns = IPAddress(0, 0, 0, 0);
    wiredStaticIPStr = "";
    wiredStaticMaskStr = "";
    wiredStaticGatewayStr = "";
    wiredStaticDnsStr = "";
  } else {
    String ipTrim = ip;
    String maskTrim = mask;
    String gatewayTrim = gateway;
    String dnsTrim = dns;
    ipTrim.trim();
    maskTrim.trim();
    gatewayTrim.trim();
    dnsTrim.trim();

    IPAddress parsedIP;
    IPAddress parsedMask;
    IPAddress parsedGateway;
    IPAddress parsedDns;

    if (!parseIPAddress(ipTrim, parsedIP)) {
      errorMessage = "Invalid IP address";
      return false;
    }
    if (!parseIPAddress(maskTrim, parsedMask)) {
      errorMessage = "Invalid subnet mask";
      return false;
    }
    if (!parseIPAddress(gatewayTrim, parsedGateway)) {
      errorMessage = "Invalid gateway address";
      return false;
    }
    if (!parseIPAddress(dnsTrim, parsedDns)) {
      errorMessage = "Invalid DNS address";
      return false;
    }

    wiredDhcpEnabled = false;
    wiredStaticIP = parsedIP;
    wiredStaticMask = parsedMask;
    wiredStaticGateway = parsedGateway;
    wiredStaticDns = parsedDns;
    wiredStaticIPStr = ipTrim;
    wiredStaticMaskStr = maskTrim;
    wiredStaticGatewayStr = gatewayTrim;
    wiredStaticDnsStr = dnsTrim;
  }

  if (wiredDhcpEnabled) {
    addLog("Applying DHCP configuration for Ethernet");
  } else {
    addLog("Applying static Ethernet configuration: " + wiredStaticIPStr + " / " + wiredStaticMaskStr);
  }

  preferences.putUChar("ethMode", wiredDhcpEnabled ? 0 : 1);
  preferences.putString("ethIP", wiredStaticIPStr);
  preferences.putString("ethMask", wiredStaticMaskStr);
  preferences.putString("ethGateway", wiredStaticGatewayStr);
  preferences.putString("ethDns", wiredStaticDnsStr);

  reconnectEthernetWithConfig();
  return true;
}

void loadWifiSettings() {
  String storedPassword = preferences.getString("apPassword", DEFAULT_AP_PASSWORD);
  if (storedPassword.length() >= 8 && storedPassword.length() <= 63) {
    ap_password = storedPassword;
    addLog("Loaded WiFi password from memory");
  } else {
    ap_password = DEFAULT_AP_PASSWORD;
    if (storedPassword.length() > 0 && storedPassword != DEFAULT_AP_PASSWORD) {
      addLog("Stored WiFi password invalid. Reverting to default");
    }
  }
}

void setupAccessPoint() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid.c_str(), ap_password.c_str());

  IPAddress apIP = WiFi.softAPIP();

  server.on("/", handleMonitor);
  server.on("/logs", handleLogs);
  server.on("/style.css", handleCSS);
  server.on("/config", HTTP_GET, handleConfigPage);
  server.on("/config", HTTP_POST, handleConfigSubmit);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
  server.begin();

  Serial.print("Access point started: ");
  Serial.println(ap_ssid);
  Serial.print("AP IP address: ");
  Serial.println(apIP);

  addLog("WiFi AP ready: " + ap_ssid + " @ " + apIP.toString());
}

void refreshPublicIP() {
  if (!eth_connected) {
    publicIPRefreshRequested = true;
    return;
  }

  publicIPRefreshRequested = false;
  lastPublicIPCheck = millis();

  HTTPClient http;
  http.setTimeout(4000);
  if (!http.begin("http://api.ipify.org")) {
    addLog("Failed to start public IP request");
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String ip = http.getString();
    ip.trim();
    if (ip.length() > 0) {
      internetAddress = ip;
    } else {
      addLog("Received empty public IP response");
    }
  } else {
    addLog("Public IP request failed: HTTP " + String(httpCode));
  }

  http.end();
}

void startNetworkScan() {
  if (!eth_connected) {
    addLog("Cannot start scan - Ethernet not connected");
    return;
  }

  IPAddress localIP = ETH.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    addLog("Cannot start scan - Ethernet IP not assigned");
    return;
  }

  IPAddress subnet = ETH.subnetMask();
  IPAddress network;

  for (int i = 0; i < 4; i++) {
    network[i] = localIP[i] & subnet[i];
  }

  String prefix = String(network[0]) + "." + String(network[1]) + "." + String(network[2]) + ".";
  String scanRange = prefix + "1 - " + prefix + "254";

  currentScanIP = 1;
  scanComplete = false;

  addLog("Starting multi-threaded network scan...");
  addLog("Scan range: " + scanRange);
  addLog("Using " + String(NUM_SCAN_TASKS) + " parallel threads");

  for (int i = 0; i < NUM_SCAN_TASKS; i++) {
    char taskName[20];
    sprintf(taskName, "ScanTask%d", i);
    xTaskCreate(
      scanTask,
      taskName,
      4096,
      NULL,
      1,
      NULL
    );
  }
}

static void scanTask(void *parameter) {
  IPAddress localIP = ETH.localIP();
  IPAddress subnet = ETH.subnetMask();
  IPAddress network;

  for (int i = 0; i < 4; i++) {
    network[i] = localIP[i] & subnet[i];
  }

  while (true) {
    if (!eth_connected) {
      vTaskDelete(NULL);
      return;
    }

    xSemaphoreTake(serverMutex, portMAX_DELAY);
    bool found = serverFound;
    xSemaphoreGive(serverMutex);

    if (found) {
      vTaskDelete(NULL);
      return;
    }

    xSemaphoreTake(scanMutex, portMAX_DELAY);
    int ipToScan = currentScanIP++;
    if (ipToScan >= 255) {
      scanComplete = true;
      xSemaphoreGive(scanMutex);
      vTaskDelete(NULL);
      return;
    }
    if (ipToScan % 10 == 0) {
      IPAddress startIP = network;
      startIP[3] = ipToScan;
      IPAddress endIP = network;
      endIP[3] = min(ipToScan + 9, 254);

      String portList = "";
      for (int p = 0; p < numPorts; p++) {
        portList += String(ports[p]);
        if (p < numPorts - 1) portList += ", ";
      }

      String progress = "Scanning " + startIP.toString() + " to " + endIP.toString() +
                       " - Ports " + portList;
      addLog(progress);
    }
    xSemaphoreGive(scanMutex);

    IPAddress targetIP = network;
    targetIP[3] = ipToScan;

    if (targetIP == localIP) continue;

    Serial.print(".");
    if (ipToScan % 50 == 0) Serial.println();

    WiFiClient testClient;
    for (int p = 0; p < numPorts; p++) {
      testClient.setTimeout(300);
      if (testClient.connect(targetIP, ports[p], 300)) {
        Serial.print("\n[Found] ");
        Serial.print(targetIP);
        Serial.print(":");
        Serial.print(ports[p]);
        Serial.print(" OPEN - Testing... ");

        testClient.println("(&V)");
        testClient.flush();

        unsigned long startTime = millis();
        while (!testClient.available() && (millis() - startTime < 1000)) {
          delay(10);
        }

        if (testClient.available()) {
          String response = testClient.readStringUntil('\n');
          Serial.print("RESPONSE: ");
          Serial.println(response);

          xSemaphoreTake(serverMutex, portMAX_DELAY);
          if (!serverFound) {
            serverIP = targetIP.toString();
            serverPort = ports[p];
            serverFound = true;

            client.connect(targetIP, ports[p]);
            lastCommandTime = millis();

            addLog("*** SERVER FOUND! ***");
            addLog("Server: " + serverIP + ":" + String(serverPort));
            persistServerDetails(serverIP, serverPort);
            addLog("Stopping all scan threads...");
          }
          xSemaphoreGive(serverMutex);

          testClient.stop();
          vTaskDelete(NULL);
          return;
        } else {
          Serial.println("no response");
        }

        testClient.stop();
      }
    }

    delay(10);
  }
}

void loadPorts() {
  yield();

  numPorts = preferences.getInt("numPorts", 0);

  if (numPorts == 0 || numPorts > 10) {
    numPorts = defaultNumPorts;
    for (int i = 0; i < numPorts; i++) {
      ports[i] = defaultPorts[i];
    }
    Serial.println("Using default ports: 2001, 1771, 854");
  } else {
    for (int i = 0; i < numPorts; i++) {
      String key = "port" + String(i);
      ports[i] = preferences.getInt(key.c_str(), defaultPorts[i % defaultNumPorts]);
    }
    Serial.print("Loaded ports from memory: ");
    for (int i = 0; i < numPorts; i++) {
      Serial.print(ports[i]);
      if (i < numPorts - 1) Serial.print(", ");
    }
    Serial.println();
  }

  yield();
}
