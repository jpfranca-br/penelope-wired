// network.ino

#include <ETH.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
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
extern const char* const PREF_KEY_SERVER_IP;
extern const char* const PREF_KEY_SERVER_PORT;
extern const char* otaRootCACertificate;

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
      Serial.println("ETH iniciado");
      ETH.setHostname("sylvester-eth");
      break;
    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("ETH conectado");
      break;
    case ARDUINO_EVENT_ETH_GOT_IP:
      Serial.println("ETH obteve IP");
      Serial.print("Endereço IP: ");
      Serial.println(ETH.localIP());
      Serial.print("MAC: ");
      Serial.println(ETH.macAddress());
      eth_connected = true;
      deviceMac = ETH.macAddress();
      wiredIP = ETH.localIP().toString();
      publicIPRefreshRequested = true;
      addLog("Ethernet conectada - IP: " + ETH.localIP().toString());
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH perdeu IP");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet perdeu IP");
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH desconectado");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet desconectada");
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH parado");
      eth_connected = false;
      wiredIP = "";
      internetAddress = "";
      publicIPRefreshRequested = false;
      lastPublicIPCheck = 0;
      addLog("Ethernet parada");
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

void loadPersistedServerDetails() {
  String savedIP = preferences.getString(PREF_KEY_SERVER_IP, "");
  int savedPort = preferences.getInt(PREF_KEY_SERVER_PORT, 0);

  if (savedIP.length() > 0 && savedPort > 0 && savedPort <= 65535) {
    serverIP = savedIP;
    serverPort = savedPort;
    addLog("Destino de servidor salvo carregado: " + serverIP + ":" + String(serverPort));
  } else {
    if (savedIP.length() > 0 || savedPort != 0) {
      preferences.putString(PREF_KEY_SERVER_IP, "");
      preferences.putInt(PREF_KEY_SERVER_PORT, 0);
    }
    serverIP = "";
    serverPort = 0;
  }
}

void persistServerDetails(const String &ip, int port) {
  preferences.putString(PREF_KEY_SERVER_IP, ip);
  preferences.putInt(PREF_KEY_SERVER_PORT, port);
  addLog("Detalhes do servidor salvos: " + ip + ":" + String(port));
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
      addLog("Configuração Ethernet armazenada inválida. Revertendo para DHCP.");
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
        addLog("Ethernet configurada para DHCP");
      } else {
        addLog("Falha ao configurar Ethernet para DHCP");
      }
    }
  } else {
    bool result = ETH.config(wiredStaticIP, wiredStaticGateway, wiredStaticMask, wiredStaticDns);
    if (logOutcome) {
      if (result) {
        addLog("Ethernet configurada com IP estático " + wiredStaticIPStr);
      } else {
        addLog("Falha ao aplicar configuração Ethernet estática");
      }
    }
  }
}

void reconnectEthernetWithConfig() {
  addLog("Reinicializando a interface Ethernet para aplicar a configuração...");

  bool wasConnected = eth_connected;
  eth_connected = false;
  wiredIP = "";
  internetAddress = "";
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
    addLog("Ethernet pronta - IP: " + wiredIP);
    publicIPRefreshRequested = true;
  } else {
    addLog("A reconfiguração da Ethernet expirou");
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
      errorMessage = "Endereço IP inválido";
      return false;
    }
    if (!parseIPAddress(maskTrim, parsedMask)) {
      errorMessage = "Máscara de sub-rede inválida";
      return false;
    }
    if (!parseIPAddress(gatewayTrim, parsedGateway)) {
      errorMessage = "Endereço de gateway inválido";
      return false;
    }
    if (!parseIPAddress(dnsTrim, parsedDns)) {
      errorMessage = "Endereço de DNS inválido";
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
    addLog("Aplicando configuração DHCP para Ethernet");
  } else {
    addLog("Aplicando configuração Ethernet estática: " + wiredStaticIPStr + " / " + wiredStaticMaskStr);
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
    addLog("Senha WiFi carregada da memória");
  } else {
    ap_password = DEFAULT_AP_PASSWORD;
    if (storedPassword.length() > 0 && storedPassword != DEFAULT_AP_PASSWORD) {
      addLog("Senha WiFi armazenada inválida. Revertendo para o padrão");
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
    server.send(404, "text/plain", "Não encontrado");
  });
  server.begin();

  Serial.print("Ponto de acesso iniciado: ");
  Serial.println(ap_ssid);
  Serial.print("IP do AP: ");
  Serial.println(apIP);

  addLog("WiFi AP pronto: " + ap_ssid + " @ " + apIP.toString());
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
    addLog("Falha ao iniciar solicitação de IP público");
    return;
  }

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    String ip = http.getString();
    ip.trim();
    if (ip.length() > 0) {
      internetAddress = ip;
    } else {
      addLog("Resposta de IP público vazia recebida");
    }
  } else {
    addLog("Solicitação de IP público falhou: HTTP " + String(httpCode));
  }

  http.end();
}

