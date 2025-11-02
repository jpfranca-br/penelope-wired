// network_scanner.ino

#include <ETH.h>

void startNetworkScan() {
  if (!eth_connected) {
    addLog("Cannot start scan - Ethernet not connected");
    return;
  }

  IPAddress localIP = ETH.localIP();
  if (localIP == IPAddress(0, 0, 0, 0)) {
    addLog("Cannot start scan - Ethernet IP not assigned");
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

  addLog("Starting multi-threaded network scan...");
  addLog("Scan range: " + scanRange);
  addLog("Using " + String(NUM_SCAN_TASKS) + " parallel threads");

  // Create multiple scanning tasks
  for (int i = 0; i < NUM_SCAN_TASKS; i++) {
    char taskName[20];
    sprintf(taskName, "ScanTask%d", i);
    xTaskCreate(
      scanTask,           // Task function
      taskName,           // Task name
      4096,               // Stack size
      NULL,               // Parameters
      1,                  // Priority
      NULL                // Task handle
    );
  }
}

void scanTask(void *parameter) {
  IPAddress localIP = ETH.localIP();
  IPAddress subnet = ETH.subnetMask();
  IPAddress network;

  for (int i = 0; i < 4; i++) {
    network[i] = localIP[i] & subnet[i];
  }

  while (true) {
    if (!eth_connected) {
      vTaskDelete(NULL);
      return;
    }

    // Check if server already found
    xSemaphoreTake(serverMutex, portMAX_DELAY);
    bool found = serverFound;
    xSemaphoreGive(serverMutex);

    if (found) {
      vTaskDelete(NULL); // Delete this task
      return;
    }

    // Get next IP to scan
    xSemaphoreTake(scanMutex, portMAX_DELAY);
    int ipToScan = currentScanIP++;
    if (ipToScan >= 255) {
      scanComplete = true;
      xSemaphoreGive(scanMutex);
      vTaskDelete(NULL); // Delete this task
      return;
    }
    // Show progress every 10 IPs
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
      
      String progress = "Scanning " + startIP.toString() + " to " + endIP.toString() + 
                       " - Ports " + portList;
      addLog(progress);
    }
    xSemaphoreGive(scanMutex);
    
    // Build target IP
    IPAddress targetIP = network;
    targetIP[3] = ipToScan;

    // Skip our own IP
    if (targetIP == localIP) continue;

    // Show which IP is being scanned
    Serial.print(".");
    if (ipToScan % 50 == 0) Serial.println(); // New line every 50 IPs
    
    // Scan this IP
    WiFiClient testClient;
    for (int p = 0; p < numPorts; p++) {
      testClient.setTimeout(300);
      if (testClient.connect(targetIP, ports[p], 300)) {
        Serial.print("\n[Found] ");
        Serial.print(targetIP);
        Serial.print(":");
        Serial.print(ports[p]);
        Serial.print(" OPEN - Testing... ");
        
        // Send test command
        testClient.println("(&V)");
        testClient.flush();
        
        // Wait for response
        unsigned long startTime = millis();
        while (!testClient.available() && (millis() - startTime < 1000)) {
          delay(10);
        }
        
        if (testClient.available()) {
          String response = testClient.readStringUntil('\n');
          Serial.print("RESPONSE: ");
          Serial.println(response);
          
          // Found the server!
          xSemaphoreTake(serverMutex, portMAX_DELAY);
          if (!serverFound) {  // Double-check another thread didn't find it
            serverIP = targetIP.toString();
            serverPort = ports[p];
            serverFound = true;
            
            // Connect main client
            client.connect(targetIP, ports[p]);
            lastCommandTime = millis();
            
            addLog("*** SERVER FOUND! ***");
            addLog("Server: " + serverIP + ":" + String(serverPort));
            addLog("Stopping all scan threads...");
          }
          xSemaphoreGive(serverMutex);
          
          testClient.stop();
          vTaskDelete(NULL); // Delete this task
          return;
        } else {
          Serial.println("no response");
        }
        
        testClient.stop();
      }
    }
    
    delay(10); // Small delay between IPs
  }
}

void loadPorts() {
  yield(); // Feed watchdog at start
  
  numPorts = preferences.getInt("numPorts", 0);
  
  if (numPorts == 0 || numPorts > 10) {
    numPorts = defaultNumPorts;
    for (int i = 0; i < numPorts; i++) {
      ports[i] = defaultPorts[i];
    }
    Serial.println("Using default ports: 2001, 1771, 854");
  } else {
    for (int i = 0; i < numPorts; i++) {
      String key = "port" + String(i);
      ports[i] = preferences.getInt(key.c_str(), defaultPorts[i % defaultNumPorts]);
    }
    Serial.print("Loaded ports from memory: ");
    for (int i = 0; i < numPorts; i++) {
      Serial.print(ports[i]);
      if (i < numPorts - 1) Serial.print(", ");
    }
    Serial.println();
  }
  
  yield(); // Feed watchdog at end
}
