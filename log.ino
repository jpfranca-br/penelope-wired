void addLog(String message) {
  // Add to buffer
  logBuffer[logIndex] = message;
  logIndex = (logIndex + 1) % MAX_LOG_LINES;
  if (logCount < MAX_LOG_LINES) logCount++;
  
  // Print to Serial
  Serial.println(message);
  
  // Try to send to MQTT if connected
  if (mqttClient.connected()) {
    String topic = mqttTopicBase + "log";
    mqttClient.publish(topic.c_str(), message.c_str());
  }
}
