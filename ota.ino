#include <HTTPClient.h>
#include <Update.h>
#include <StreamString.h>
#include <WiFiClient.h>

extern void addLog(String message);
extern bool beginHttpDownload(const String &url,
                              HTTPClient &http,
                              WiFiClient *&clientOut,
                              String &errorMessage,
                              bool &usedSecureTransport,
                              bool forceInsecure);
extern bool downloadTextFile(const String &url, String &content, String &errorMessage);
extern String describeTlsError(WiFiClient *client, bool usedSecureTransport);
extern const char* otaRootCACertificate;

static bool isOtaCertificateConfigured() {
  return otaRootCACertificate != nullptr && otaRootCACertificate[0] != '\0';
}

static String describeUpdateError() {
#if defined(ESP32)
  const char *error = Update.errorString();
  if (error != nullptr) {
    return String(error);
  }
#endif

  StreamString errorStream;
  Update.printError(errorStream);
  if (errorStream.length() > 0) {
    return String(errorStream.c_str());
  }

  return String("Erro desconhecido (código ") + String(Update.getError()) + ")";
}

static String parseMd5FromContent(String content) {
  content.trim();
  content.replace("\r", "\n");

  String md5 = "";
  for (size_t i = 0; i < content.length(); ++i) {
    char c = content.charAt(i);
    if (c == '\n' || c == '\t' || c == ' ') {
      break;
    }
    md5 += c;
  }

  md5.trim();
  md5.toLowerCase();
  return md5;
}

void performOtaUpdate(const String &binUrl, const String &md5Url) {
  addLog("Iniciando atualização OTA");
  addLog("Solicitando MD5 em: " + md5Url);

  String errorMessage;
  String md5Content;
  if (!downloadTextFile(md5Url, md5Content, errorMessage)) {
    addLog("Falha ao baixar arquivo MD5: " + errorMessage);
    return;
  }

  String expectedMd5 = parseMd5FromContent(md5Content);
  if (expectedMd5.length() != 32) {
    addLog("MD5 inválido recebido: " + expectedMd5);
    return;
  }

  addLog("MD5 esperado: " + expectedMd5);
  addLog("Baixando firmware em: " + binUrl);

  HTTPClient http;
  WiFiClient *client = nullptr;
  bool usedSecureTransport = false;
  bool forceInsecure = false;

  while (true) {
    if (!beginHttpDownload(binUrl, http, client, errorMessage, usedSecureTransport, forceInsecure)) {
      if (!forceInsecure && isOtaCertificateConfigured()) {
        addLog("Falha ao iniciar download do firmware: " + errorMessage +
               ". Tentando novamente sem validação de certificado.");
        forceInsecure = true;
        continue;
      }

      addLog("Falha ao iniciar download do firmware: " + errorMessage);
      return;
    }

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      break;
    }

    String reason = http.errorToString(httpCode);
    String message = "Download do firmware falhou: HTTP " + String(httpCode);
    if (reason.length() > 0) {
      message += " - " + reason;
    }
#if defined(ESP32)
    if (httpCode <= 0) {
      String tlsError = describeTlsError(client, usedSecureTransport);
      if (tlsError.length() > 0) {
        message += " | " + tlsError;
      }
    }
#else
    (void)usedSecureTransport;
#endif

    bool canRetryInsecure = (!forceInsecure) && usedSecureTransport && isOtaCertificateConfigured() && (httpCode <= 0);
    if (canRetryInsecure) {
      addLog(message + ". Tentando novamente sem validação de certificado.");
      http.end();
      forceInsecure = true;
      continue;
    }

    addLog(message);
    http.end();
    return;
  }

  int contentLength = http.getSize();
  if (contentLength <= 0) {
    addLog("Aviso: servidor não informou o tamanho do firmware");
  } else {
    addLog("Tamanho do firmware: " + String(contentLength) + " bytes");
  }

  WiFiClient *stream = http.getStreamPtr();
  if (stream == nullptr) {
    addLog("Fluxo de download indisponível");
    http.end();
    return;
  }
  stream->setTimeout(15000);

  size_t updateSize = contentLength > 0 ? static_cast<size_t>(contentLength) : UPDATE_SIZE_UNKNOWN;
  if (!Update.begin(updateSize)) {
    addLog("Update.begin falhou: " + describeUpdateError());
    http.end();
    return;
  }

  if (!Update.setMD5(expectedMd5.c_str())) {
    addLog("Falha ao configurar MD5 esperado");
    Update.abort();
    http.end();
    return;
  }

  size_t written = Update.writeStream(*stream);
  if (written == 0) {
    addLog("Nenhum dado escrito durante a atualização OTA");
    Update.abort();
    http.end();
    return;
  }

  if (contentLength > 0 && written != static_cast<size_t>(contentLength)) {
    addLog("Tamanho baixado inesperado: " + String(written) + " de " + String(contentLength) + " bytes");
    Update.abort();
    http.end();
    return;
  }

  if (Update.hasError()) {
    addLog("Erro durante gravação OTA: " + describeUpdateError());
    Update.abort();
    http.end();
    return;
  }

  if (!Update.end()) {
    addLog("Update.end falhou: " + describeUpdateError());
    http.end();
    return;
  }

  if (!Update.isFinished()) {
    addLog("Atualização OTA incompleta");
    http.end();
    return;
  }

  http.end();
  addLog("Atualização OTA concluída com sucesso. Reiniciando...");
  delay(1000);
  ESP.restart();
}
