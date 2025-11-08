// mqtt_functions.ino

// mqtt_functions.ino

#include <stdint.h>
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#ifndef tskNO_AFFINITY
#define tskNO_AFFINITY -1
#endif

void handleWifiPasswordCommand(String command);
void handleFactoryResetCommand();
void handleIpConfigCommand(String command);
void initializeCommandScheduler();
void handleMqttRequest(const String &payload);
uint32_t calculateCommandCrc32(const String &command);
String buildResponseTopic(const String &command);
bool parseRequestPayload(const String &payload, String &command, unsigned long &intervalMs, bool &sendAlways);
bool sendCommandAndMaybePublish(int slotIndex, const String &command, bool sendAlways);
bool sendCommandToTcpServer(const String &command, String &responseOut);
int reserveSlotForCommand(const String &command);
int getActiveWorkerCount();
void startCommandWorker(int slotIndex);
void commandTask(void *param);
bool parseUnsignedLong(const String &value, unsigned long &result);

const unsigned long COMMAND_RESPONSE_TIMEOUT_MS = 2000;
const int MAX_COMMAND_SLOTS = 8;
int maxCommandWorkers = MAX_COMMAND_SLOTS;

struct CommandSlot {
  bool inUse = false;
  bool active = false;
  bool sendAlways = false;
  bool triggerImmediate = false;
  bool terminateAfterNext = false;
  unsigned long intervalMs = 0;
  unsigned long nextRun = 0;
  String command = "";
  String lastResponse = "";
  bool hasLastResponse = false;
  TaskHandle_t taskHandle = nullptr;
};

CommandSlot commandSlots[MAX_COMMAND_SLOTS];
SemaphoreHandle_t commandSlotsMutex = nullptr;

const uint16_t COMMAND_TASK_STACK_SIZE = 4096;
const UBaseType_t COMMAND_TASK_PRIORITY = 1;

extern bool setWiredConfiguration(bool useDhcp, const String &ip, const String &mask, const String &gateway, const String &dns, String &errorMessage);
extern bool wiredDhcpEnabled;
extern String wiredStaticIPStr;
extern String wiredStaticMaskStr;
extern String wiredStaticGatewayStr;
extern String wiredStaticDnsStr;
extern IPAddress wiredStaticIP;
extern IPAddress wiredStaticMask;
extern IPAddress wiredStaticGateway;
extern IPAddress wiredStaticDns;

void connectMQTT() {
  int attempts = 0;
  while (!mqttClient.connected() && attempts < 5) {
    Serial.print("Connecting to MQTT broker...");
    yield(); // Feed watchdog
    
    String clientId = "ESP32-" + macAddress;
    
    if (strlen(mqtt_user) > 0) {
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
        Serial.println(" connected!");
        
        // Subscribe to request and command topics
        String requestTopic = mqttTopicBase + "request";
        String commandTopic = mqttTopicBase + "command";
        mqttClient.subscribe(requestTopic.c_str());
        mqttClient.subscribe(commandTopic.c_str());

        wiredIP = ETH.localIP().toString();
        addLog("MQTT connected - Device IP: " + ETH.localIP().toString());
        addLog("Subscribed to: " + requestTopic);
        addLog("Subscribed to: " + commandTopic);
        return;
      }
    } else {
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(" connected!");
        
        // Subscribe to request and command topics
        String requestTopic = mqttTopicBase + "request";
        String commandTopic = mqttTopicBase + "command";
        mqttClient.subscribe(requestTopic.c_str());
        mqttClient.subscribe(commandTopic.c_str());

        wiredIP = ETH.localIP().toString();
        addLog("MQTT connected - Device IP: " + ETH.localIP().toString());
        addLog("Subscribed to: " + requestTopic);
        addLog("Subscribed to: " + commandTopic);
        return;
      }
    }
    
    Serial.print(" failed, rc=");
    Serial.println(mqttClient.state());
    attempts++;
    
    // Feed watchdog during delay
    for (int i = 0; i < 20; i++) {
      delay(100);
      yield();
    }
  }
  
  if (!mqttClient.connected()) {
    Serial.println("MQTT connection failed after 5 attempts. Will retry later.");
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Convert payload to string
  String message = "";
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }

  String topicStr = String(topic);

  // Check if it's a command topic
  if (topicStr.endsWith("/command")) {
    handleCommand(message);
    return;
  }

  // Otherwise it's a request topic - process according to scheduler rules
  handleMqttRequest(message);
}

