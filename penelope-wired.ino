// wemos32_separated.ino
#include <WiFi.h>
#include <PubSubClient.h>
#include <WebServer.h>
#include <Preferences.h>

// Ethernet configuration for WT32-ETH01
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_PHY_ADDR    1
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_POWER   16
#define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN

#include <ETH.h>

static bool eth_connected = false;

// WiFi credentials
String ssid = "";
String password = "";

// MQTT Broker settings
const char* mqtt_broker = "mqtt.jpfranca.com";
const int mqtt_port = 1883;
const char* mqtt_user = "";
const char* mqtt_password = "";

// Configuration Portal
WebServer server(80);
Preferences preferences;
bool configMode = false;
String ap_ssid = "penelope-";
const char* ap_password = "12345678";

// Boot button configuration (WT32-ETH01 doesn't have one - connect external or disable)
const int BOOT_BUTTON_PIN = 9;
const unsigned long LONG_PRESS_TIME = 5000;

// MQTT Client
WiFiClient espClient;
PubSubClient mqttClient(espClient);
String macAddress = "";
String mqttTopicBase = "";

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

// Timing
unsigned long lastCommandTime = 0;
const unsigned long commandInterval = 5000;

// Scanning
#define NUM_SCAN_TASKS 8
volatile int currentScanIP = 1;
volatile bool scanComplete = false;
SemaphoreHandle_t scanMutex;

// Ethernet event handler
void onEvent(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("ETH Started");
      ETH.setHostname("penelope-eth");
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
      break;
    case ARDUINO_EVENT_ETH_LOST_IP:
      Serial.println("ETH Lost IP");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_DISCONNECTED:
      Serial.println("ETH Disconnected");
      eth_connected = false;
      break;
    case ARDUINO_EVENT_ETH_STOP:
      Serial.println("ETH Stopped");
      eth_connected = false;
      break;
    default:
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\nWT32-ETH01 Multi-threaded Network Scanner");

  // Setup Ethernet
  Serial.println("Starting Wired Ethernet...");
  Network.onEvent(onEvent);
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_POWER, ETH_CLK_MODE);
  
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
    Serial.println("Ethernet connected!");
    macAddress = ETH.macAddress();
    Serial.print("MAC: ");
    Serial.println(macAddress);
    Serial.print("IP: ");
    Serial.println(ETH.localIP());
  } else {
    Serial.println("Ethernet connection timeout!");
    // Continue anyway - might connect later
    macAddress = ETH.macAddress();
  }
  
  macAddress.replace(":", "");
  macAddress.toLowerCase();
  ap_ssid = "penelope-" + macAddress;
  mqttTopicBase = "penelope/" + macAddress + "/";
  
  // Skip boot button check if not connected to avoid blocking
  // pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
  // delay(1000);
  // checkBootButton();
  
  preferences.begin("wifi-config", false);
  loadPorts();
  
  // Wait a bit more for Ethernet to stabilize
  if (!eth_connected) {
    Serial.println("Waiting for Ethernet to stabilize...");
    for (int i = 0; i < 20; i++) {
      delay(250);
      yield();
      if (eth_connected) break;
    }
  }
  
  if (eth_connected) {
    logMessage("WT32-ETH01 Network Scanner Ready");
    logMessage("MAC Address: " + macAddress);
    logMessage("IP Address: " + ETH.localIP().toString());
  } else {
    Serial.println("Starting without Ethernet connection...");
  }
  
  serverMutex = xSemaphoreCreateMutex();
  scanMutex = xSemaphoreCreateMutex();
  
  mqttClient.setServer(mqtt_broker, mqtt_port);
  mqttClient.setCallback(mqttCallback);
  
  // Only connect MQTT and scan if Ethernet is up
  if (eth_connected) {
    connectMQTT();
    startNetworkScan();
  }
}
void loop() {
  if (configMode) {
    server.handleClient();
    return;
  }
  
  if (!eth_connected && WiFi.status() != WL_CONNECTED) {
    logMessage("Network disconnected! Reconnecting...");
    if (!connectToWiFi()) {
      startConfigPortal();
      return;
    }
  }
  
  if (!mqttClient.connected()) {
    connectMQTT();
  }
  mqttClient.loop();
  
  if (serverFound) {
    if (millis() - lastCommandTime >= commandInterval) {
      if (!sendCommand()) {
        logMessage("Lost connection to server. Restarting scan...");
        xSemaphoreTake(serverMutex, portMAX_DELAY);
        serverFound = false;
        serverIP = "";
        serverPort = 0;
        xSemaphoreGive(serverMutex);
        client.stop();
        
        currentScanIP = 1;
        scanComplete = false;
        startNetworkScan();
      }
      lastCommandTime = millis();
    }
  } else if (scanComplete) {
    logMessage("Scan complete. No server found. Restarting in 5 seconds...");
    delay(5000);
    currentScanIP = 1;
    scanComplete = false;
    startNetworkScan();
  }
  
  delay(100);
}