void startNetworkScan() {
  if (!eth_connected) {
    addLog("Não é possível iniciar a varredura - Ethernet não conectada");
    return;
  }

  IPAddress localIP = ETH.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    addLog("Não é possível iniciar a varredura - IP da Ethernet não atribuído");
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

  addLog("Iniciando varredura de rede multithread...");
  addLog("Intervalo de varredura: " + scanRange);
  addLog("Utilizando " + String(NUM_SCAN_TASKS) + " threads paralelas");

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

      String progress = "Varrendo " + startIP.toString() + " até " + endIP.toString() +
                       " - Portas " + portList;
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
        Serial.print("\n[Encontrado] ");
        Serial.print(targetIP);
        Serial.print(":");
        Serial.print(ports[p]);
        Serial.print(" ABERTA - Testando... ");

        testClient.println("(&V)");
        testClient.flush();

        unsigned long startTime = millis();
        while (!testClient.available() && (millis() - startTime < 1000)) {
          delay(10);
        }

        if (testClient.available()) {
          String response = testClient.readStringUntil('\n');
          Serial.print("RESPOSTA: ");
          Serial.println(response);

          xSemaphoreTake(serverMutex, portMAX_DELAY);
          if (!serverFound) {
            serverIP = targetIP.toString();
            serverPort = ports[p];
            serverFound = true;

            client.connect(targetIP, ports[p]);
            lastCommandTime = millis();

            addLog("*** SERVIDOR ENCONTRADO! ***");
            addLog("Servidor: " + serverIP + ":" + String(serverPort));
            persistServerDetails(serverIP, serverPort);
            addLog("Parando todas as threads de varredura...");
          }
          xSemaphoreGive(serverMutex);

          testClient.stop();
          vTaskDelete(NULL);
          return;
        } else {
          Serial.println("sem resposta");
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
    Serial.println("Utilizando portas padrão: 2001, 1771, 854");
  } else {
    for (int i = 0; i < numPorts; i++) {
      String key = "port" + String(i);
      ports[i] = preferences.getInt(key.c_str(), defaultPorts[i % defaultNumPorts]);
    }
    Serial.print("Portas carregadas da memória: ");
    for (int i = 0; i < numPorts; i++) {
      Serial.print(ports[i]);
      if (i < numPorts - 1) Serial.print(", ");
    }
    Serial.println();
  }

  yield();
}

bool beginHttpDownload(const String &url, HTTPClient &http, WiFiClient *&clientOut, String &errorMessage) {
  clientOut = nullptr;
  String trimmedUrl = url;
  trimmedUrl.trim();

  if (trimmedUrl.length() == 0) {
    errorMessage = "URL vazia";
    return false;
  }

  String urlLower = trimmedUrl;
  urlLower.toLowerCase();

  bool isHttps = urlLower.startsWith("https://");
  bool isHttp = urlLower.startsWith("http://");

  if (!isHttp && !isHttps) {
    errorMessage = "URL deve iniciar com http:// ou https://";
    return false;
  }

  http.end();
  http.setTimeout(15000);
#if defined(HTTPCLIENT_1_1_COMPATIBLE)
  http.useHTTP10(false);
#endif
#ifdef HTTPC_STRICT_FOLLOW_REDIRECTS
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
#endif

  if (isHttps) {
    static WiFiClientSecure secureClient;
    secureClient.stop();

    if (otaRootCACertificate != nullptr && otaRootCACertificate[0] != '\0') {
      if (!secureClient.setCACert(otaRootCACertificate)) {
        errorMessage = "Falha ao aplicar certificado raiz";
        return false;
      }
    } else {
      secureClient.setInsecure();
    }

    if (!http.begin(secureClient, trimmedUrl)) {
      errorMessage = "Falha ao iniciar conexão HTTPS";
      return false;
    }

    clientOut = &secureClient;
  } else {
    static WiFiClient plainClient;
    plainClient.stop();

    if (!http.begin(plainClient, trimmedUrl)) {
      errorMessage = "Falha ao iniciar conexão HTTP";
      return false;
    }

    clientOut = &plainClient;
  }

  return true;
}

bool downloadTextFile(const String &url, String &content, String &errorMessage) {
  HTTPClient http;
  WiFiClient *client = nullptr;

  if (!beginHttpDownload(url, http, client, errorMessage)) {
    return false;
  }

  (void)client;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    String reason = http.errorToString(httpCode);
    errorMessage = "HTTP " + String(httpCode);
    if (reason.length() > 0) {
      errorMessage += " - " + reason;
    }
    http.end();
    return false;
  }

  content = http.getString();
  http.end();
  return true;
}
