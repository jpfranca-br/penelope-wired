// mqtt_functions.ino

// mqtt_functions.ino

void handleWifiPasswordCommand(String command);
void handleFactoryResetCommand();
void handleIpConfigCommand(String command);
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
  
  // Otherwise it's a request topic - send to server
  addLog("MQTT Request received: " + message);
  lastRequestSent = message;
  
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
      lastResponseReceived = response;
      if (mqttClient.connected()) {
        String responseTopic = mqttTopicBase + "response";
        mqttClient.publish(responseTopic.c_str(), response.c_str());
      }
    } else {
      addLog("No response from server");
      lastResponseReceived = "No response from server";
    }
  } else {
    addLog("Cannot send command - server not connected");
    lastResponseReceived = "Server not connected";
  }
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
