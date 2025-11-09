// wemos32_separated.ino
#include <Preferences.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <WiFi.h>

// Ethernet configuration for WT32-ETH01
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_PHY_ADDR    1
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_POWER   16
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN

#include <ETH.h>

volatile bool eth_connected = false;

// Log buffer shared across modules
const int MAX_LOG_LINES = 200;
String logBuffer[MAX_LOG_LINES];
int logIndex = 0;
int logCount = 0;

// Forward declarations
void addLog(String message);
void handleMonitor();
void handleLogs();
void handleCSS();
void handleConfigPage();
void handleConfigSubmit();
void connectMQTT();
void initializeCommandScheduler();
void loadPersistedCommandSlots();
void startNetworkScan();
void setupAccessPoint();
void loadWifiSettings();
void loadWiredConfig();
void loadPersistedServerDetails();
void persistServerDetails(const String &ip, int port);
bool setWiredConfiguration(bool useDhcp, const String &ip, const String &mask, const String &gateway, const String &dns, String &errorMessage);
bool isWiredDhcp();
String getWiredIpSetting();
String getWiredMaskSetting();
String getWiredGatewaySetting();
String getWiredDnsSetting();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void loadPorts();
void refreshPublicIP();
void applyWiredConfigToDriver(bool logOutcome);
void reconnectEthernetWithConfig();

inline void logMessage(const String &message) {
  addLog(message);
}

// MQTT Broker settings
const char* mqtt_broker = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";

WebServer server(80);
Preferences preferences;
String ap_ssid = "sylvester-";
const char* const DEFAULT_AP_PASSWORD = "12345678";
String ap_password = DEFAULT_AP_PASSWORD;

// Root CA used for OTA HTTPS downloads. Set to nullptr to allow insecure certificates.
const char* otaRootCACertificate = nullptr;
//const char* otaRootCACertificate = R"EOF(
//-----BEGIN CERTIFICATE-----
//MIIF...
//-----END CERTIFICATE-----
//)EOF";

// MQTT Client
WiFiClient espClient;
PubSubClient mqttClient(espClient);
String macAddress = "";
String deviceMac = "";
String mqttTopicBase = "";
String wiredIP = "";
String internetAddress = "";
String lastCommandReceived = "Nenhum";
String lastRequestSent = "Nenhum";
String lastResponseReceived = "Nenhum";
unsigned long lastCommandTime = 0;

const char* const PREF_KEY_SERVER_IP = "srvIP";
const char* const PREF_KEY_SERVER_PORT = "srvPort";

bool wiredDhcpEnabled = true;
IPAddress wiredStaticIP(0, 0, 0, 0);
IPAddress wiredStaticMask(255, 255, 255, 0);
IPAddress wiredStaticGateway(0, 0, 0, 0);
IPAddress wiredStaticDns(0, 0, 0, 0);
String wiredStaticIPStr = "";
String wiredStaticMaskStr = "";
String wiredStaticGatewayStr = "";
String wiredStaticDnsStr = "";

unsigned long lastPublicIPCheck = 0;
const unsigned long PUBLIC_IP_REFRESH_INTERVAL = 300000; // 5 minutes
bool publicIPRefreshRequested = false;

// Ports to scan
int ports[10];
int numPorts = 3;
const int defaultPorts[] = {2001, 1771, 854};
const int defaultNumPorts = 3;

// Server connection details
WiFiClient client;
String serverIP = "";
int serverPort = 0;
bool serverFound = false;
SemaphoreHandle_t serverMutex;

bool ethPreviouslyConnected = false;
unsigned long lastEthStatusLog = 0;

