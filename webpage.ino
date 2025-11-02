String escapeJson(String value) {
  value.replace("\\", "\\\\");
  value.replace("\"", "\\\"");
  value.replace("\n", "\\n");
  value.replace("\r", "\\r");
  return value;
}

void handleMonitor() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  html += "<title>Penelope Monitor</title>";
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
  html += "    if (macEl) macEl.textContent = 'MAC: ' + (data.mac || 'N/A');";
  html += "    const wiredEl = document.getElementById('wired-ip');";
  html += "    if (wiredEl) wiredEl.textContent = 'Wired IP: ' + (data.wired_ip || 'N/A');";
  html += "    const internetEl = document.getElementById('internet-ip');";
  html += "    if (internetEl) internetEl.textContent = 'Internet: ' + (data.internet_ip || 'N/A');";
  html += "    const commandEl = document.getElementById('last-command');";
  html += "    if (commandEl) commandEl.textContent = data.last_command || 'None';";
  html += "    const requestEl = document.getElementById('last-request');";
  html += "    if (requestEl) requestEl.textContent = data.last_request || 'None';";
  html += "    const responseEl = document.getElementById('last-response');";
  html += "    if (responseEl) responseEl.textContent = data.last_response || 'None';";
  html += "    document.getElementById('status').innerText = data.eth ? 'Connected' : 'Disconnected';";
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
  html += "<h1>🔍 Penelope Monitor</h1>";
  html += "<div class='info'>";
  html += "<span id='mac'>MAC: --</span>";
  html += "<span id='wired-ip'>Wired IP: --</span>";
  html += "<span id='internet-ip'>Internet: --</span>";
  html += "<span id='status' class='status'>Checking...</span>";
  html += "</div></div>";
  html += "<div class='summary'>";
  html += "<div class='summary-card'><h2>Last Command</h2><div class='summary-value' id='last-command'>None</div></div>";
  html += "<div class='summary-card'><h2>Last Request</h2><div class='summary-value' id='last-request'>None</div></div>";
  html += "<div class='summary-card'><h2>Last Response</h2><div class='summary-value' id='last-response'>None</div></div>";
  html += "</div>";
  html += "<div id='logs' class='log-container'></div>";
  html += "<div class='footer'>Auto-updating every second | Scroll locked to bottom</div>";
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
  css += ".status { padding: 2px 8px; border-radius: 3px; font-weight: bold; }";
  css += ".status.connected { background: #00ff00; color: #000; }";
  css += ".status.disconnected { background: #ff0000; color: #fff; }";
  css += ".summary { display: flex; gap: 15px; padding: 15px 20px; background: #101010; border-bottom: 1px solid #00ff00; flex-wrap: wrap; }";
  css += ".summary-card { flex: 1; min-width: 200px; background: #0f1f0f; border: 1px solid #00ff00; padding: 10px 15px; border-radius: 6px; }";
  css += ".summary-card h2 { font-size: 14px; margin-bottom: 8px; text-transform: uppercase; letter-spacing: 1px; color: #7fff7f; }";
  css += ".summary-value { font-size: 16px; font-weight: bold; word-break: break-word; }";
  css += ".log-container { flex: 1; overflow-y: auto; padding: 10px 20px; background: #0a0a0a; }";
  css += ".log-line { padding: 4px 0; border-bottom: 1px solid #1a3a1a; font-size: 13px; word-wrap: break-word; }";
  css += ".log-line:hover { background: #1a1a1a; }";
  css += ".footer { background: #1a1a1a; padding: 10px 20px; text-align: center; font-size: 12px; border-top: 1px solid #00ff00; }";
  css += "::-webkit-scrollbar { width: 10px; }";
  css += "::-webkit-scrollbar-track { background: #1a1a1a; }";
  css += "::-webkit-scrollbar-thumb { background: #00ff00; border-radius: 5px; }";
  css += "::-webkit-scrollbar-thumb:hover { background: #00cc00; }";
  
  server.send(200, "text/css", css);
}