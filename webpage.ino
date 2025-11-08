String escapeJson(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

void sendConfigPage(const String &message, bool success);
extern bool isWiredDhcp();
extern String getWiredIpSetting();
extern String getWiredMaskSetting();
extern String getWiredGatewaySetting();
extern String getWiredDnsSetting();
extern bool setWiredConfiguration(bool useDhcp, const String &ip, const String &mask, const String &gateway, const String &dns, String &errorMessage);

void handleMonitor() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Monitor Sylvester</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "<script>";
  html += "let autoScroll = true;";
  html += "function updateLogs() {";
  html += "  fetch('/logs').then(r => r.json()).then(data => {";
  html += "    const logDiv = document.getElementById('logs');";
  html += "    const wasAtBottom = logDiv.scrollHeight - logDiv.scrollTop <= logDiv.clientHeight + 50;";
  html += "    logDiv.innerHTML = data.logs.map(l => '<div class=\"log-line\">' + escapeHtml(l) + '</div>').join('');";
  html += "    if (autoScroll && wasAtBottom) logDiv.scrollTop = logDiv.scrollHeight;";
  html += "    const macEl = document.getElementById('mac');";
  html += "    if (macEl) macEl.textContent = 'MAC: ' + (data.mac || 'N/D');";
  html += "    const wiredEl = document.getElementById('wired-ip');";
  html += "    if (wiredEl) wiredEl.textContent = 'IP cabeado: ' + (data.wired_ip || 'N/D');";
  html += "    const internetEl = document.getElementById('internet-ip');";
  html += "    if (internetEl) internetEl.textContent = 'Internet: ' + (data.internet_ip || 'N/D');";
  html += "    const commandEl = document.getElementById('last-command');";
  html += "    if (commandEl) commandEl.textContent = data.last_command || 'Nenhum';";
  html += "    const requestEl = document.getElementById('last-request');";
  html += "    if (requestEl) requestEl.textContent = data.last_request || 'Nenhum';";
  html += "    const responseEl = document.getElementById('last-response');";
  html += "    if (responseEl) responseEl.textContent = data.last_response || 'Nenhum';";
  html += "    const runningEl = document.getElementById('running-total');";
  html += "    if (runningEl) runningEl.textContent = data.running_total || 'Nenhum';";
  html += "    document.getElementById('status').innerText = data.eth ? 'Conectado' : 'Desconectado';";
  html += "    document.getElementById('status').className = data.eth ? 'status connected' : 'status disconnected';";
  html += "  });";
  html += "}";
  html += "function escapeHtml(text) {";
  html += "  const div = document.createElement('div');";
  html += "  div.textContent = text;";
  html += "  return div.innerHTML;";
  html += "}";
  html += "setInterval(updateLogs, 1000);";
  html += "window.onload = updateLogs;";
  html += "</script></head><body>";
  html += "<div class='container'>";
  html += "<div class='header'>";
  html += "<h1>🔍 Monitor Sylvester</h1>";
  html += "<div class='info'>";
  html += "<span id='mac'>MAC: --</span>";
  html += "<span id='wired-ip'>IP cabeado: --</span>";
  html += "<span id='internet-ip'>Internet: --</span>";
  html += "<span id='status' class='status'>Verificando...</span>";
  html += "</div>";
  html += "<a class='config-button' href='/config'>Configurar</a>";
  html += "</div>";
  html += "<div class='summary'>";
  html += "<div class='summary-card full'><h2>Último comando</h2><div class='summary-value' id='last-command'>Nenhum</div></div>";
  html += "</div>";
  html += "<div class='activity-grid'>";
  html += "<div class='detail-card'><h2>Última requisição</h2><div class='detail-value' id='last-request'>Nenhum</div></div>";
  html += "<div class='detail-card'><h2>Última resposta</h2><div class='detail-value' id='last-response'>Nenhum</div></div>";
  html += "<div class='detail-card'><h2>Total em execução</h2><div class='detail-value' id='running-total'>Nenhum</div></div>";
  html += "</div>";
  html += "<div id='logs' class='log-container'></div>";
  html += "<div class='footer'>Atualização automática a cada segundo | Rolagem travada no fim</div>";
  html += "</div></body></html>";

  server.send(200, "text/html", html);
}

