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
void persistCommandSlots();
void loadPersistedCommandSlots();
void handleMqttRequest(const String &payload);
uint32_t calculateCommandCrc32(const String &command);
String buildResponseTopic(const String &command);
bool parseRequestPayload(const String &payload, String &command, unsigned long &intervalMs, bool &sendAlways, bool &hasMetadata);
bool sendCommandAndMaybePublish(int slotIndex, const String &command, bool sendAlways);
bool sendCommandToTcpServer(const String &command, String &responseOut);
int reserveSlotForCommand(const String &command);
int getActiveWorkerCount();
void startCommandWorker(int slotIndex);
void commandTask(void *param);
bool parseUnsignedLong(const String &value, unsigned long &result);
void performOtaUpdate(const String &binUrl, const String &md5Url);
bool ensureServerConnection();

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
    Serial.print("Conectando ao broker MQTT...");
    yield(); // Feed watchdog
    
    String clientId = "ESP32-" + macAddress;
    
    if (strlen(mqtt_user) > 0) {
      if (mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
        Serial.println(" conectado!");
        
        // Subscribe to request and command topics
        String requestTopic = mqttTopicBase + "request";
        String commandTopic = mqttTopicBase + "command";
        mqttClient.subscribe(requestTopic.c_str());
        mqttClient.subscribe(commandTopic.c_str());

        wiredIP = ETH.localIP().toString();
        addLog("MQTT conectado - IP do dispositivo: " + ETH.localIP().toString());
        addLog("Inscrito em: " + requestTopic);
        addLog("Inscrito em: " + commandTopic);
        return;
      }
    } else {
      if (mqttClient.connect(clientId.c_str())) {
        Serial.println(" conectado!");
        
        // Subscribe to request and command topics
        String requestTopic = mqttTopicBase + "request";
        String commandTopic = mqttTopicBase + "command";
        mqttClient.subscribe(requestTopic.c_str());
        mqttClient.subscribe(commandTopic.c_str());

        wiredIP = ETH.localIP().toString();
        addLog("MQTT conectado - IP do dispositivo: " + ETH.localIP().toString());
        addLog("Inscrito em: " + requestTopic);
        addLog("Inscrito em: " + commandTopic);
        return;
      }
    }
    
    Serial.print(" falhou, rc=");
    Serial.println(mqttClient.state());
    attempts++;
    
    // Feed watchdog during delay
    for (int i = 0; i < 20; i++) {
      delay(100);
      yield();
    }
  }
  
  if (!mqttClient.connected()) {
    Serial.println("Conexão MQTT falhou após 5 tentativas. Tentará novamente depois.");
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
    addLog("Falha ao inicializar o agendador de comandos");
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

void persistCommandSlots() {
  if (commandSlotsMutex == nullptr) {
    return;
  }

  struct SlotSnapshot {
    bool inUse;
    bool active;
    bool sendAlways;
    unsigned long intervalMs;
    String command;
  };

  SlotSnapshot snapshots[MAX_COMMAND_SLOTS];

  xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    snapshots[i].inUse = commandSlots[i].inUse;
    snapshots[i].active = commandSlots[i].active;
    snapshots[i].sendAlways = commandSlots[i].sendAlways;
    snapshots[i].intervalMs = commandSlots[i].intervalMs;
    snapshots[i].command = commandSlots[i].command;
  }
  xSemaphoreGive(commandSlotsMutex);

  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    String baseKey = "reqSlot" + String(i);
    String keyInUse = baseKey + "InUse";
    String keyCmd = baseKey + "Cmd";
    String keyInterval = baseKey + "Interval";
    String keyAlways = baseKey + "Always";
    String keyActive = baseKey + "Active";

    preferences.putBool(keyInUse.c_str(), snapshots[i].inUse);

    if (!snapshots[i].inUse) {
      preferences.remove(keyCmd.c_str());
      preferences.remove(keyInterval.c_str());
      preferences.remove(keyAlways.c_str());
      preferences.remove(keyActive.c_str());
      continue;
    }

    preferences.putString(keyCmd.c_str(), snapshots[i].command);
    preferences.putULong(keyInterval.c_str(), snapshots[i].intervalMs);
    preferences.putBool(keyAlways.c_str(), snapshots[i].sendAlways);
    preferences.putBool(keyActive.c_str(), snapshots[i].active);
  }
}

