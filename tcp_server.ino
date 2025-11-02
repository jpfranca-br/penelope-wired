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
    addLog("Reconnecting to server (attempt " + String(attempt) + "/" + String(MAX_RECONNECT_ATTEMPTS) + ")...");
    if (client.connect(serverIP.c_str(), serverPort)) {
      addLog("Reconnection successful");
      return true;
    }

    addLog("Reconnection attempt failed");
    delay(RETRY_DELAY_MS);
  }

  addLog("Unable to reconnect to server after multiple attempts");
  return false;
}

bool sendCommand() {
  for (int attempt = 1; attempt <= MAX_COMMAND_ATTEMPTS; attempt++) {
    if (!ensureServerConnection()) {
      return false;
    }

    String cmdMsg = "Sending command to " + serverIP + ":" + String(serverPort);
    if (attempt > 1) {
      cmdMsg += " (retry " + String(attempt) + "/" + String(MAX_COMMAND_ATTEMPTS) + ")";
    }
    Serial.print(cmdMsg);
    Serial.print("... ");

    client.println("(&V)");
    client.flush();

    unsigned long startTime = millis();
    while (!client.available() && (millis() - startTime < RESPONSE_TIMEOUT)) {
      delay(10);
    }

    if (client.available()) {
      String response = client.readStringUntil('\n');
      Serial.print("Response: ");
      Serial.println(response);

      if (mqttClient.connected()) {
        String topic = mqttTopicBase + "running";
        mqttClient.publish(topic.c_str(), response.c_str());
      }

      return true;
    }

    addLog("No response from server (attempt " + String(attempt) + "/" + String(MAX_COMMAND_ATTEMPTS) + ")");
    client.stop();
    delay(RETRY_DELAY_MS);
  }

  addLog("Server not responding after multiple retries");
  return false;
}