void initializeCommandScheduler() {
  if (commandSlotsMutex == nullptr) {
    commandSlotsMutex = xSemaphoreCreateMutex();
  }

  if (commandSlotsMutex == nullptr) {
    addLog("Failed to initialize command scheduler");
    return;
  }

  xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    commandSlots[i].inUse = false;
    commandSlots[i].active = false;
    commandSlots[i].sendAlways = false;
    commandSlots[i].triggerImmediate = false;
    commandSlots[i].terminateAfterNext = false;
    commandSlots[i].intervalMs = 0;
    commandSlots[i].nextRun = 0;
    commandSlots[i].command = "";
    commandSlots[i].lastResponse = "";
    commandSlots[i].hasLastResponse = false;
    commandSlots[i].taskHandle = nullptr;
  }
  xSemaphoreGive(commandSlotsMutex);
}

bool parseUnsignedLong(const String &value, unsigned long &result) {
  if (value.length() == 0) {
    return false;
  }

  unsigned long parsed = 0;
  for (size_t i = 0; i < value.length(); ++i) {
    char c = value.charAt(i);
    if (c < '0' || c > '9') {
      return false;
    }
    parsed = parsed * 10 + (c - '0');
  }

  result = parsed;
  return true;
}

bool parseRequestPayload(const String &payload, String &command, unsigned long &intervalMs, bool &sendAlways) {
  String trimmed = payload;
  trimmed.trim();

  if (trimmed.length() == 0) {
    return false;
  }

  intervalMs = 0;
  sendAlways = false;
  command = trimmed;

  int metadataSeparator = trimmed.indexOf('|');
  if (metadataSeparator == -1) {
    return command.length() > 0;
  }

  String commandPart = trimmed.substring(0, metadataSeparator);
  String metadataPart = trimmed.substring(metadataSeparator + 1);
  commandPart.trim();
  metadataPart.trim();

  if (commandPart.length() == 0) {
    return false;
  }

  String intervalToken = metadataPart;
  String flagToken = "";

  int secondSeparator = metadataPart.indexOf('|');
  if (secondSeparator != -1) {
    intervalToken = metadataPart.substring(0, secondSeparator);
    flagToken = metadataPart.substring(secondSeparator + 1);
  }

  intervalToken.trim();
  flagToken.trim();

  bool metadataValid = true;

  if (intervalToken.length() > 0) {
    unsigned long parsedInterval = 0;
    if (parseUnsignedLong(intervalToken, parsedInterval)) {
      intervalMs = parsedInterval;
    } else {
      metadataValid = false;
    }
  }

  if (flagToken.length() > 0) {
    if (flagToken.equals("0")) {
      sendAlways = false;
    } else if (flagToken.equals("1")) {
      sendAlways = true;
    } else {
      metadataValid = false;
    }
  }

  if (!metadataValid) {
    command = trimmed;
    intervalMs = 0;
    sendAlways = false;
    return command.length() > 0;
  }

  command = commandPart;
  return command.length() > 0;
}

int reserveSlotForCommand(const String &command) {
  if (commandSlotsMutex == nullptr) {
    return -1;
  }

  xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);

  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    if (commandSlots[i].inUse && commandSlots[i].command.equals(command)) {
      int existingIndex = i;
      xSemaphoreGive(commandSlotsMutex);
      return existingIndex;
    }
  }

  int freeIndex = -1;
  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    if (!commandSlots[i].inUse) {
      freeIndex = i;
      break;
    }
  }

  if (freeIndex == -1) {
    for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
      if (!commandSlots[i].active) {
        freeIndex = i;
        break;
      }
    }
  }

  if (freeIndex != -1) {
    CommandSlot &slot = commandSlots[freeIndex];
    slot.inUse = true;
    slot.active = false;
    slot.sendAlways = false;
    slot.triggerImmediate = false;
    slot.terminateAfterNext = false;
    slot.intervalMs = 0;
    slot.nextRun = 0;
    slot.taskHandle = nullptr;
    if (!slot.command.equals(command)) {
      slot.lastResponse = "";
      slot.hasLastResponse = false;
    }
    slot.command = command;
  }

  xSemaphoreGive(commandSlotsMutex);
  return freeIndex;
}

