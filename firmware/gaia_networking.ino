#define FIRMWARE_VERSION "0.0.8"

/* Gaia Estación Meteorológica - Versión con Configuración WiFi
   - Sistema de configuración WiFi inicial con modo AP
   - OTA desde GitHub con chunks optimizados
   - Interfaz web responsiva y funcional
   - Gestión automática de conexiones WiFi
   - Almacenamiento de credenciales en EEPROM
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
#include <EEPROM.h>

// ---------------- Configuración de memoria ----------------
#define EEPROM_SIZE 512
#define WIFI_SSID_ADDR 0
#define WIFI_PASS_ADDR 100
#define WIFI_CONFIGURED_ADDR 200

// ---------------- Configuración del dispositivo ----------------
const char* ap_ssid = "Gaia Setup";
const char* hostname = "gaia";
const int setup_timeout = 30000; // 30 segundos para intentar conexión

AsyncWebServer server(80);

// ---------------- Variables globales ----------------
String latestVersion = "";
String latestBinUrl = "";
bool otaInProgress = false;
bool isAPMode = false;
String currentSSID = "";
String currentPassword = "";

// ---------------- Gestión de credenciales WiFi ----------------

/**
 * Guarda las credenciales WiFi en la EEPROM
 * @param ssid SSID de la red WiFi
 * @param password Contraseña de la red WiFi
 */
void saveWiFiCredentials(String ssid, String password) {
  Serial.println("[WiFi] Guardando credenciales en EEPROM...");
  
  // Limpiar las direcciones de memoria
  for (int i = 0; i < 100; i++) {
    EEPROM.write(WIFI_SSID_ADDR + i, 0);
    EEPROM.write(WIFI_PASS_ADDR + i, 0);
  }
  
  // Escribir SSID
  for (int i = 0; i < ssid.length() && i < 99; i++) {
    EEPROM.write(WIFI_SSID_ADDR + i, ssid[i]);
  }
  
  // Escribir contraseña
  for (int i = 0; i < password.length() && i < 99; i++) {
    EEPROM.write(WIFI_PASS_ADDR + i, password[i]);
  }
  
  // Marcar como configurado
  EEPROM.write(WIFI_CONFIGURED_ADDR, 1);
  EEPROM.commit();
  
  Serial.println("[WiFi] Credenciales guardadas exitosamente");
}

/**
 * Carga las credenciales WiFi desde la EEPROM
 * @return true si hay credenciales válidas guardadas
 */
bool loadWiFiCredentials() {
  Serial.println("[WiFi] Cargando credenciales desde EEPROM...");
  
  if (EEPROM.read(WIFI_CONFIGURED_ADDR) != 1) {
    Serial.println("[WiFi] No hay credenciales guardadas");
    return false;
  }
  
  currentSSID = "";
  currentPassword = "";
  
  // Leer SSID
  for (int i = 0; i < 99; i++) {
    char c = EEPROM.read(WIFI_SSID_ADDR + i);
    if (c == 0) break;
    currentSSID += c;
  }
  
  // Leer contraseña
  for (int i = 0; i < 99; i++) {
    char c = EEPROM.read(WIFI_PASS_ADDR + i);
    if (c == 0) break;
    currentPassword += c;
  }
  
  if (currentSSID.length() > 0) {
    Serial.println("[WiFi] Credenciales cargadas: " + currentSSID);
    return true;
  }
  
  Serial.println("[WiFi] Credenciales inválidas en EEPROM");
  return false;
}

/**
 * Borra las credenciales WiFi de la EEPROM
 */
void clearWiFiCredentials() {
  Serial.println("[WiFi] Borrando credenciales...");
  EEPROM.write(WIFI_CONFIGURED_ADDR, 0);
  EEPROM.commit();
}

// ---------------- Gestión de conexiones WiFi ----------------

/**
 * Intenta conectarse a una red WiFi específica
 * @param ssid SSID de la red
 * @param password Contraseña de la red
 * @return true si la conexión fue exitosa
 */
bool connectToWiFi(String ssid, String password) {
  Serial.println("[WiFi] Intentando conectar a: " + ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < setup_timeout) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Conectado exitosamente");
    Serial.println("[WiFi] IP: " + WiFi.localIP().toString());
    Serial.println("[WiFi] RSSI: " + String(WiFi.RSSI()) + " dBm");
    return true;
  } else {
    Serial.println("\n[WiFi] Error: No se pudo conectar");
    return false;
  }
}

/**
 * Inicia el modo Access Point para configuración inicial
 */
