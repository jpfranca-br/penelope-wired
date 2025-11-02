// io.ino

void checkBootButton() {
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    addLog("Boot button pressed!");
    
    // Measure how long button is held
    unsigned long pressStart = millis();
    addLog("Waiting to see if this is a long press...");
    
    // Wait to see if it's a long press
    while (digitalRead(BOOT_BUTTON_PIN) == LOW && (millis() - pressStart) < LONG_PRESS_TIME) {
      delay(100);
      yield();
      if ((millis() - pressStart) % 1000 == 0) {
        Serial.print(".");
      }
    }
    
    unsigned long pressDuration = millis() - pressStart;
    
    if (pressDuration >= LONG_PRESS_TIME) {
      // Long press - Factory reset
      addLog("*** FACTORY RESET ***");
      addLog("Clearing all stored data...");
      
      preferences.begin("wifi-config", false);
      preferences.clear();
      preferences.end();
      
      addLog("All data cleared!");
      addLog("Device will restart...");
      delay(2000);
      ESP.restart();
    } else {
      // Short press - Just log it (no config portal needed)
      addLog("Short press detected - No action");
    }
  }
}