void handleLogs() {
  String json = "{\"logs\":[";
  
  int start = logCount < MAX_LOG_LINES ? 0 : logIndex;
  for (int i = 0; i < logCount; i++) {
    int idx = (start + i) % MAX_LOG_LINES;
    if (i > 0) json += ",";
    json += "\"";
    // Escape quotes in log messages
    String msg = logBuffer[idx];
    msg.replace("\\", "\\\\");
    msg.replace("\"", "\\\"");
    msg.replace("\n", "\\n");
    msg.replace("\r", "\\r");
    json += msg;
    json += "\"";
  }

  json += "],\"eth\":";
  json += eth_connected ? "true" : "false";
  json += ",\"mac\":\"" + escapeJson(deviceMac) + "\"";
  json += ",\"wired_ip\":\"" + escapeJson(wiredIP) + "\"";
  json += ",\"internet_ip\":\"" + escapeJson(internetAddress) + "\"";
  json += ",\"last_command\":\"" + escapeJson(lastCommandReceived) + "\"";
  json += ",\"last_request\":\"" + escapeJson(lastRequestSent) + "\"";
  json += ",\"last_response\":\"" + escapeJson(lastResponseReceived) + "\"";
  json += ",\"running_total\":\"" + escapeJson(runningTotal) + "\"";
  json += "}";

  server.send(200, "application/json", json);
}

