// mqtt_functions.ino

// mqtt_functions.ino

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
  
  // Otherwise it's a request topic - send to server
  addLog("MQTT Request received: " + message);
  
  // Send command to server if connected
  if (serverFound && client.connected()) {
    Serial.print("Sending to server: ");
    Serial.println(message);
    
    client.println(message);
    client.flush();
    
    // Wait for response
    unsigned long startTime = millis();
    while (!client.available() && (millis() - startTime < 2000)) {
      delay(10);
    }
    
    if (client.available()) {
      String response = client.readStringUntil('\n');
      Serial.print("Server response: ");
      Serial.println(response);
      
      // Send response to MQTT
      if (mqttClient.connected()) {
        String responseTopic = mqttTopicBase + "response";
        mqttClient.publish(responseTopic.c_str(), response.c_str());
      }
    } else {
      addLog("No response from server");
    }
  } else {
    addLog("Cannot send command - server not connected");
  }
}

void handleCommand(String command) {
  command.trim();
  addLog("Command received: " + command);
  
  // Convert to lowercase for comparison
  String cmdLower = command;
  cmdLower.toLowerCase();
  
  if (cmdLower.equals("boot")) {
    addLog("Rebooting device...");
    delay(1000);
    ESP.restart();
  }
  else if (cmdLower.equals("forgetwifi")) {
    addLog("Forgetting WiFi credentials...");
    preferences.clear();
    addLog("WiFi credentials cleared. Rebooting...");
    delay(1000);
    ESP.restart();
  }
  else if (cmdLower.equals("scan")) {
    addLog("Starting network rescan...");
    
    // Stop current server connection
    if (serverFound) {
      xSemaphoreTake(serverMutex, portMAX_DELAY);
      serverFound = false;
      serverIP = "";
      serverPort = 0;
      xSemaphoreGive(serverMutex);
      client.stop();
    }
    
    // Restart scan
    currentScanIP = 1;
    scanComplete = false;
    startNetworkScan();
  }
  else if (cmdLower.startsWith("port ")) {
    handlePortCommand(command);
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
    xSemaphoreGive(serverMutex);
    client.stop();
  }
  
  currentScanIP = 1;
  scanComplete = false;
  startNetworkScan();
}