void loadPersistedCommandSlots() {
  if (commandSlotsMutex == nullptr) {
    return;
  }

  bool anyRestored = false;

  for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
    String baseKey = "reqSlot" + String(i);
    String keyInUse = baseKey + "InUse";

    bool inUse = preferences.getBool(keyInUse.c_str(), false);
    if (!inUse) {
      continue;
    }

    String command = preferences.getString((baseKey + "Cmd").c_str(), "");
    if (command.length() == 0) {
      continue;
    }

    unsigned long interval = preferences.getULong((baseKey + "Interval").c_str(), 0);
    bool sendAlways = preferences.getBool((baseKey + "Always").c_str(), true);
    bool wasActive = preferences.getBool((baseKey + "Active").c_str(), interval > 0);

    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    CommandSlot &slot = commandSlots[i];
    slot.inUse = true;
    slot.active = false;
    slot.sendAlways = sendAlways;
    slot.triggerImmediate = false;
    slot.terminateAfterNext = false;
    slot.intervalMs = interval;
    slot.nextRun = 0;
    slot.command = command;
    slot.lastResponse = "";
    slot.hasLastResponse = false;
    slot.taskHandle = nullptr;
    xSemaphoreGive(commandSlotsMutex);

    anyRestored = true;

    if (interval > 0 && wasActive) {
      addLog("Restaurando worker persistido para \"" + command + "\" a cada " + String(interval) + "ms");
      startCommandWorker(i);
    } else {
      addLog("Comando persistido carregado: \"" + command + "\"");
    }
  }

  if (anyRestored) {
    persistCommandSlots();
  }
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

bool parseRequestPayload(const String &payload, String &command, unsigned long &intervalMs, bool &sendAlways, bool &hasMetadata) {
  String trimmed = payload;
  trimmed.trim();

  if (trimmed.length() == 0) {
    return false;
  }

  intervalMs = 0;
  sendAlways = false;
  hasMetadata = false;
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
      hasMetadata = true;
    } else {
      metadataValid = false;
    }
  }

  if (flagToken.length() > 0) {
    if (flagToken.equals("0")) {
      sendAlways = false;
      hasMetadata = true;
    } else if (flagToken.equals("1")) {
      sendAlways = true;
      hasMetadata = true;
    } else {
      metadataValid = false;
    }
  }

  if (!metadataValid) {
    command = trimmed;
    intervalMs = 0;
    sendAlways = false;
    hasMetadata = false;
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
    addLog("Falha ao iniciar o worker de comando para \"" + command + "\"");
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    commandSlots[slotIndex].active = false;
    commandSlots[slotIndex].taskHandle = nullptr;
    commandSlots[slotIndex].triggerImmediate = false;
    xSemaphoreGive(commandSlotsMutex);
  } else {
    addLog("Worker de comando iniciado para \"" + command + "\" @ " + String(intervalMs) + "ms");
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
        addLog("Worker de comando encerrado para \"" + command + "\"");
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
    persistCommandSlots();
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
    addLog("Não é possível enviar comando - servidor não conectado");
    lastResponseReceived = "Servidor não conectado";
    return false;
  }

  xSemaphoreTake(serverMutex, portMAX_DELAY);

  bool connected = serverFound && client.connected();
  if (!connected && serverFound) {
    connected = ensureServerConnection();
  }

  if (!serverFound || !connected) {
    xSemaphoreGive(serverMutex);
    addLog("Não é possível enviar comando - servidor não conectado");
    lastResponseReceived = "Servidor não conectado";
    return false;
  }

  Serial.print("Enviando para o servidor: ");
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
    Serial.print("Resposta do servidor: ");
    Serial.println(responseOut);
    received = true;
  } else {
    responseOut = "Nenhuma resposta do servidor";
    addLog("Nenhuma resposta do servidor");
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
  addLog("Solicitação MQTT recebida: " + payload);

  String command;
  unsigned long intervalMs = 0;
  bool sendAlways = false;
  bool hasMetadata = false;
  bool persistNeeded = false;

  if (!parseRequestPayload(payload, command, intervalMs, sendAlways, hasMetadata)) {
    addLog("Formato inválido de carga de solicitação");
    return;
  }

  bool effectiveSendAlways = hasMetadata ? sendAlways : true;

  int slotIndex = reserveSlotForCommand(command);
  if (slotIndex == -1) {
    addLog("Não foi possível reservar slot para o comando: " + command + ". Executando uma vez sem agendamento.");
    sendCommandAndMaybePublish(-1, command, effectiveSendAlways);
    return;
  }

  if (intervalMs == 0) {
    bool hadActiveWorker = false;

    if (commandSlotsMutex != nullptr) {
      xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
      CommandSlot &slot = commandSlots[slotIndex];
      slot.sendAlways = effectiveSendAlways;
      persistNeeded = true;
      hadActiveWorker = slot.active;
      if (slot.active) {
        slot.intervalMs = 0;
        slot.terminateAfterNext = true;
        slot.triggerImmediate = true;
      }
      xSemaphoreGive(commandSlotsMutex);
    }

    if (hadActiveWorker) {
      addLog("Parando comando repetido \"" + command + "\" após a próxima resposta");
      if (persistNeeded) {
        persistCommandSlots();
      }
    } else {
      sendCommandAndMaybePublish(slotIndex, command, effectiveSendAlways);
      if (persistNeeded) {
        persistCommandSlots();
      }
    }
    return;
  }

  if (commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    CommandSlot &slot = commandSlots[slotIndex];
    slot.sendAlways = effectiveSendAlways;
    slot.intervalMs = intervalMs;
    slot.terminateAfterNext = false;
    slot.triggerImmediate = true;
    slot.nextRun = 0;
    xSemaphoreGive(commandSlotsMutex);
    persistNeeded = true;
  }

  bool alreadyActive = false;
  if (commandSlotsMutex != nullptr) {
    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    alreadyActive = commandSlots[slotIndex].active;
    xSemaphoreGive(commandSlotsMutex);
  }

  if (alreadyActive) {
    addLog("Intervalo do comando \"" + command + "\" atualizado para " + String(intervalMs) + "ms");
    if (persistNeeded) {
      persistCommandSlots();
    }
    return;
  }

  if (getActiveWorkerCount() >= maxCommandWorkers) {
    addLog("Número máximo de workers de comando atingido. Executando \"" + command + "\" apenas uma vez");
    sendCommandAndMaybePublish(slotIndex, command, effectiveSendAlways);
    if (persistNeeded) {
      persistCommandSlots();
    }
    return;
  }

  startCommandWorker(slotIndex);
  persistCommandSlots();
}