int getActiveWorkerCount() {
  if (commandSlotsMutex == nullptr) {
    return 0;
  }

  int count = 0;
  xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    if (commandSlots[i].active) {
      count++;
    }
  }
  xSemaphoreGive(commandSlotsMutex);
  return count;
}

void startCommandWorker(int slotIndex) {
  if (slotIndex < 0 || slotIndex >= MAX_COMMAND_SLOTS) {
    return;
  }
  if (commandSlotsMutex == nullptr) {
    return;
  }

  unsigned long intervalMs = 0;
  String command;

  xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
  CommandSlot &slot = commandSlots[slotIndex];
  slot.active = true;
  slot.triggerImmediate = true;
  slot.terminateAfterNext = false;
  slot.nextRun = 0;
  intervalMs = slot.intervalMs;
  command = slot.command;
  xSemaphoreGive(commandSlotsMutex);

  BaseType_t result = xTaskCreatePinnedToCore(
    commandTask,
    "cmd_worker",
    COMMAND_TASK_STACK_SIZE,
    reinterpret_cast<void *>(static_cast<intptr_t>(slotIndex)),
    COMMAND_TASK_PRIORITY,
    &commandSlots[slotIndex].taskHandle,
    tskNO_AFFINITY
  );

  if (result != pdPASS) {
    addLog("Failed to start command worker for \"" + command + "\"");
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    commandSlots[slotIndex].active = false;
    commandSlots[slotIndex].taskHandle = nullptr;
    commandSlots[slotIndex].triggerImmediate = false;
    xSemaphoreGive(commandSlotsMutex);
  } else {
    addLog("Started command worker for \"" + command + "\" @ " + String(intervalMs) + "ms");
  }
}

void commandTask(void *param) {
  int slotIndex = static_cast<int>(reinterpret_cast<intptr_t>(param));

  while (true) {
    if (commandSlotsMutex == nullptr) {
      break;
    }

    String command;
    bool sendAlways = false;
    bool shouldSend = false;

    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    if (slotIndex < 0 || slotIndex >= MAX_COMMAND_SLOTS) {
      xSemaphoreGive(commandSlotsMutex);
      break;
    }

    CommandSlot &slot = commandSlots[slotIndex];
    if (!slot.active) {
      xSemaphoreGive(commandSlotsMutex);
      break;
    }

    command = slot.command;
    sendAlways = slot.sendAlways;

    if (slot.triggerImmediate) {
      shouldSend = true;
      slot.triggerImmediate = false;
    } else if (slot.intervalMs > 0) {
      unsigned long now = millis();
      if (slot.nextRun == 0 || now >= slot.nextRun) {
        shouldSend = true;
      }
    }

    xSemaphoreGive(commandSlotsMutex);

    if (shouldSend) {
      sendCommandAndMaybePublish(slotIndex, command, sendAlways);

      bool shouldExit = false;

      xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
      if (slotIndex >= 0 && slotIndex < MAX_COMMAND_SLOTS) {
        CommandSlot &slotRef = commandSlots[slotIndex];
        if (slotRef.intervalMs > 0 && slotRef.active) {
          slotRef.nextRun = millis() + slotRef.intervalMs;
        } else {
          slotRef.nextRun = 0;
        }

        if (slotRef.terminateAfterNext) {
          slotRef.active = false;
          slotRef.terminateAfterNext = false;
          shouldExit = true;
        }
      }
      xSemaphoreGive(commandSlotsMutex);

      if (shouldExit) {
        addLog("Stopped command worker for \"" + command + "\"");
        break;
      }
    } else {
      vTaskDelay(25 / portTICK_PERIOD_MS);
    }
  }

  if (commandSlotsMutex != nullptr && slotIndex >= 0 && slotIndex < MAX_COMMAND_SLOTS) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    commandSlots[slotIndex].taskHandle = nullptr;
    commandSlots[slotIndex].active = false;
    commandSlots[slotIndex].triggerImmediate = false;
    commandSlots[slotIndex].terminateAfterNext = false;
    xSemaphoreGive(commandSlotsMutex);
  }

  vTaskDelete(nullptr);
}

