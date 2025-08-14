/* Pruebas de conectividad WiFi para Gaia V0.0.2
  - V0.0.2: Reorganización de sección de configuración y preparación para actualizaciones OTA.
*/
#define FIRMWARE_VERSION "0.0.2"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Preferences.h>
#include "time.h"

Preferences preferences;
AsyncWebServer server(80);

String savedSSID;
String savedPASS;

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 0;      // Ajustar según zona horaria
const int   daylightOffset_sec = 0; // Ajustar según horario de verano

// ---------- Funciones de red ----------
void startAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP("Gaia-Setup");
  Serial.println("AP activo. Conecta y visita http://gaia.local");
}

void connectToWiFi() {
  if (savedSSID.length() == 0) return;

  Serial.printf("Conectando a %s...\n", savedSSID.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(savedSSID.c_str(), savedPASS.c_str());

  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 10000) {
    delay(200);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConectado a WiFi");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  } else {
    Serial.println("\nNo se pudo conectar. Volviendo a AP.");
    startAP();
  }
}

// ---------- HTML principal ----------
String mainPageHTML() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<title>Panel Gaia</title>
<style>
  body {
    font-family: Arial, sans-serif;
    margin: 0; padding: 0;
    background: #f4f6f8;
    color: #333;
  }
  header {
    background: #4CAF50;
    color: white;
    padding: 10px 20px;
    text-align: center;
  }
  header h1 {
    margin: 0;
    font-size: 1.8em;
  }
  header small {
    display: block;
    font-size: 0.9em;
    opacity: 0.8;
  }
  nav {
    display: flex;
    background: #333;
  }
  .tab {
    flex: 1;
    text-align: center;
    padding: 12px;
    cursor: pointer;
    color: white;
    transition: background 0.3s;
  }
  .tab:hover { background: #444; }
  .tab.active { background: #4CAF50; }
  .content {
    display: none;
    padding: 20px;
  }
  .content.active { display: block; }
  h2 { color: #4CAF50; }
  
  /* Firmware section styling */
  .firmware-section {
    background: white;
    padding: 15px;
    border-radius: 8px;
    box-shadow: 0 2px 5px rgba(0,0,0,0.1);
    margin-bottom: 15px;
  }
  .firmware-section button {
    background: #4CAF50;
    color: white;
    border: none;
    padding: 8px 12px;
    margin-top: 10px;
    cursor: pointer;
    border-radius: 4px;
  }
  .firmware-section button:hover {
    background: #45a049;
  }

  /* WiFi dropdown styling */
  .dropdown {
    background: white;
    border-radius: 8px;
    box-shadow: 0 2px 5px rgba(0,0,0,0.1);
    overflow: hidden;
  }
  .dropdown-header {
    background: #eee;
    padding: 10px;
    cursor: pointer;
    font-weight: bold;
  }
  .dropdown-content {
    display: none;
    padding: 15px;
  }
  form {
    max-width: 300px;
  }
  input[type=text], input[type=password] {
    width: 100%;
    padding: 8px;
    margin: 8px 0;
    box-sizing: border-box;
  }
  input[type=submit] {
    background: #4CAF50;
    color: white;
    border: none;
    padding: 10px 15px;
    cursor: pointer;
    width: 100%;
    border-radius: 4px;
    font-size: 1em;
  }
  input[type=submit]:hover {
    background: #45a049;
  }
  #clock {
    font-size: 2.5em;
    font-weight: bold;
    margin-top: 10px;
  }
</style>
<script>
function showTab(tabId) {
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  document.querySelectorAll('.content').forEach(c => c.classList.remove('active'));
  document.getElementById('tab-'+tabId).classList.add('active');
  document.getElementById('content-'+tabId).classList.add('active');
}
function updateClock(){
  fetch('/time').then(r => r.text()).then(t => {
    document.getElementById('clock').innerText = t;
  });
}
function toggleDropdown(id) {
  var content = document.getElementById(id);
  content.style.display = (content.style.display === "block") ? "none" : "block";
}
setInterval(updateClock, 1000);
window.onload = function() { updateClock(); showTab('hora'); }
</script>
</head>
<body>

<header>
  <h1>Panel Gaia</h1>
  <small>Firmware v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</small>
</header>

<nav>
  <div id="tab-hora" class="tab" onclick="showTab('hora')">Hora actual</div>
  <div id="tab-config" class="tab" onclick="showTab('config')">Configuración</div>
</nav>

<div id="content-hora" class="content">
  <h2>Hora actual</h2>
  <div id="clock">--:--:--</div>
</div>

<div id="content-config" class="content">
  <!-- Firmware Section -->
  <div class="firmware-section">
    <h2>Firmware</h2>
    <p>Versión actual: <strong>v)rawliteral" + String(FIRMWARE_VERSION) + R"rawliteral(</strong></p>
    <button onclick="alert('Función de búsqueda de actualizaciones próximamente')">Buscar actualizaciones</button>
    <button onclick="alert('Función de actualización próximamente')">Actualizar firmware</button>
  </div>

  <!-- WiFi Dropdown Section -->
  <div class="dropdown">
    <div class="dropdown-header" onclick="toggleDropdown('wifi-settings')">Configuración WiFi ▼</div>
    <div id="wifi-settings" class="dropdown-content">
      <form action='/save' method='POST'>
        <label>SSID:</label>
        <input type='text' name='ssid' placeholder='Nombre de la red'>
        <label>Password:</label>
        <input type='password' name='pass' placeholder='Contraseña'>
        <input type='submit' value='Aplicar'>
      </form>
    </div>
  </div>
</div>

</body>
</html>
)rawliteral";
  return html;
}

// ---------- Setup ----------
void setup() {
  Serial.begin(115200);
  preferences.begin("wifi", false);

  savedSSID = preferences.getString("ssid", "");
  savedPASS = preferences.getString("pass", "");

  if (savedSSID.length() > 0) {
    connectToWiFi();
    if (WiFi.status() != WL_CONNECTED) {
      startAP();
    }
  } else {
    startAP();
  }

  if (MDNS.begin("gaia")) {
    Serial.println("mDNS iniciado: http://gaia.local");
  }

  // Página principal con pestañas
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", mainPageHTML());
  });

  // Endpoint para hora
  server.on("/time", HTTP_GET, [](AsyncWebServerRequest *request) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
      request->send(200, "text/plain", "--:--:--");
      return;
    }
    char buffer[9];
    strftime(buffer, sizeof(buffer), "%H:%M:%S", &timeinfo);
    request->send(200, "text/plain", buffer);
  });

  // Guardar datos WiFi
  server.on("/save", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("ssid", true) && request->hasParam("pass", true)) {
      savedSSID = request->getParam("ssid", true)->value();
      savedPASS = request->getParam("pass", true)->value();

      preferences.putString("ssid", savedSSID);
      preferences.putString("pass", savedPASS);

      request->send(200, "text/html", "<h1>Datos guardados. Intentando conectar...</h1><p>La placa se reiniciará.</p>");
      delay(1000);
      ESP.restart();
    } else {
      request->send(400, "text/plain", "Faltan parámetros");
    }
  });

  server.begin();
}

void loop() {
  // Nada aquí, AsyncWebServer maneja todo
}