void handleCSS() {
  String css = "* { margin: 0; padding: 0; box-sizing: border-box; }";
  css += "body { font-family: 'Courier New', monospace; background: #0a0a0a; color: #00ff00; }";
  css += ".container { max-width: 100%; height: 100vh; display: flex; flex-direction: column; }";
  css += ".header { background: #1a1a1a; padding: 15px 20px; border-bottom: 2px solid #00ff00; }";
  css += ".header h1 { font-size: 24px; margin-bottom: 10px; }";
  css += ".info { display: flex; gap: 20px; font-size: 14px; flex-wrap: wrap; align-items: center; }";
  css += ".config-button { margin-left: auto; padding: 6px 14px; border: 1px solid #00ff00; border-radius: 4px; color: #00ff00; text-decoration: none; font-size: 14px; transition: background 0.2s ease; }";
  css += ".config-button:hover { background: #00ff00; color: #000; }";
  css += ".status { padding: 2px 8px; border-radius: 3px; font-weight: bold; }";
  css += ".status.connected { background: #00ff00; color: #000; }";
  css += ".status.disconnected { background: #ff0000; color: #fff; }";
  css += ".summary { display: flex; gap: 15px; padding: 15px 20px; background: #101010; border-bottom: 1px solid #00ff00; flex-wrap: wrap; }";
  css += ".summary-card { flex: 1; min-width: 200px; background: #0f1f0f; border: 1px solid #00ff00; padding: 10px 15px; border-radius: 6px; }";
  css += ".summary-card.full { flex: 1 1 100%; }";
  css += ".summary-card h2 { font-size: 14px; margin-bottom: 8px; text-transform: uppercase; letter-spacing: 1px; color: #7fff7f; }";
  css += ".summary-value { font-size: 16px; font-weight: bold; word-break: break-word; white-space: pre-wrap; }";
  css += ".activity-grid { display: grid; gap: 15px; padding: 15px 20px; background: #101010; border-bottom: 1px solid #00ff00; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); }";
  css += ".detail-card { background: #0f1f0f; border: 1px solid #00ff00; padding: 12px 15px; border-radius: 6px; display: flex; flex-direction: column; gap: 8px; }";
  css += ".detail-card h2 { font-size: 14px; text-transform: uppercase; letter-spacing: 1px; color: #7fff7f; }";
  css += ".detail-value { font-size: 15px; font-weight: bold; white-space: pre-wrap; word-break: break-word; min-height: 24px; }";
  css += ".log-container { flex: 1; overflow-y: auto; padding: 10px 20px; background: #0a0a0a; }";
  css += ".log-line { padding: 4px 0; border-bottom: 1px solid #1a3a1a; font-size: 13px; word-wrap: break-word; }";
  css += ".log-line:hover { background: #1a1a1a; }";
  css += ".footer { background: #1a1a1a; padding: 10px 20px; text-align: center; font-size: 12px; border-top: 1px solid #00ff00; }";
  css += "::-webkit-scrollbar { width: 10px; }";
  css += "::-webkit-scrollbar-track { background: #1a1a1a; }";
  css += "::-webkit-scrollbar-thumb { background: #00ff00; border-radius: 5px; }";
  css += "::-webkit-scrollbar-thumb:hover { background: #00cc00; }";
  css += ".config-container { max-width: 520px; margin: 40px auto; background: #0f1f0f; border: 1px solid #00ff00; border-radius: 8px; padding: 20px; box-shadow: 0 0 20px rgba(0, 255, 0, 0.1); }";
  css += ".config-container h1 { font-size: 22px; margin-bottom: 10px; text-align: center; }";
  css += ".config-description { font-size: 14px; margin-bottom: 20px; color: #7fff7f; text-align: center; }";
  css += ".config-form { display: flex; flex-direction: column; gap: 16px; }";
  css += ".mode-selection { display: flex; gap: 20px; justify-content: center; }";
  css += ".mode-selection label { display: flex; align-items: center; gap: 6px; cursor: pointer; }";
  css += ".static-fields { display: grid; gap: 12px; grid-template-columns: 1fr; }";
  css += ".static-fields label { display: flex; flex-direction: column; gap: 6px; font-size: 14px; color: #7fff7f; }";
  css += ".static-fields input { padding: 10px; background: #000; border: 1px solid #00ff00; border-radius: 4px; color: #00ff00; font-family: inherit; }";
  css += ".config-actions { display: flex; justify-content: space-between; align-items: center; gap: 12px; }";
  css += ".config-actions button { flex: 1; padding: 10px; background: #00ff00; color: #000; border: none; border-radius: 4px; font-weight: bold; cursor: pointer; transition: background 0.2s ease; }";
  css += ".config-actions button:hover { background: #00cc00; }";
  css += ".back-button { flex: 1; text-align: center; padding: 10px; border: 1px solid #00ff00; border-radius: 4px; text-decoration: none; color: #00ff00; font-weight: bold; transition: background 0.2s ease; }";
  css += ".back-button:hover { background: #00ff00; color: #000; }";
  css += ".config-message { padding: 10px; border-radius: 4px; text-align: center; font-size: 14px; }";
  css += ".config-message.success { border: 1px solid #00ff00; color: #00ff00; }";
  css += ".config-message.error { border: 1px solid #ff0000; color: #ff8080; }";
  css += "@media (max-width: 480px) { .config-actions { flex-direction: column; } .config-button { margin-left: 0; margin-top: 10px; } }";

  server.send(200, "text/css", css);
}