uint32_t calculateCommandCrc32(const String &command) {
  uint32_t crc = 0xFFFFFFFF;
  for (size_t i = 0; i < command.length(); ++i) {
    uint8_t byte = static_cast<uint8_t>(command.charAt(i));
    crc ^= byte;
    for (int bit = 0; bit < 8; ++bit) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }
  }
  return crc ^ 0xFFFFFFFF;
}

String buildResponseTopic(const String &command) {
  uint32_t crc = calculateCommandCrc32(command);
  char crcBuffer[9];
  snprintf(crcBuffer, sizeof(crcBuffer), "%08lX", static_cast<unsigned long>(crc));
  return mqttTopicBase + "response/" + String(crcBuffer);
}

bool sendCommandToTcpServer(const String &command, String &responseOut) {
  lastRequestSent = command;

  if (!serverFound) {
    addLog("Cannot send command - server not connected");
    lastResponseReceived = "Server not connected";
    return false;
  }

  xSemaphoreTake(serverMutex, portMAX_DELAY);

  bool connected = serverFound && client.connected();
  if (!connected) {
    xSemaphoreGive(serverMutex);
    addLog("Cannot send command - server not connected");
    lastResponseReceived = "Server not connected";
    return false;
  }

  Serial.print("Sending to server: ");
  Serial.println(command);

  client.println(command);
  client.flush();

  unsigned long startTime = millis();
  while (!client.available() && (millis() - startTime < COMMAND_RESPONSE_TIMEOUT_MS)) {
    delay(10);
    yield();
  }

  bool received = false;
  if (client.available()) {
    responseOut = client.readStringUntil('\n');
    Serial.print("Server response: ");
    Serial.println(responseOut);
    received = true;
  } else {
    responseOut = "No response from server";
    addLog("No response from server");
  }

  xSemaphoreGive(serverMutex);

  lastResponseReceived = responseOut;
  return received;
}

bool sendCommandAndMaybePublish(int slotIndex, const String &command, bool sendAlways) {
  String response;
  bool received = sendCommandToTcpServer(command, response);

  if (!received) {
    return false;
  }

  bool shouldPublish = sendAlways;
  String previousResponse = "";
  bool hasPreviousResponse = false;

  if (slotIndex >= 0 && slotIndex < MAX_COMMAND_SLOTS && commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    CommandSlot &slot = commandSlots[slotIndex];
    hasPreviousResponse = slot.hasLastResponse;
    previousResponse = slot.lastResponse;
    xSemaphoreGive(commandSlotsMutex);
  }

  if (!sendAlways) {
    if (!hasPreviousResponse || response != previousResponse) {
      shouldPublish = true;
    } else {
      shouldPublish = false;
    }
  }

  if (shouldPublish && mqttClient.connected()) {
    String topic = buildResponseTopic(command);
    mqttClient.publish(topic.c_str(), response.c_str());
  }

  if (slotIndex >= 0 && slotIndex < MAX_COMMAND_SLOTS && commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    CommandSlot &slot = commandSlots[slotIndex];
    slot.lastResponse = response;
    slot.hasLastResponse = true;
    xSemaphoreGive(commandSlotsMutex);
  }

  return true;
}

