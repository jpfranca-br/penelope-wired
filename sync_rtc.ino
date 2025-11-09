static bool syncRtcWithNtp(bool logIfDisconnected) {
  if (!eth_connected) {
    if (logIfDisconnected) {
      addLog("Não é possível sincronizar o RTC - Ethernet não conectada");
    }
    return false;
  }

  addLog("Sincronizando RTC com servidores NTP...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  const time_t MIN_VALID_TIME = 1609459200;  // 2021-01-01 00:00:00 UTC
  const int MAX_ATTEMPTS = 20;
  bool synced = false;

  for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
    time_t now = time(nullptr);
    if (now >= MIN_VALID_TIME) {
      struct tm timeInfo;
      gmtime_r(&now, &timeInfo);
      char buffer[32];
      strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeInfo);
      addLog(String("RTC sincronizado em ") + buffer + " UTC");
      synced = true;
      break;
    }

    delay(200);
    yield();
  }

  if (!synced) {
    addLog("Falha ao sincronizar RTC via NTP");
  }

  lastRtcSyncAttempt = millis();
  return synced;
}
