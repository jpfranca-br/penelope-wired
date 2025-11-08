// tcp_server.ino

const int MAX_RECONNECT_ATTEMPTS = 3;
const int MAX_COMMAND_ATTEMPTS = 3;
const unsigned long RESPONSE_TIMEOUT = 2000;
const unsigned long RETRY_DELAY_MS = 500;

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

bool sendCommand() {
  for (int attempt = 1; attempt <= MAX_COMMAND_ATTEMPTS; attempt++) {
    if (!ensureServerConnection()) {
      return false;
    }

    String cmdMsg = "Enviando comando para " + serverIP + ":" + String(serverPort);
    if (attempt > 1) {
      cmdMsg += " (retry " + String(attempt) + "/" + String(MAX_COMMAND_ATTEMPTS) + ")";
    }
    Serial.print(cmdMsg);
    Serial.print("... ");

    client.println("(&V)");
    client.flush();
    lastRequestSent = "(&V)";

    unsigned long startTime = millis();
    while (!client.available() && (millis() - startTime < RESPONSE_TIMEOUT)) {
      delay(10);
    }

    if (client.available()) {
      String response = client.readStringUntil('\n');
      Serial.print("Resposta: ");
      Serial.println(response);

      runningTotal = response;

      if (mqttClient.connected()) {
        String topic = mqttTopicBase + "running";
        mqttClient.publish(topic.c_str(), response.c_str());
      }

      return true;
    }

    addLog("Nenhuma resposta do servidor (tentativa " + String(attempt) + "/" + String(MAX_COMMAND_ATTEMPTS) + ")");
    runningTotal = "Nenhuma resposta do servidor";
    client.stop();
    delay(RETRY_DELAY_MS);
  }

  addLog("Servidor não responde após várias tentativas");
  runningTotal = "Servidor não está respondendo";
  return false;
}
