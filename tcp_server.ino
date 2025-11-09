// tcp_server.ino

#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

extern volatile bool eth_connected;
extern bool serverFound;
extern volatile int currentScanIP;
extern volatile bool scanComplete;
extern unsigned long lastCommandTime;
extern String serverIP;
extern int serverPort;
extern Preferences preferences;
extern WiFiClient client;
extern SemaphoreHandle_t serverMutex;
extern SemaphoreHandle_t scanMutex;
extern int ports[10];
extern int numPorts;
extern const int defaultPorts[];
extern const int defaultNumPorts;
extern const char* const PREF_KEY_SERVER_IP;
extern const char* const PREF_KEY_SERVER_PORT;

void addLog(String message);

namespace {
const int MAX_RECONNECT_ATTEMPTS = 3;
const unsigned long RETRY_DELAY_MS = 500;
const int NUM_SCAN_TASKS = 8;
bool scanAbortRequested = false;
bool networkScanSuspended = false;
TaskHandle_t scanTaskHandles[NUM_SCAN_TASKS] = {nullptr};

void scanTask(void *parameter);
bool tryReconnectToSavedServer();
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

void startNetworkScan() {
  if (networkScanSuspended) {
    addLog("Varredura de rede suspensa. Ignorando solicitação de início.");
    return;
  }

  if (!eth_connected) {
    addLog("Não é possível iniciar a varredura - Ethernet não conectada");
    return;
  }

  IPAddress localIP = ETH.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    addLog("Não é possível iniciar a varredura - IP da Ethernet não atribuído");
    return;
  }

  if (tryReconnectToSavedServer()) {
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
  scanAbortRequested = false;

  for (int i = 0; i < NUM_SCAN_TASKS; ++i) {
    scanTaskHandles[i] = nullptr;
  }

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
      reinterpret_cast<void *>(static_cast<intptr_t>(i)),
      1,
      &scanTaskHandles[i]
    );
  }
}