void handleMqttRequest(const String &payload) {
  addLog("MQTT Request received: " + payload);

  String command;
  unsigned long intervalMs = 0;
  bool sendAlways = false;

  if (!parseRequestPayload(payload, command, intervalMs, sendAlways)) {
    addLog("Invalid request payload format");
    return;
  }

  int slotIndex = reserveSlotForCommand(command);
  if (slotIndex == -1) {
    addLog("Unable to reserve slot for command: " + command + ". Executing once without scheduling.");
    sendCommandAndMaybePublish(-1, command, sendAlways);
    return;
  }

  if (intervalMs == 0) {
    bool hadActiveWorker = false;

    if (commandSlotsMutex != nullptr) {
      xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
      CommandSlot &slot = commandSlots[slotIndex];
      slot.sendAlways = sendAlways;
      hadActiveWorker = slot.active;
      if (slot.active) {
        slot.intervalMs = 0;
        slot.terminateAfterNext = true;
        slot.triggerImmediate = true;
      }
      xSemaphoreGive(commandSlotsMutex);
    }

    if (hadActiveWorker) {
      addLog("Stopping repeating command \"" + command + "\" after next response");
    } else {
      sendCommandAndMaybePublish(slotIndex, command, sendAlways);
    }
    return;
  }

  if (commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    CommandSlot &slot = commandSlots[slotIndex];
    slot.sendAlways = sendAlways;
    slot.intervalMs = intervalMs;
    slot.terminateAfterNext = false;
    slot.triggerImmediate = true;
    slot.nextRun = 0;
    xSemaphoreGive(commandSlotsMutex);
  }

  bool alreadyActive = false;
  if (commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    alreadyActive = commandSlots[slotIndex].active;
    xSemaphoreGive(commandSlotsMutex);
  }

  if (alreadyActive) {
    addLog("Updated command \"" + command + "\" interval to " + String(intervalMs) + "ms");
    return;
  }

  if (getActiveWorkerCount() >= maxCommandWorkers) {
    addLog("Maximum command workers reached. Executing \"" + command + "\" once only");
    sendCommandAndMaybePublish(slotIndex, command, sendAlways);
    return;
  }

  startCommandWorker(slotIndex);
}

void handleCommand(String command) {
  command.trim();
  addLog("Command received: " + command);
  lastCommandReceived = command;
  
  // Convert to lowercase for comparison
  String cmdLower = command;
  cmdLower.toLowerCase();
  
  if (cmdLower.equals("boot")) {
    addLog("Rebooting device...");
    delay(1000);
    ESP.restart();
  }
  else if (cmdLower.equals("factoryreset")) {
    handleFactoryResetCommand();
  }
  else if (cmdLower.equals("scan")) {
    addLog("Starting network rescan...");

    // Stop current server connection
    if (serverFound) {
      xSemaphoreTake(serverMutex, portMAX_DELAY);
      serverFound = false;
      serverIP = "";
      serverPort = 0;
      internetAddress = "";
      runningTotal = "None";
      xSemaphoreGive(serverMutex);
      client.stop();
    }

    // Restart scan
    currentScanIP = 1;
    scanComplete = false;
    startNetworkScan();
  }
  else if (cmdLower.startsWith("wifipassword")) {
    handleWifiPasswordCommand(command);
  }
  else if (cmdLower.startsWith("port ")) {
    handlePortCommand(command);
  }
  else if (cmdLower.startsWith("ipconfig")) {
    handleIpConfigCommand(command);
  }
  else {
    addLog("Unknown command: " + command);
  }
}

void handlePortCommand(String command) {
  // Remove "port " prefix (case insensitive)
  String cmdLower = command;
  cmdLower.toLowerCase();
  int portIndex = cmdLower.indexOf("port ");
  if (portIndex != -1) {
    command = command.substring(portIndex + 5);
  }
  command.trim();
  
  // Parse port numbers
  int newPorts[10];
  int newNumPorts = 0;
  
  int startIndex = 0;
  while (startIndex < command.length() && newNumPorts < 10) {
    int spaceIndex = command.indexOf(' ', startIndex);
    String portStr;
    
    if (spaceIndex == -1) {
      portStr = command.substring(startIndex);
    } else {
      portStr = command.substring(startIndex, spaceIndex);
    }
    
    portStr.trim();
    if (portStr.length() > 0) {
      int port = portStr.toInt();
      if (port > 0 && port <= 65535) {
        newPorts[newNumPorts++] = port;
      } else {
        addLog("Invalid port number: " + portStr);
        return;
      }
    }
    
    if (spaceIndex == -1) break;
    startIndex = spaceIndex + 1;
  }
  
  if (newNumPorts == 0) {
    addLog("Error: No valid ports specified");
    return;
  }
  
  // Save to preferences
  preferences.putInt("numPorts", newNumPorts);
  for (int i = 0; i < newNumPorts; i++) {
    String key = "port" + String(i);
    preferences.putInt(key.c_str(), newPorts[i]);
  }
  
  // Update current ports
  numPorts = newNumPorts;
  for (int i = 0; i < numPorts; i++) {
    ports[i] = newPorts[i];
  }
  
  // Build confirmation message
  String portList = "Ports updated: ";
  for (int i = 0; i < numPorts; i++) {
    portList += String(ports[i]);
    if (i < numPorts - 1) portList += ", ";
  }
  addLog(portList);
  
  // Trigger rescan with new ports
  if (serverFound) {
    xSemaphoreTake(serverMutex, portMAX_DELAY);
    serverFound = false;
    serverIP = "";
    serverPort = 0;
    internetAddress = "";
    xSemaphoreGive(serverMutex);
    client.stop();
  }
  
  currentScanIP = 1;
  scanComplete = false;
  startNetworkScan();
}