// Timing
// Scanning
volatile int currentScanIP = 1;
volatile bool scanComplete = false;
SemaphoreHandle_t scanMutex;

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nWT32-ETH01 Multi-threaded Network Scanner");

  preferences.begin("wifi-config", false);

  macAddress.replace(":", "");
  macAddress.toLowerCase();
  ap_ssid = "sylvester-" + macAddress;
  mqttTopicBase = "sylvester/" + macAddress + "/";
  loadWifiSettings();
  setupAccessPoint();
  
  loadWiredConfig();
  loadPersistedServerDetails();

  // Setup Ethernet
  Serial.println("Iniciando Ethernet cabeada...");
  WiFi.onEvent(onEvent);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
  applyWiredConfigToDriver(true);

  // Wait for Ethernet connection with proper delay
  int eth_wait = 0;
  while (!eth_connected && eth_wait < 40) {  // Increased from 20 to 40
    delay(250);  // Shorter delay
    yield();     // CRITICAL: Feed the watchdog!
    Serial.print(".");
    eth_wait++;
  }
  Serial.println();
  
  if (eth_connected) {
    Serial.println("Ethernet conectada!");
    macAddress = ETH.macAddress();
    deviceMac = macAddress;
    Serial.print("MAC: ");
    Serial.println(macAddress);
    Serial.print("IP: ");
    Serial.println(ETH.localIP());
    wiredIP = ETH.localIP().toString();
    publicIPRefreshRequested = true;
  } else {
    Serial.println("Tempo limite da conexão Ethernet!");
    // Continue anyway - might connect later
    macAddress = ETH.macAddress();
    deviceMac = macAddress;
  }

  loadPorts();
  
  // Wait a bit more for Ethernet to stabilize
  if (!eth_connected) {
    Serial.println("Aguardando a estabilização da Ethernet...");
    for (int i = 0; i < 20; i++) {
      delay(250);
      yield();
      if (eth_connected) break;
    }
  }
  
  if (eth_connected) {
    logMessage("WT32-ETH01 Scanner de Rede pronto");
    logMessage("Endereço MAC: " + deviceMac);
    logMessage("Endereço IP: " + ETH.localIP().toString());
    publicIPRefreshRequested = true;
  } else {
    Serial.println("Iniciando sem conexão Ethernet...");
    logMessage("Aguardando conexão Ethernet...");
  }

  serverMutex = xSemaphoreCreateMutex();
  initializeCommandScheduler();
  loadPersistedCommandSlots();
  scanMutex = xSemaphoreCreateMutex();

  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);

  ethPreviouslyConnected = eth_connected;

  // Only connect MQTT and scan if Ethernet is up
  if (eth_connected) {
    connectMQTT();
    startNetworkScan();
  }
}
void loop() {
  server.handleClient();

  if (eth_connected && !ethPreviouslyConnected) {
    logMessage("Link Ethernet restaurado - IP: " + ETH.localIP().toString());
    wiredIP = ETH.localIP().toString();
    publicIPRefreshRequested = true;
    mqttClient.disconnect();
    if (client.connected()) {
      client.stop();
    }
    xSemaphoreTake(serverMutex, portMAX_DELAY);
    serverFound = false;
    serverIP = "";
    serverPort = 0;
    internetAddress = "";
    xSemaphoreGive(serverMutex);

    currentScanIP = 1;
    scanComplete = false;
    connectMQTT();
    startNetworkScan();
  } else if (!eth_connected && ethPreviouslyConnected) {
    logMessage("Link Ethernet perdido. Aguardando reconexão...");
    mqttClient.disconnect();
    if (client.connected()) {
      client.stop();
    }
    publicIPRefreshRequested = false;
    lastPublicIPCheck = 0;
  }

  ethPreviouslyConnected = eth_connected;

  if (!eth_connected) {
    unsigned long now = millis();
    if (now - lastEthStatusLog > 5000) {
      addLog("Ethernet desconectada - tentando novamente");
      lastEthStatusLog = now;
    }
    delay(100);
    return;
  }

  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();

  if (eth_connected) {
    unsigned long now = millis();
    bool intervalElapsed = (lastPublicIPCheck == 0) || (now - lastPublicIPCheck >= PUBLIC_IP_REFRESH_INTERVAL);
    if (publicIPRefreshRequested || intervalElapsed) {
      refreshPublicIP();
    }
  }

  if (!serverFound && scanComplete) {
    logMessage("Varredura concluída. Nenhum servidor encontrado. Reiniciando em 5 segundos...");
    delay(5000);
    currentScanIP = 1;
    scanComplete = false;
    startNetworkScan();
  }

  delay(100);
}