void handleCommand(String command) {
  command.trim();
  addLog("Comando recebido: " + command);
  lastCommandReceived = command;
  
  // Convert to lowercase for comparison
  String cmdLower = command;
  cmdLower.toLowerCase();
  
  if (cmdLower.equals("boot")) {
    addLog("Reiniciando dispositivo...");
    delay(1000);
    ESP.restart();
  }
  else if (cmdLower.equals("factoryreset")) {
    handleFactoryResetCommand();
  }
  else if (cmdLower.equals("scan")) {
    addLog("Iniciando nova varredura de rede...");

    // Stop current server connection
    if (serverFound) {
      xSemaphoreTake(serverMutex, portMAX_DELAY);
      serverFound = false;
      serverIP = "";
      serverPort = 0;
      internetAddress = "";
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
  else if (cmdLower.equals("ota")) {
    addLog("Uso: ota <url firmware.bin> <url firmware.md5>");
  }
  else if (cmdLower.startsWith("ota ")) {
    String args = command.substring(command.indexOf(' ') + 1);
    args.trim();

    if (args.length() == 0) {
      addLog("Uso: ota <url firmware.bin> <url firmware.md5>");
      return;
    }

    int secondSpace = args.indexOf(' ');
    if (secondSpace == -1) {
      addLog("Uso: ota <url firmware.bin> <url firmware.md5>");
      return;
    }

    String binUrl = args.substring(0, secondSpace);
    String md5Url = args.substring(secondSpace + 1);
    binUrl.trim();
    md5Url.trim();

    if (binUrl.length() == 0 || md5Url.length() == 0) {
      addLog("Uso: ota <url firmware.bin> <url firmware.md5>");
      return;
    }

    performOtaUpdate(binUrl, md5Url);
  }
  else if (cmdLower.equals("workers")) {
    if (commandSlotsMutex == nullptr) {
      addLog("Agendador de comandos não inicializado");
      return;
    }

    bool slotInUse[MAX_COMMAND_SLOTS];
    bool slotActive[MAX_COMMAND_SLOTS];
    bool slotSendAlways[MAX_COMMAND_SLOTS];
    unsigned long slotInterval[MAX_COMMAND_SLOTS];
    String slotCommand[MAX_COMMAND_SLOTS];

    xSemaphoreTake(commandSlotsMutex, portMAX_DELAY);
    for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
      const CommandSlot &slot = commandSlots[i];
      slotInUse[i] = slot.inUse;
      slotActive[i] = slot.active;
      slotSendAlways[i] = slot.sendAlways;
      slotInterval[i] = slot.intervalMs;
      slotCommand[i] = slot.command;
    }
    xSemaphoreGive(commandSlotsMutex);

    bool anyWorkers = false;
    addLog("Workers configurados:");

    for (int i = 0; i < MAX_COMMAND_SLOTS; ++i) {
      if (!slotInUse[i] || slotCommand[i].length() == 0) {
        continue;
      }

      anyWorkers = true;
      uint32_t crc = calculateCommandCrc32(slotCommand[i]);
      char crcBuffer[9];
      snprintf(crcBuffer, sizeof(crcBuffer), "%08lX", static_cast<unsigned long>(crc));

      String intervaloStr = slotInterval[i] > 0 ? String(slotInterval[i]) + "ms" : "sem intervalo";
      String publishStr = slotSendAlways[i] ? "sempre" : "quando mudar";
      String ativoStr = slotActive[i] ? "sim" : "não";

      String message = "- " + String(crcBuffer) + ": comando=\"" + slotCommand[i] + "\"";
      message += ", intervalo=" + intervaloStr;
      message += ", envia=" + publishStr;
      message += ", ativo=" + ativoStr;
      addLog(message);
    }

    if (!anyWorkers) {
      addLog("Nenhum worker configurado");
    }
  }
  else {
    addLog("Comando desconhecido: " + command);
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
        addLog("Número de porta inválido: " + portStr);
        return;
      }
    }
    
    if (spaceIndex == -1) break;
    startIndex = spaceIndex + 1;
  }
  
  if (newNumPorts == 0) {
    addLog("Erro: nenhuma porta válida especificada");
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
  String portList = "Portas atualizadas: ";
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
    addLog("Erro: comando de senha WiFi requer uma senha");
    return;
  }

  String newPassword = command.substring(spaceIndex + 1);
  newPassword.trim();

  if (newPassword.length() < 8 || newPassword.length() > 63) {
    addLog("Erro: a senha WiFi deve ter entre 8 e 63 caracteres");
    return;
  }

  if (newPassword == ap_password) {
    addLog("Senha WiFi inalterada");
    return;
  }

  preferences.putString("apPassword", newPassword);
  ap_password = newPassword;

  addLog("Senha WiFi atualizada. Desconectando clientes...");
  WiFi.softAPdisconnect(true);
  delay(200);

  if (WiFi.softAP(ap_ssid.c_str(), ap_password.c_str())) {
    addLog("Ponto de acesso reiniciado com a nova senha");
  } else {
    addLog("Falha ao reiniciar o ponto de acesso com a nova senha");
  }
}