void handleWifiPasswordCommand(String command) {
  int spaceIndex = command.indexOf(' ');
  if (spaceIndex == -1) {
    addLog("Error: WiFi password command requires a password");
    return;
  }

  String newPassword = command.substring(spaceIndex + 1);
  newPassword.trim();

  if (newPassword.length() < 8 || newPassword.length() > 63) {
    addLog("Error: WiFi password must be between 8 and 63 characters");
    return;
  }

  if (newPassword == ap_password) {
    addLog("WiFi password unchanged");
    return;
  }

  preferences.putString("apPassword", newPassword);
  ap_password = newPassword;

  addLog("WiFi password updated. Disconnecting clients...");
  WiFi.softAPdisconnect(true);
  delay(200);

  if (WiFi.softAP(ap_ssid.c_str(), ap_password.c_str())) {
    addLog("Access point restarted with new password");
  } else {
    addLog("Failed to restart access point with new password");
  }
}

void handleFactoryResetCommand() {
  addLog("Factory reset requested");

  WiFi.softAPdisconnect(true);
  preferences.clear();
  ap_password = DEFAULT_AP_PASSWORD;
  wiredDhcpEnabled = true;
  wiredStaticIPStr = "";
  wiredStaticMaskStr = "";
  wiredStaticGatewayStr = "";
  wiredStaticDnsStr = "";
  wiredStaticIP = IPAddress(0, 0, 0, 0);
  wiredStaticMask = IPAddress(255, 255, 255, 0);
  wiredStaticGateway = IPAddress(0, 0, 0, 0);
  wiredStaticDns = IPAddress(0, 0, 0, 0);

  addLog("Preferences cleared. Rebooting...");
  delay(1000);
  ESP.restart();
}

void handleIpConfigCommand(String command) {
  String working = command;
  working.trim();

  int spaceIndex = working.indexOf(' ');
  if (spaceIndex == -1) {
    addLog("Usage: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
    return;
  }

  working = working.substring(spaceIndex + 1);
  working.trim();

  if (working.length() == 0) {
    addLog("Usage: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
    return;
  }

  String tokens[5];
  int tokenCount = 0;
  int startIndex = 0;

  while (tokenCount < 5 && startIndex < working.length()) {
    int nextSpace = working.indexOf(' ', startIndex);
    String token;
    if (nextSpace == -1) {
      token = working.substring(startIndex);
      startIndex = working.length();
    } else {
      token = working.substring(startIndex, nextSpace);
      startIndex = nextSpace + 1;
    }

    token.trim();
    if (token.length() > 0) {
      tokens[tokenCount++] = token;
    }
  }

  if (tokenCount == 0) {
    addLog("Usage: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
    return;
  }

  String mode = tokens[0];
  mode.toLowerCase();

  String errorMessage;

  if (mode == "dhcp") {
    if (setWiredConfiguration(true, "", "", "", "", errorMessage)) {
      addLog("Ethernet configured for DHCP via MQTT");
    } else {
      addLog("Failed to apply DHCP configuration: " + errorMessage);
    }
    return;
  }

  if (mode != "fixed") {
    addLog("Unknown IP configuration mode: " + tokens[0]);
    return;
  }

  if (tokenCount < 5) {
    addLog("Usage: ipconfig fixed <ip> <mask> <gateway> <dns>");
    return;
  }

  String ip = tokens[1];
  String mask = tokens[2];
  String gateway = tokens[3];
  String dns = tokens[4];

  if (setWiredConfiguration(false, ip, mask, gateway, dns, errorMessage)) {
    addLog("Ethernet static IP set to " + ip + " via MQTT");
  } else {
    addLog("Failed to apply static IP configuration: " + errorMessage);
  }
}
