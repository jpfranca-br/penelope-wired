// tcp_server.ino

bool sendCommand() {
  if (!client.connected()) {
    addLog("Reconnecting to server...");
    if (!client.connect(serverIP.c_str(), serverPort)) {
      addLog("Reconnection failed!");
      return false;
    }
  }
  
  String cmdMsg = "Sending command to " + serverIP + ":" + String(serverPort);
  Serial.print(cmdMsg);
  Serial.print("... ");
  
  client.println("(&V)");
  client.flush();
  
  // Wait for response
  unsigned long startTime = millis();
  while (!client.available() && (millis() - startTime < 2000)) {
    delay(10);
  }
  
  if (client.available()) {
    String response = client.readStringUntil('\n');
    Serial.print("Response: ");
    Serial.println(response);
    
    // Send to MQTT running topic
    if (mqttClient.connected()) {
      String topic = mqttTopicBase + "running";
      mqttClient.publish(topic.c_str(), response.c_str());
    }
    
    return true;
  } else {
    addLog("No response from server!");
    client.stop();
    return false;
  }
}