namespace {

void scanTask(void *parameter) {
  int taskIndex = static_cast<int>(reinterpret_cast<intptr_t>(parameter));
  if (taskIndex < 0 || taskIndex >= NUM_SCAN_TASKS) {
    taskIndex = NUM_SCAN_TASKS - 1;
  }
  IPAddress localIP = ETH.localIP();
  IPAddress subnet = ETH.subnetMask();
  IPAddress network;

  for (int i = 0; i < 4; i++) {
    network[i] = localIP[i] & subnet[i];
  }

  while (true) {
    if (scanAbortRequested) {
      if (taskIndex >= 0 && taskIndex < NUM_SCAN_TASKS) {
        scanTaskHandles[taskIndex] = nullptr;
      }
      vTaskDelete(NULL);
      return;
    }

    if (!eth_connected) {
      if (taskIndex >= 0 && taskIndex < NUM_SCAN_TASKS) {
        scanTaskHandles[taskIndex] = nullptr;
      }
      vTaskDelete(NULL);
      return;
    }

    xSemaphoreTake(serverMutex, portMAX_DELAY);
    bool found = serverFound;
    xSemaphoreGive(serverMutex);

    if (found) {
      if (taskIndex >= 0 && taskIndex < NUM_SCAN_TASKS) {
        scanTaskHandles[taskIndex] = nullptr;
      }
      vTaskDelete(NULL);
      return;
    }

    xSemaphoreTake(scanMutex, portMAX_DELAY);
    int ipToScan = currentScanIP++;
    if (ipToScan >= 255) {
      scanComplete = true;
      xSemaphoreGive(scanMutex);
      if (taskIndex >= 0 && taskIndex < NUM_SCAN_TASKS) {
        scanTaskHandles[taskIndex] = nullptr;
      }
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
          if (taskIndex >= 0 && taskIndex < NUM_SCAN_TASKS) {
            scanTaskHandles[taskIndex] = nullptr;
          }
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

bool tryReconnectToSavedServer() {
  if (serverMutex == nullptr) {
    return false;
  }

  xSemaphoreTake(serverMutex, portMAX_DELAY);
  bool alreadyFound = serverFound;
  String savedIP = serverIP;
  int savedPort = serverPort;
  xSemaphoreGive(serverMutex);

  if (alreadyFound) {
    return true;
  }

  if (savedIP.length() == 0 || savedPort <= 0 || savedPort > 65535) {
    return false;
  }

  addLog("Tentando reconectar ao servidor salvo: " + savedIP + ":" + String(savedPort));

  xSemaphoreTake(serverMutex, portMAX_DELAY);

  client.stop();
  bool connected = client.connect(savedIP.c_str(), savedPort, 500);
  if (!connected) {
    xSemaphoreGive(serverMutex);
    addLog("Falha ao conectar ao servidor salvo");
    return false;
  }

  client.println("(&V)");
  client.flush();

  unsigned long startTime = millis();
  while (!client.available() && (millis() - startTime < 1000)) {
    delay(10);
    yield();
  }

  if (!client.available()) {
    client.stop();
    xSemaphoreGive(serverMutex);
    addLog("Servidor salvo não respondeu. Iniciando varredura completa.");
    return false;
  }

  String response = client.readStringUntil('\n');
  response.trim();
  if (response.length() > 0) {
    addLog("Resposta do servidor salvo: " + response);
  }

  serverFound = true;
  lastCommandTime = millis();
  xSemaphoreGive(serverMutex);

  addLog("Conexão restabelecida com o servidor salvo. Varredura não necessária.");
  return true;
}

}  // namespace

bool pauseNetworkScan() {
  networkScanSuspended = true;
  scanAbortRequested = true;

  bool hadActiveTask = false;
  unsigned long startWait = millis();
  const unsigned long WAIT_TIMEOUT_MS = 5000;

  while (true) {
    bool anyActive = false;
    for (int i = 0; i < NUM_SCAN_TASKS; ++i) {
      if (scanTaskHandles[i] != nullptr) {
        anyActive = true;
        break;
      }
    }

    if (!anyActive) {
      break;
    }

    hadActiveTask = true;
    if (millis() - startWait > WAIT_TIMEOUT_MS) {
      addLog("Aviso: timeout aguardando término das threads de varredura");
      break;
    }

    delay(25);
    yield();
  }

  bool tasksStillActive = false;
  for (int i = 0; i < NUM_SCAN_TASKS; ++i) {
    if (scanTaskHandles[i] != nullptr) {
      tasksStillActive = true;
      break;
    }
  }

  if (!tasksStillActive) {
    scanAbortRequested = false;
  }

  if (scanMutex != nullptr) {
    xSemaphoreTake(scanMutex, portMAX_DELAY);
    scanComplete = false;
    xSemaphoreGive(scanMutex);
  } else {
    scanComplete = false;
  }

  if (hadActiveTask) {
    addLog("Varredura de rede pausada");
  } else {
    addLog("Nenhuma thread de varredura ativa para pausar");
  }

  return hadActiveTask;
}

void resumeNetworkScan(bool restartImmediately) {
  networkScanSuspended = false;

  if (restartImmediately && eth_connected) {
    if (scanMutex != nullptr) {
      xSemaphoreTake(scanMutex, portMAX_DELAY);
      currentScanIP = 1;
      scanComplete = false;
      xSemaphoreGive(scanMutex);
    } else {
      currentScanIP = 1;
      scanComplete = false;
    }
    startNetworkScan();
  }
}

bool ensureServerConnection() {
  if (client.connected()) {
    return true;
  }

  for (int attempt = 1; attempt <= MAX_RECONNECT_ATTEMPTS; attempt++) {
    addLog("Reconectando ao servidor (tentativa " + String(attempt) + "/" + String(MAX_RECONNECT_ATTEMPTS) + ")...");
    if (client.connect(serverIP.c_str(), serverPort)) {
      addLog("Reconexão bem-sucedida");
      return true;
    }

    addLog("Tentativa de reconexão falhou");
    delay(RETRY_DELAY_MS);
  }

  addLog("Não foi possível reconectar ao servidor após várias tentativas");
  return false;
}