void handleFactoryResetCommand() {
  addLog("Solicitada restauração de fábrica");

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

  addLog("Preferências limpas. Reiniciando...");
  delay(1000);
  ESP.restart();
}

void handleIpConfigCommand(String command) {
  String working = command;
  working.trim();

  int spaceIndex = working.indexOf(' ');
  if (spaceIndex == -1) {
    addLog("Uso: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
    return;
  }

  working = working.substring(spaceIndex + 1);
  working.trim();

  if (working.length() == 0) {
    addLog("Uso: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
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
    addLog("Uso: ipconfig <dhcp|fixed> <ip> <mask> <gateway> <dns>");
    return;
  }

  String mode = tokens[0];
  mode.toLowerCase();

  String errorMessage;

  if (mode == "dhcp") {
    if (setWiredConfiguration(true, "", "", "", "", errorMessage)) {
      addLog("Ethernet configurada para DHCP via MQTT");
    } else {
      addLog("Falha ao aplicar configuração DHCP: " + errorMessage);
    }
    return;
  }

  if (mode != "fixed") {
    addLog("Modo de configuração de IP desconhecido: " + tokens[0]);
    return;
  }

  if (tokenCount < 5) {
    addLog("Uso: ipconfig fixed <ip> <mask> <gateway> <dns>");
    return;
  }

  String ip = tokens[1];
  String mask = tokens[2];
  String gateway = tokens[3];
  String dns = tokens[4];

  if (setWiredConfiguration(false, ip, mask, gateway, dns, errorMessage)) {
    addLog("IP estático da Ethernet definido como " + ip + " via MQTT");
  } else {
    addLog("Falha ao aplicar configuração de IP estático: " + errorMessage);
  }
}
