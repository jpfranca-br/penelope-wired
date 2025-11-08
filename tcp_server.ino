// tcp_server.ino

const int MAX_RECONNECT_ATTEMPTS = 3;
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

