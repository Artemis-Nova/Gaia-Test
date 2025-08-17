#define FIRMWARE_VERSION "0.0.7"

/* Gaia OTA desde GitHub - Versión Optimizada
   - Usa chunks pequeños con gestión inteligente del watchdog
   - Detiene AsyncTCP durante escritura crítica
   - Manejo robusto de memoria y errores
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <HTTPClient.h>
#include <Update.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>

// ---------------- Credenciales ----------------
const char* ssid = "INFINITUMCDB2";
const char* password = "UC4Zn9xZue";
const char* hostname = "gaia";

AsyncWebServer server(80);

// ---------------- Variables globales ----------------
String latestVersion = "";
String latestBinUrl = "";
bool otaInProgress = false;

// ---------------- OTA helpers ----------------
bool startOTAUpdate(WiFiClient* client, int contentLength) {
  Serial.println("[OTA] Iniciando escritura al flash...");

  if (!Update.begin(contentLength)) {
    Serial.printf("[OTA] Error al iniciar Update: %s\n", Update.errorString());
    return false;
  }

  // Usar chunks de 64 bytes - muy pequeños para minimizar bloqueos
  const size_t CHUNK_SIZE = 64;
  uint8_t buff[CHUNK_SIZE];
  size_t written = 0;
  unsigned long lastPrint = millis();
  unsigned long lastYield = millis();

  Serial.println("[OTA] Escribiendo firmware por chunks pequeños...");
  
  while (written < contentLength) {
    size_t remaining = contentLength - written;
    size_t toRead = (remaining < CHUNK_SIZE) ? remaining : CHUNK_SIZE;
    
    int readLen = client->readBytes(buff, toRead);
    if (readLen > 0) {
      if (Update.write(buff, readLen) != readLen) {
        Serial.printf("[OTA] Error al escribir chunk: %s\n", Update.errorString());
        Update.abort();
        return false;
      }
      written += readLen;
    } else if (!client->connected() && !client->available()) {
      Serial.println("[OTA] Conexión cerrada prematuramente");
      Update.abort();
      return false;
    }

    // Progreso cada 3 segundos
    if (millis() - lastPrint > 3000) {
      int progress = (written * 100) / contentLength;
      Serial.printf("[OTA] Escritura: %d%% (%d/%d bytes)\n", progress, written, contentLength);
      lastPrint = millis();
    }

    // Yield MUY frecuente - cada 10ms y después de cada chunk
    if (millis() - lastYield > 10) {
      yield();
      vTaskDelay(1); // Dar tiempo al scheduler de FreeRTOS
      lastYield = millis();
    }
    
    // Yield adicional después de cada chunk
    yield();
  }

  if (written != contentLength) {
    Serial.printf("[OTA] Error: escritos %d de %d bytes\n", written, contentLength);
    Update.abort();
    return false;
  }

  if (!Update.end()) {
    Serial.printf("[OTA] Error al finalizar: %s\n", Update.errorString());
    return false;
  }

  Serial.println("[OTA] Escritura completada exitosamente!");
  return true;
}

void performFirmwareUpdate(String url) {
  Serial.println("[OTA] ===========================================");
  Serial.println("[OTA] Iniciando proceso de actualización OTA");
  Serial.println("[OTA] ===========================================");
  
  otaInProgress = true;

  // ETAPA 1: Detener servidor web para liberar recursos
  Serial.println("[OTA] Etapa 1: Deteniendo servidor web...");
  server.end();
  delay(500); // Dar tiempo para que se liberen los recursos
  
  // ETAPA 2: Configurar watchdog con timeout más largo
  Serial.println("[OTA] Etapa 2: Configurando watchdog...");
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms = 15000, // 15 segundos - más tiempo
    .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic = false // NO hacer panic, solo advertir
  };
  esp_task_wdt_reconfigure(&wdt_config);

  // ETAPA 3: Descargar y escribir firmware
  Serial.println("[OTA] Etapa 3: Iniciando descarga desde: " + url);
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(90000); // 90 segundos - muy generoso
  http.begin(url);

  int httpCode = http.GET();
  Serial.printf("[OTA] HTTP respuesta: %d\n", httpCode);

  if (httpCode == HTTP_CODE_OK) {
    int contentLength = http.getSize();
    Serial.printf("[OTA] Tamaño firmware: %d bytes (%.1f MB)\n", contentLength, contentLength / 1024.0 / 1024.0);

    if (contentLength > 0) {
      WiFiClient* stream = http.getStreamPtr();
      
      Serial.println("[OTA] Etapa 4: Iniciando escritura directa...");
      delay(100); // Pequeña pausa antes de empezar
      
      if (startOTAUpdate(stream, contentLength)) {
        Serial.println("[OTA] ===========================================");
        Serial.println("[OTA] ACTUALIZACIÓN COMPLETADA EXITOSAMENTE");
        Serial.println("[OTA] Reiniciando dispositivo en 5 segundos...");
        Serial.println("[OTA] ===========================================");
        
        http.end();
        delay(5000);
        ESP.restart();
      } else {
        Serial.println("[OTA] Error durante la escritura");
        http.end();
        otaInProgress = false;
        
        // Restaurar watchdog original
        esp_task_wdt_config_t original_config = {
          .timeout_ms = 5000,
          .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
          .trigger_panic = true
        };
        esp_task_wdt_reconfigure(&original_config);
        
        setupServer(); // Reiniciar servidor
      }
    } else {
      Serial.println("[OTA] Tamaño de firmware inválido");
      http.end();
      otaInProgress = false;
      setupServer();
    }
  } else {
    Serial.printf("[OTA] Error HTTP: %d\n", httpCode);
    http.end();
    otaInProgress = false;
    setupServer();
  }
}

// ---------------- Version check ----------------
void checkLatestRelease() {
  if (otaInProgress) return;
  
  HTTPClient http;
  WiFiClientSecure client;
  client.setInsecure();

  String apiUrl = "https://api.github.com/repos/Artemis-Nova/Gaia-Test/releases/latest";
  Serial.println("[GitHub] Consultando última versión...");
  http.begin(client, apiUrl);
  http.setTimeout(20000);
  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    String payload = http.getString();
    JsonDocument doc;
    deserializeJson(doc, payload);

    latestVersion = doc["tag_name"].as<String>();
    JsonArray assets = doc["assets"];
    for (JsonObject asset : assets) {
      String name = asset["name"].as<String>();
      if (name.endsWith(".bin")) {
        latestBinUrl = asset["browser_download_url"].as<String>();
        break;
      }
    }

    Serial.println("[GitHub] Última versión: " + latestVersion);
    Serial.println("[GitHub] Binario: " + latestBinUrl);
  } else {
    Serial.printf("[GitHub] Error: %d\n", httpCode);
  }
  http.end();
}

// ---------------- HTML UI ----------------
String mainPageHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Gaia</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { 
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
      background: linear-gradient(135deg, #1e5772 0%, #2a7d98 100%);
      min-height: 100vh; padding: 20px;
    }
    .container { max-width: 500px; margin: 0 auto; }
    .card { 
      background: rgba(255,255,255,0.95); 
      border-radius: 20px; padding: 25px; margin-bottom: 20px; 
      box-shadow: 0 10px 30px rgba(0,0,0,0.2);
      backdrop-filter: blur(15px);
    }
    .header {
      text-align: center; margin-bottom: 20px;
      background: linear-gradient(45deg, #67bfeb, #4b68a3);
      -webkit-background-clip: text; -webkit-text-fill-color: transparent;
    }
    .status-indicator {
      width: 12px; height: 12px; border-radius: 50%;
      display: inline-block; margin-right: 8px; animation: pulse 2s infinite;
    }
    .status-ok { background: #28a745; }
    .status-warning { background: #ffc107; }
    .status-error { background: #dc3545; }
    @keyframes pulse { 0%, 100% { opacity: 1; } 50% { opacity: 0.5; } }
    
    .info-row {
      display: flex; justify-content: space-between; align-items: center;
      padding: 12px 0; border-bottom: 1px solid #eee;
    }
    .info-row:last-child { border-bottom: none; }
    .info-label { color: #666; font-size: 0.9em; }
    .info-value { font-weight: 600; color: #333; }
    
    .btn {
      width: 100%; padding: 15px; margin: 8px 0;
      border: none; border-radius: 12px; font-size: 1em; font-weight: 600;
      cursor: pointer; transition: all 0.3s ease;
      background: linear-gradient(45deg, #667eea, #764ba2);
      color: white; text-transform: uppercase; letter-spacing: 0.5px;
    }
    .btn:hover:not(:disabled) { 
      transform: translateY(-2px); 
      box-shadow: 0 8px 25px rgba(102, 126, 234, 0.3);
    }
    .btn:disabled { 
      background: #ccc; cursor: not-allowed; transform: none; box-shadow: none;
    }
    .btn-update { background: linear-gradient(45deg, #ff6b35, #f7931e); }
    
    .progress-container {
      margin: 20px 0; padding: 20px; background: #f8f9fa; border-radius: 12px;
    }
    .progress-bar {
      width: 100%; height: 8px; background: #e9ecef; border-radius: 20px; overflow: hidden;
    }
    .progress-fill {
      height: 100%; background: linear-gradient(90deg, #00b894, #00cec9); 
      width: 0%; transition: width 0.5s ease; border-radius: 20px;
    }
    .progress-text {
      text-align: center; margin-top: 10px; font-weight: 600; color: #555;
    }
    
    .warning-box {
      background: linear-gradient(45deg, #ffeaa7, #fab1a0); 
      padding: 15px; border-radius: 10px; margin: 15px 0;
      border-left: 4px solid #e17055;
    }
    
    .spinner {
      width: 20px; height: 20px; margin-right: 10px;
      border: 2px solid transparent; border-top: 2px solid currentColor;
      border-radius: 50%; animation: spin 1s linear infinite;
      display: inline-block;
    }
    @keyframes spin { to { transform: rotate(360deg); } }
  </style>
</head>
<body>
  <div class="container">
    <div class="card">
      <div class="header">
        <h1>Gaia</h1>
        <p style="margin-top: 5px; font-size: 0.9em; opacity: 0.8;">ESP32-S3 Firmware Update System</p>
      </div>
      
      <div class="info-row">
        <span class="info-label">Firmware Actual</span>
        <span class="info-value">v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Dispositivo</span>
        <span class="info-value">ESP32-S3</span>
      </div>
      <div class="info-row">
        <span class="info-label">Estado</span>
        <span class="info-value" id="status">
          <span class="status-indicator status-ok"></span>Sistema Listo
        </span>
      </div>
      <div class="info-row">
        <span class="info-label">IP Address</span>
        <span class="info-value" id="deviceIP">Cargando...</span>
      </div>
    </div>
    
    <div class="card">
      <button id="checkBtn" class="btn" onclick="checkUpdate()">
        <span id="checkIcon"></span> Verificar Actualizaciones
      </button>
      
      <button id="updateBtn" class="btn btn-update" onclick="doUpdate()" disabled>
        <span id="updateIcon"></span> Actualizar Firmware
      </button>
      
      <div id="progressContainer" class="progress-container" style="display:none;">
        <div class="progress-bar">
          <div id="progressFill" class="progress-fill"></div>
        </div>
        <div id="progressText" class="progress-text">Preparando...</div>
      </div>
      
      <div id="warningBox" class="warning-box" style="display:none;">
        <strong>⚠️ Actualización en Proceso</strong><br>
        No desconectes el dispositivo. El proceso puede tardar hasta 5 minutos.
      </div>
    </div>
  </div>

<script>
let updateInProgress = false;

function setStatus(text, type = 'ok') {
  const statusEl = document.getElementById('status');
  const indicators = { ok: 'status-ok', warning: 'status-warning', error: 'status-error' };
  statusEl.innerHTML = `<span class="status-indicator ${indicators[type]}"></span>${text}`;
}

function setProgress(percent, text) {
  document.getElementById('progressFill').style.width = percent + '%';
  document.getElementById('progressText').textContent = text;
}

function showProgress(show = true) {
  document.getElementById('progressContainer').style.display = show ? 'block' : 'none';
}

function showWarning(show = true) {
  document.getElementById('warningBox').style.display = show ? 'block' : 'none';
}

function checkUpdate() {
  if(updateInProgress) return;
  
  document.getElementById('checkBtn').disabled = true;
  document.getElementById('checkIcon').innerHTML = '<span class="spinner"></span>';
  setStatus('Verificando GitHub...', 'warning');
  
  fetch('/check-update')
  .then(r => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  })
  .then(data => {
    if(data.updateAvailable){
      setStatus(`Nueva version disponible: ${data.latest}`, 'warning');
      document.getElementById('updateBtn').disabled = false;
    } else {
      setStatus(`Firmware actualizado (${data.current})`, 'ok');
    }
  })
  .catch(err => {
    setStatus('Error al verificar: ' + err.message, 'error');
    console.error('Error:', err);
  })
  .finally(() => {
    document.getElementById('checkBtn').disabled = false;
    document.getElementById('checkIcon').textContent = '🔍';
  });
}

function doUpdate() {
  if(updateInProgress) return;
  
  if (!confirm('¿Seguro de que quieres actualizar el firmware? El proceso tarda varios minutos.')) {
    return;
  }
  
  updateInProgress = true;
  document.getElementById('updateBtn').disabled = true;
  document.getElementById('checkBtn').disabled = true;
  document.getElementById('updateIcon').innerHTML = '<span class="spinner"></span>';
  
  setStatus('Iniciando actualización...', 'warning');
  showProgress(true);
  showWarning(true);
  setProgress(0, 'Conectando al servidor...');
  
  let progress = 0;
  const progressTimer = setInterval(() => {
    if(progress < 95) {
      progress += Math.random() * 3;
      setProgress(Math.min(progress, 95), 'Descargando y escribiendo firmware...');
    }
  }, 2000);
  
  fetch('/do-update')
  .then(r => {
    if (!r.ok) throw new Error(`Error ${r.status}: ${r.statusText}`);
    return r.text();
  })
  .then(response => {
    clearInterval(progressTimer);
    setProgress(100, 'Actualización completada');
    setStatus('Firmware actualizado. Reiniciando...', 'ok');
    
    setTimeout(() => {
      setStatus('Esperando reconexión...', 'warning');
      setProgress(0, 'Reconectando...');
      checkReconnection();
    }, 8000);
  })
  .catch(err => {
    clearInterval(progressTimer);
    setStatus('Error: ' + err.message, 'error');
    showProgress(false);
    showWarning(false);
    updateInProgress = false;
    document.getElementById('updateBtn').disabled = false;
    document.getElementById('checkBtn').disabled = false;
    document.getElementById('updateIcon').textContent = '⚡';
  });
}

function checkReconnection() {
  fetch('/', { cache: 'no-cache' })
  .then(r => {
    if(r.ok) {
      setStatus('Reconexión exitosa', 'ok');
      setProgress(100, 'Recargando interfaz...');
      setTimeout(() => window.location.reload(), 2000);
    }
  })
  .catch(() => {
    setTimeout(checkReconnection, 4000);
  });
}

// Inicialización
document.getElementById('deviceIP').textContent = window.location.hostname + ':' + window.location.port;
setTimeout(() => {
  if (!updateInProgress) checkUpdate();
}, 2000);
</script>
</body>
</html>
)rawliteral";
  return html;
}

// ---------------- WebServer ----------------
void setupServer() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", mainPageHTML());
  });

  server.on("/check-update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (otaInProgress) {
      request->send(503, "application/json", "{\"error\":\"OTA en progreso\"}");
      return;
    }
    
    checkLatestRelease();
    bool updateAvailable = (String(FIRMWARE_VERSION) != latestVersion && !latestVersion.isEmpty());
    
    String json = "{";
    json += "\"current\":\"" + String(FIRMWARE_VERSION) + "\",";
    json += "\"latest\":\"" + latestVersion + "\",";
    json += "\"updateAvailable\":" + String(updateAvailable ? "true" : "false");
    json += "}";
    request->send(200, "application/json", json);
  });

  server.on("/do-update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (otaInProgress) {
      request->send(503, "text/plain", "Actualizacion ya en progreso");
      return;
    }
    
    if (latestBinUrl != "" && String(FIRMWARE_VERSION) != latestVersion) {
      // Enviar respuesta inmediatamente
      request->send(200, "text/plain", "Actualizacion iniciada - no cierres esta pagina");
      
      // Iniciar OTA en tarea separada después de enviar respuesta
      xTaskCreate([](void* parameter) {
        delay(1000); // Dar tiempo para que se envíe la respuesta
        performFirmwareUpdate(latestBinUrl);
        vTaskDelete(NULL);
      }, "OTA_Task", 16384, NULL, 1, NULL); // Stack más grande
      
    } else {
      request->send(400, "text/plain", "No hay actualizacion disponible");
    }
  });

  server.begin();
  Serial.println("Servidor web iniciado");
}

// ---------------- Main ----------------
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n" + String("==========================================="));
  Serial.println("Gaia v" + String(FIRMWARE_VERSION));
  Serial.println("ESP32-S3 OTA - Chunk Method con Watchdog Optimizado");
  Serial.println("===========================================");
  
  Serial.printf("RAM libre: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("Flash libre: %d KB\n", ESP.getFreeSketchSpace() / 1024);
  Serial.printf("Tamaño chunks: 64 bytes (ultra-pequeños)\n");
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Conectando WiFi");
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" WiFi Conectado");
    Serial.println("IP: " + WiFi.localIP().toString());
    Serial.println("RSSI: " + String(WiFi.RSSI()) + " dBm");
  } else {
    Serial.println(" Error de conexión");
    return;
  }

  if (MDNS.begin(hostname)) {
    Serial.println("mDNS: http://" + String(hostname) + ".local");
  }

  setupServer();
  
  Serial.println("===========================================");
  Serial.println("Sistema listo: http://" + WiFi.localIP().toString());
  Serial.println("===========================================");
}

void loop() {
  delay(200);
}