void sendConfigPage(const String &message, bool success) {
  bool useDhcp = isWiredDhcp();

  String ipSetting = getWiredIpSetting();
  String maskSetting = getWiredMaskSetting();
  String gatewaySetting = getWiredGatewaySetting();
  String dnsSetting = getWiredDnsSetting();

  String ipPlaceholder = ipSetting.length() > 0 ? ipSetting : "192.168.1.200";
  String maskPlaceholder = maskSetting.length() > 0 ? maskSetting : "255.255.255.0";
  String gatewayPlaceholder = gatewaySetting.length() > 0 ? gatewaySetting : "192.168.1.1";
  String dnsPlaceholder = dnsSetting.length() > 0 ? dnsSetting : "8.8.8.8";

  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Configuração de Ethernet</title>";
  html += "<link rel='stylesheet' href='/style.css'>";
  html += "</head><body>";
  html += "<div class='config-container'>";
  html += "<h1>Configuração de Ethernet</h1>";
  html += "<p class='config-description'>Escolha DHCP ou defina um endereço IP fixo para a interface cabeada.</p>";

  if (message.length() > 0) {
    html += "<div class='config-message ";
    html += success ? "success'>" : "error'>";
    html += message;
    html += "</div>";
  }

  html += "<form method='POST' action='/config' class='config-form'>";
  html += "<div class='mode-selection'>";
  html += "<label><input type='radio' id='mode-dhcp' name='mode' value='dhcp'";
  if (useDhcp) html += " checked";
  html += ">DHCP</label>";
  html += "<label><input type='radio' id='mode-fixed' name='mode' value='fixed'";
  if (!useDhcp) html += " checked";
  html += ">Fixo</label>";
  html += "</div>";

  html += "<div id='static-fields' class='static-fields'>";
  html += "<label>Endereço IP<input type='text' name='ip' placeholder='" + ipPlaceholder + "' value='";
  html += useDhcp ? "" : ipSetting;
  html += "'></label>";
  html += "<label>Máscara de sub-rede<input type='text' name='mask' placeholder='" + maskPlaceholder + "' value='";
  html += useDhcp ? "" : maskSetting;
  html += "'></label>";
  html += "<label>Gateway<input type='text' name='gateway' placeholder='" + gatewayPlaceholder + "' value='";
  html += useDhcp ? "" : gatewaySetting;
  html += "'></label>";
  html += "<label>DNS<input type='text' name='dns' placeholder='" + dnsPlaceholder + "' value='";
  html += useDhcp ? "" : dnsSetting;
  html += "'></label>";
  html += "</div>";

  html += "<div class='config-actions'>";
  html += "<button type='submit'>Aplicar</button>";
  html += "<a class='back-button' href='/'>Voltar</a>";
  html += "</div>";
  html += "</form>";
  html += "</div>";

  html += "<script>";
  html += "const dhcpRadio=document.getElementById('mode-dhcp');";
  html += "const fixedRadio=document.getElementById('mode-fixed');";
  html += "const staticFields=document.getElementById('static-fields');";
  html += "function updateMode(){const fixed=fixedRadio.checked;staticFields.style.display=fixed?'grid':'none';staticFields.querySelectorAll('input').forEach(el=>el.required=fixed);}";
  html += "dhcpRadio.addEventListener('change',updateMode);";
  html += "fixedRadio.addEventListener('change',updateMode);";
  html += "updateMode();";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleConfigPage() {
  sendConfigPage("", true);
}

void handleConfigSubmit() {
  if (!server.hasArg("mode")) {
    sendConfigPage("Modo de configuração é obrigatório", false);
    return;
  }

  String mode = server.arg("mode");
  mode.trim();
  mode.toLowerCase();

  bool useDhcp;
  if (mode == "dhcp") {
    useDhcp = true;
  } else if (mode == "fixed") {
    useDhcp = false;
  } else {
    sendConfigPage("Modo de configuração desconhecido", false);
    return;
  }

  String ip = server.hasArg("ip") ? server.arg("ip") : "";
  String mask = server.hasArg("mask") ? server.arg("mask") : "";
  String gateway = server.hasArg("gateway") ? server.arg("gateway") : "";
  String dns = server.hasArg("dns") ? server.arg("dns") : "";

  String errorMessage;
  if (setWiredConfiguration(useDhcp, ip, mask, gateway, dns, errorMessage)) {
    sendConfigPage("Configuração da Ethernet cabeada atualizada.", true);
  } else {
    sendConfigPage("Erro de configuração: " + errorMessage, false);
  }
}