void startAccessPoint() {
  Serial.println("[AP] Iniciando modo Access Point...");
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.println("[AP] Red: " + String(ap_ssid));
  Serial.println("[AP] IP: " + IP.toString());
  Serial.println("[AP] Accede a: http://gaia.local o http://" + IP.toString());
  
  isAPMode = true;
}

// ---------------- Funciones OTA (mantenidas intactas) ----------------

/**
 * Inicia la actualización OTA del firmware
 * @param client Cliente WiFi para la descarga
 * @param contentLength Tamaño del archivo de firmware
 * @return true si la actualización fue exitosa
 */
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

/**
 * Ejecuta la actualización completa del firmware
 * @param url URL del archivo de firmware
 */
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

/**
 * Verifica la última versión disponible en GitHub
 */
void checkLatestRelease() {
  if (otaInProgress) return;
  
  // Solo verificar si estamos conectados a WiFi (no en modo AP)
  if (isAPMode || WiFi.status() != WL_CONNECTED) {
    Serial.println("[GitHub] No hay conexión a internet para verificar actualizaciones");
    return;
  }
  
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

// ---------------- Interfaz HTML ----------------

/**
 * Genera el HTML de la página principal con todas las funcionalidades
 * @return String con el código HTML completo
 */
String mainPageHTML() {
  String wifiStatus = isAPMode ? "Modo configuración (AP)" : ("Conectado a: " + currentSSID);
  String wifiStatusClass = isAPMode ? "status-warning" : "status-ok";
  String deviceIP = isAPMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Gaia - Estación Meteorológica</title>
  <style>
    * { margin: 0; padding: 0; box-sizing: border-box; }
    body { 
      font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; 
      background: linear-gradient(135deg, #1e5772 0%, #2a7d98 100%);
      min-height: 100vh; padding: 20px;
    }
    .container { max-width: 600px; margin: 0 auto; }
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
    
    .tabs {
      display: flex; border-bottom: 2px solid #eee; margin-bottom: 20px;
    }
    .tab {
      flex: 1; padding: 15px; text-align: center; cursor: pointer;
      background: none; border: none; font-size: 1em; font-weight: 600;
      color: #666; transition: all 0.3s ease;
    }
    .tab.active {
      color: #4b68a3; border-bottom: 3px solid #4b68a3;
    }
    .tab-content { display: none; }
    .tab-content.active { display: block; }
    
    .collapsible {
      background: #f8f9fa; border: none; padding: 15px; width: 100%;
      text-align: left; font-size: 1em; font-weight: 600;
      cursor: pointer; border-radius: 10px; margin-bottom: 10px;
      transition: all 0.3s ease;
    }
    .collapsible:hover { background: #e9ecef; }
    .collapsible.active { background: #dee2e6; }
    .collapsible-content {
      display: none; padding: 15px; background: white;
      border-radius: 10px; margin-bottom: 15px;
    }
    .collapsible-content.active { display: block; }
    
    .form-group {
      margin-bottom: 15px;
    }
    .form-label {
      display: block; margin-bottom: 5px; font-weight: 600; color: #333;
    }
    .form-input {
      width: 100%; padding: 12px; border: 2px solid #e9ecef;
      border-radius: 8px; font-size: 1em; transition: border-color 0.3s ease;
    }
    .form-input:focus {
      outline: none; border-color: #4b68a3;
    }
    
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
    .btn-wifi { background: linear-gradient(45deg, #00b894, #00cec9); }
    
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
    
    .success-box {
      background: linear-gradient(45deg, #a7ffeb, #64ffda); 
      padding: 15px; border-radius: 10px; margin: 15px 0;
      border-left: 4px solid #26a69a;
    }
    
    .spinner {
      width: 20px; height: 20px; margin-right: 10px;
      border: 2px solid transparent; border-top: 2px solid currentColor;
      border-radius: 50%; animation: spin 1s linear infinite;
      display: inline-block;
    }
    @keyframes spin { to { transform: rotate(360deg); } }

    .section-title {
      font-size: 1.2em; font-weight: 700; color: #333;
      margin-bottom: 15px; padding-bottom: 8px;
      border-bottom: 2px solid #4b68a3;
    }
  </style>
</head>
<body>
  <div class="container">
    <!-- Información del dispositivo -->
    <div class="card">
      <div class="header">
        <h1>Gaia</h1>
        <p style="margin-top: 5px; font-size: 0.9em; opacity: 0.8;">Estacion Meteorologica ESP32-S3</p>
      </div>
    </div>
    
    <!-- Pestañas -->
    <div class="card">
      <div class="tabs">
        <button class="tab active" onclick="showTab('panel')">Panel</button>
        <button class="tab" onclick="showTab('config')">Configuracion</button>
      </div>
      
      <!-- Contenido pestaña Panel -->
      <div id="panel" class="tab-content active">
        <div class="section-title">Panel Principal</div>
        <p>Bienvenido al panel de control de Gaia. Desde aqui puedes acceder a todas las funciones de configuracion y monitoreo del sistema.</p>
      </div>
      
      <!-- Contenido pestaña Configuración -->
      <div id="config" class="tab-content">
        <!-- Estado del Sistema -->
        <div class="section-title">Estado del Sistema</div>
        <button class="collapsible" id="systemCollapsible">Informacion del Dispositivo</button>
        <div class="collapsible-content" id="systemContent">
          <div class="info-row">
            <span class="info-label">Firmware</span>
            <span class="info-value">v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</span>
          </div>
          <div class="info-row">
            <span class="info-label">Dispositivo</span>
            <span class="info-value">ESP32-S3</span>
          </div>
          <div class="info-row">
            <span class="info-label">Estado WiFi</span>
            <span class="info-value" id="wifiStatus">
              <span class="status-indicator )rawliteral" + wifiStatusClass + R"rawliteral("></span>)rawliteral" + wifiStatus + R"rawliteral(
            </span>
          </div>
          <div class="info-row">
            <span class="info-label">Direccion IP</span>
            <span class="info-value" id="deviceIP">)rawliteral" + deviceIP + R"rawliteral(</span>
          </div>
          <div class="info-row">
            <span class="info-label">RAM Libre</span>
            <span class="info-value" id="freeHeap">Cargando...</span>
          </div>
          <div class="info-row">
            <span class="info-label">Flash Libre</span>
            <span class="info-value" id="freeSketch">Cargando...</span>
          </div>
          <div class="info-row">
            <span class="info-label">Tiempo Activo</span>
            <span class="info-value" id="uptime">Cargando...</span>
          </div>
        </div>
        
        <!-- Configuración WiFi -->
        <div class="section-title">Configuracion WiFi</div>
        <button class="collapsible" id="wifiCollapsible">Configuracion de Red WiFi</button>
        <div class="collapsible-content" id="wifiContent">
          <div class="info-row">
            <span class="info-label">Red Actual</span>
            <span class="info-value" id="currentNetwork">)rawliteral" + (isAPMode ? "Ninguna (Modo AP)" : currentSSID) + R"rawliteral(</span>
          </div>
          
          <div class="form-group">
            <label class="form-label">Nuevo SSID</label>
            <input type="text" class="form-input" id="newSSID" placeholder="Nombre de la red WiFi">
          </div>
          
          <div class="form-group">
            <label class="form-label">Nueva Contraseña</label>
            <input type="password" class="form-input" id="newPassword" placeholder="Contraseña de la red WiFi">
          </div>
          
          <button class="btn btn-wifi" onclick="updateWiFi()" id="wifiUpdateBtn">
            <span id="wifiUpdateIcon">Conectar a Nueva Red</span>
          </button>
          
          <div id="wifiMessage" style="display:none; margin-top: 15px;"></div>
        </div>
        
        <!-- Configuración de versión -->
        <div class="section-title" style="margin-top: 30px;">Actualizacion de Firmware</div>
        <button class="collapsible" id="versionCollapsible">Gestion de Versiones</button>
        <div class="collapsible-content" id="versionContent">
          <div class="info-row">
            <span class="info-label">Version Actual</span>
            <span class="info-value">v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</span>
          </div>
          <div class="info-row">
            <span class="info-label">Ultima Version</span>
            <span class="info-value" id="latestVersion">Verificar actualizaciones</span>
          </div>
          
          <button id="checkBtn" class="btn" onclick="checkUpdate()">
            <span id="checkIcon">Buscar Actualizaciones</span>
          </button>
          
          <button id="updateBtn" class="btn btn-update" onclick="doUpdate()" disabled>
            <span id="updateIcon">Actualizar Firmware</span>
          </button>
          
          <div id="progressContainer" class="progress-container" style="display:none;">
            <div class="progress-bar">
              <div id="progressFill" class="progress-fill"></div>
            </div>
            <div id="progressText" class="progress-text">Preparando...</div>
          </div>
          
          <div id="warningBox" class="warning-box" style="display:none;">
            <strong>Actualizacion en Proceso</strong><br>
            No desconectes el dispositivo. El proceso puede tardar hasta 5 minutos.
          </div>
        </div>
      </div>
    </div>
  </div>

<script>
let updateInProgress = false;
let wifiUpdateInProgress = false;

// Gestión de pestañas
function showTab(tabName) {
  // Ocultar todos los contenidos
  const contents = document.querySelectorAll('.tab-content');
  contents.forEach(content => content.classList.remove('active'));
  
  // Desactivar todas las pestañas
  const tabs = document.querySelectorAll('.tab');
  tabs.forEach(tab => tab.classList.remove('active'));
  
  // Mostrar contenido seleccionado
  document.getElementById(tabName).classList.add('active');
  
  // Activar pestaña seleccionada
  event.target.classList.add('active');
}

// Gestión de contenido colapsable
document.addEventListener('DOMContentLoaded', function() {
  const collapsibles = document.querySelectorAll('.collapsible');
  collapsibles.forEach(function(collapsible) {
    collapsible.addEventListener('click', function() {
      this.classList.toggle('active');
      const content = this.nextElementSibling;
      content.classList.toggle('active');
    });
  });
  
  // Actualizar información del sistema
  updateSystemInfo();
});

function updateSystemInfo() {
  fetch('/system-info')
  .then(r => r.json())
  .then(data => {
    document.getElementById('freeHeap').textContent = data.freeHeap;
    document.getElementById('freeSketch').textContent = data.freeSketch;
    document.getElementById('uptime').textContent = data.uptime;
  })
  .catch(err => console.error('Error:', err));
}

// Funciones WiFi
function updateWiFi() {
  if (wifiUpdateInProgress || updateInProgress) return;
  
  const ssid = document.getElementById('newSSID').value.trim();
  const password = document.getElementById('newPassword').value.trim();
  
  if (!ssid) {
    showWiFiMessage('Por favor ingresa un SSID válido', 'error');
    return;
  }
  
  if (!confirm('Conectar Gaia a la red "' + ssid + '"? El dispositivo se reiniciara para aplicar los cambios.')) {
    return;
  }
  
  wifiUpdateInProgress = true;
  document.getElementById('wifiUpdateBtn').disabled = true;
  document.getElementById('wifiUpdateIcon').innerHTML = '<span class="spinner"></span>Conectando...';
  
  showWiFiMessage('Conectando a la red WiFi...', 'warning');
  
  const data = {
    ssid: ssid,
    password: password
  };
  
  fetch('/update-wifi', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(data)
  })
  .then(r => {
    if (!r.ok) throw new Error(`Error ${r.status}: ${r.statusText}`);
    return r.text();
  })
  .then(response => {
    showWiFiMessage('Configuracion guardada. Gaia se conectara a "' + ssid + '" y se reiniciara...', 'success');
    
    setTimeout(() => {
      showWiFiMessage('Esperando reconexion del dispositivo...', 'warning');
      setTimeout(checkReconnection, 10000);
    }, 3000);
  })
  .catch(err => {
    showWiFiMessage('Error: ' + err.message, 'error');
    wifiUpdateInProgress = false;
    document.getElementById('wifiUpdateBtn').disabled = false;
    document.getElementById('wifiUpdateIcon').textContent = 'Conectar a Nueva Red';
  });
}

function showWiFiMessage(message, type) {
  const messageEl = document.getElementById('wifiMessage');
  const typeClasses = {
    success: 'success-box',
    warning: 'warning-box',
    error: 'warning-box'
  };
  
  messageEl.className = typeClasses[type] || 'warning-box';
  messageEl.textContent = message;
  messageEl.style.display = 'block';
}

// Funciones de estado y utilidades
function setStatus(text, type = 'ok') {
  const statusEl = document.getElementById('wifiStatus');
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

// Funciones de actualización OTA (mantenidas intactas)
function checkUpdate() {
  if(updateInProgress || wifiUpdateInProgress) return;
  
  document.getElementById('checkBtn').disabled = true;
  document.getElementById('checkIcon').innerHTML = '<span class="spinner"></span>Verificando...';
  
  fetch('/check-update')
  .then(r => {
    if (!r.ok) throw new Error(`HTTP ${r.status}`);
    return r.json();
  })
  .then(data => {
    document.getElementById('latestVersion').textContent = data.latest || 'Error al verificar';
    
    if(data.updateAvailable){
      document.getElementById('updateBtn').disabled = false;
    } else {
      // No hay actualización disponible
    }
  })
  .catch(err => {
    document.getElementById('latestVersion').textContent = 'Error: ' + err.message;
    console.error('Error:', err);
  })
  .finally(() => {
    document.getElementById('checkBtn').disabled = false;
    document.getElementById('checkIcon').textContent = 'Buscar Actualizaciones';
  });
}

function doUpdate() {
  if(updateInProgress || wifiUpdateInProgress) return;
  
  if (!confirm('Seguro de que quieres actualizar el firmware? El proceso tarda varios minutos.')) {
    return;
  }
  
  updateInProgress = true;
  document.getElementById('updateBtn').disabled = true;
  document.getElementById('checkBtn').disabled = true;
  document.getElementById('updateIcon').innerHTML = '<span class="spinner"></span>Actualizando...';
  
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
    setProgress(100, 'Actualizacion completada');
    
    setTimeout(() => {
      setProgress(0, 'Reconectando...');
      checkReconnection();
    }, 8000);
  })
  .catch(err => {
    clearInterval(progressTimer);
    showProgress(false);
    showWarning(false);
    updateInProgress = false;
    document.getElementById('updateBtn').disabled = false;
    document.getElementById('checkBtn').disabled = false;
    document.getElementById('updateIcon').textContent = 'Actualizar Firmware';
    alert('Error: ' + err.message);
  });
}

function checkReconnection() {
  fetch('/', { cache: 'no-cache' })
  .then(r => {
    if(r.ok) {
      setProgress(100, 'Recargando interfaz...');
      setTimeout(() => window.location.reload(), 2000);
    }
  })
  .catch(() => {
    setTimeout(checkReconnection, 4000);
  });
}

// Actualizar información del sistema cada 30 segundos
setInterval(updateSystemInfo, 30000);
</script>
</body>
</html>
)rawliteral";
  return html;
}

// ---------------- Configuración del servidor web ----------------

/**
 * Configura todos los endpoints del servidor web
 */
void setupServer() {
  // Página principal
  server.on("/", HTTP_GET, [](AsyncWebServerRequest* request) {
    request->send(200, "text/html", mainPageHTML());
  });

  // Información del sistema
  server.on("/system-info", HTTP_GET, [](AsyncWebServerRequest* request) {
    String json = "{";
    json += "\"freeHeap\":\"" + String(ESP.getFreeHeap() / 1024) + " KB\",";
    json += "\"freeSketch\":\"" + String(ESP.getFreeSketchSpace() / 1024) + " KB\",";
    json += "\"uptime\":\"" + String(millis() / 1000 / 60) + " minutos\"";
    json += "}";
    request->send(200, "application/json", json);
  });

  // Actualizar configuración WiFi
  server.on("/update-wifi", HTTP_POST, [](AsyncWebServerRequest* request) {}, NULL,
    [](AsyncWebServerRequest* request, uint8_t* data, size_t len, size_t index, size_t total) {
      if (otaInProgress) {
        request->send(503, "text/plain", "OTA en progreso");
        return;
      }
      
      String body = String((char*)data).substring(0, len);
      JsonDocument doc;
      deserializeJson(doc, body);
      
      String newSSID = doc["ssid"].as<String>();
      String newPassword = doc["password"].as<String>();
      
      if (newSSID.length() > 0) {
        Serial.println("[WiFi] Nueva configuración recibida: " + newSSID);
        
        // Guardar credenciales
        saveWiFiCredentials(newSSID, newPassword);
        
        // Enviar respuesta antes del reinicio
        request->send(200, "text/plain", "Configuración guardada, reiniciando...");
        
        // Reiniciar después de un delay
        delay(2000);
        ESP.restart();
      } else {
        request->send(400, "text/plain", "SSID inválido");
      }
    });

  // Verificar actualizaciones (mantenido intacto)
  server.on("/check-update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (otaInProgress) {
      request->send(503, "application/json", "{\"error\":\"OTA en progreso\"}");
      return;
    }
    
    if (isAPMode) {
      request->send(503, "application/json", "{\"error\":\"Sin conexión a internet\"}");
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

  // Ejecutar actualización OTA (mantenido intacto)
  server.on("/do-update", HTTP_GET, [](AsyncWebServerRequest* request) {
    if (otaInProgress) {
      request->send(503, "text/plain", "Actualización ya en progreso");
      return;
    }
    
    if (isAPMode) {
      request->send(503, "text/plain", "Sin conexión a internet para actualizar");
      return;
    }
    
    if (latestBinUrl != "" && String(FIRMWARE_VERSION) != latestVersion) {
      // Enviar respuesta inmediatamente
      request->send(200, "text/plain", "Actualización iniciada - no cierres esta página");
      
      // Iniciar OTA en tarea separada después de enviar respuesta
      xTaskCreate([](void* parameter) {
        delay(1000); // Dar tiempo para que se envíe la respuesta
        performFirmwareUpdate(latestBinUrl);
        vTaskDelete(NULL);
      }, "OTA_Task", 16384, NULL, 1, NULL); // Stack más grande
      
    } else {
      request->send(400, "text/plain", "No hay actualización disponible");
    }
  });

  server.begin();
  Serial.println("[Web] Servidor web iniciado");
}

// ---------------- Configuración principal ----------------

void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n===========================================");
  Serial.println("Gaia v" + String(FIRMWARE_VERSION));
  Serial.println("ESP32-S3 Estación Meteorológica");
  Serial.println("Sistema de configuración WiFi y OTA");
  Serial.println("===========================================");
  
  // Inicializar EEPROM
  EEPROM.begin(EEPROM_SIZE);
  
  // Información del sistema
  Serial.printf("[Sys] RAM libre: %d KB\n", ESP.getFreeHeap() / 1024);
  Serial.printf("[Sys] Flash libre: %d KB\n", ESP.getFreeSketchSpace() / 1024);
  
  // Intentar cargar credenciales WiFi guardadas
  bool hasCredentials = loadWiFiCredentials();
  
  if (hasCredentials) {
    // Intentar conectarse a la red guardada
    Serial.println("[Setup] Intentando conectar con credenciales guardadas...");
    
    if (connectToWiFi(currentSSID, currentPassword)) {
      // Conexión exitosa
      isAPMode = false;
      Serial.println("[Setup] Conectado a WiFi guardado");
    } else {
      // Falló la conexión, activar modo AP
      Serial.println("[Setup] Falló conexión WiFi, iniciando modo AP");
      startAccessPoint();
    }
  } else {
    // No hay credenciales, iniciar en modo AP
    Serial.println("[Setup] No hay credenciales WiFi, iniciando modo AP");
    startAccessPoint();
  }
  
  // Configurar mDNS solo si no estamos en modo AP
  if (!isAPMode) {
    // Delay para asegurar que WiFi esté completamente inicializado
    delay(1000);
    if (MDNS.begin(hostname)) {
      Serial.println("[mDNS] Servicio iniciado: http://" + String(hostname) + ".local");
      MDNS.addService("http", "tcp", 80);
    }
  }
  
  // Configurar servidor web
  setupServer();
  
  Serial.println("===========================================");
  if (isAPMode) {
    Serial.println("[Setup] Modo AP activo - http://" + WiFi.softAPIP().toString());
    Serial.println("[Setup] También disponible en: http://" + String(hostname) + ".local");
  } else {
    Serial.println("[Setup] Conectado a WiFi: http://" + WiFi.localIP().toString());
    Serial.println("[Setup] También disponible en: http://" + String(hostname) + ".local");
  }
  Serial.println("===========================================");
}

void loop() {
  // Verificar estado de la conexión WiFi si no estamos en modo AP
  if (!isAPMode && WiFi.status() != WL_CONNECTED) {
    Serial.println("[Loop] Conexión WiFi perdida, reintentando...");
    
    // Intentar reconectar
    if (!connectToWiFi(currentSSID, currentPassword)) {
      Serial.println("[Loop] Reconexión fallida, cambiando a modo AP");
      startAccessPoint();
      
      // Parar mDNS antes de cambiar a modo AP
      MDNS.end();
      
      // Reiniciar servidor para el nuevo modo
      server.end();
      delay(1000);
      setupServer();
    } else {
      // Reconexión exitosa, reiniciar mDNS si es necesario
      isAPMode = false;
      Serial.println("[Loop] Reconectado a WiFi, reiniciando mDNS...");
      MDNS.end();
      delay(500); // Mayor delay para asegurar limpieza
      if (MDNS.begin(hostname)) {
        MDNS.addService("http", "tcp", 80);
        MDNS.addService("_http", "_tcp", 80);
        MDNS.addServiceTxt("http", "tcp", "board", "esp32s3");
        MDNS.addServiceTxt("http", "tcp", "device", "gaia");
        Serial.println("[Loop] mDNS reiniciado tras reconexión");
      }
    }
  }
  
  delay(5000); // Verificar cada 5 segundos
}
