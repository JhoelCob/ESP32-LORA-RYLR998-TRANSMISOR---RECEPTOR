#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include <FS.h> // Para el sistema de archivos (SPIFFS o LittleFS)
#include <NTPClient.h> // Para obtener la hora por NTP
#include <WiFiUdp.h>   // Necesario para NTPClient

// === Configuración de Pines y Módulos ===
#define LORA_RX 16
#define LORA_TX 17
#define SD_CS 5
#define SD_MOSI 23
#define SD_MISO 19
#define SD_SCK 18
#define LED_PIN 2 // Pin para el LED de indicación de rango

// === Constantes y Configuración de LoRa ===
const struct {
  int address = 2;
  int networkId = 18;
} loraConfig;

// === Configuración NTP ===
const long utcOffsetInSeconds = -5 * 3600;
// Offset para Lima, Perú (-5 horas)
const char* ntpServer = "pool.ntp.org";

const unsigned long SD_SAVE_INTERVAL = 60000;
// Guardar cada 60 segundos
const int MAX_HISTORY = 50; // Máximo de registros en el historial en RAM

// === Objetos Globales ===
HardwareSerial LoRaSerial(2);
WebServer server(80);
Preferences preferences;
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, ntpServer, utcOffsetInSeconds);

// === Estructuras de Datos ===
struct SensorData {
  float temperature = 0;
  float humidity = 0;
  float lux = 0;
  int soilMoisture = 0;
  unsigned long lastUpdate = 0;
  // Timestamp millis del ESP32
  bool dataValid = false;
} sensorData;

struct DataPoint {
  unsigned long timestamp;
  // Timestamp millis del ESP32 para el historial interno
  float temperature, humidity, lux;
  int soilMoisture;
} dataHistory[MAX_HISTORY];

struct SensorRanges {
  float tempMin = -40; float tempMax = 80;
  float humMin = 0; float humMax = 100;
  float luxMin = 0; float luxMax = 100000;
  int soilMin = 0; int soilMax = 100;
} sensorRanges;

// === Variables de Estado ===
int historyIndex = 0, historyCount = 0;
String ssid = "";
String password = "";
bool wifiConnected = false;
bool sdCardAvailable = false;
unsigned long lastSdSave = 0;
String currentLogFile = "";
// Nombre del archivo de log actual en SD
bool timeSynchronized = false;
// Bandera para saber si la hora está sincronizada
unsigned long ledOnStartTime = 0;
const long LED_ON_DURATION = 15000; // 15 segundos

// === Prototipos de Funciones ===
void setup();
void loop();
void initializeSD();
bool initializeLogFile(); // Modificado para retornar bool
void saveToSD();
String getFormattedDateTime();
void initializeLoRa();
void initializeWiFi();
void loadWiFiCredentials();
void requestWiFiCredentials();
bool connectToWiFi();
void checkWiFiConnection();
void initializeMDNS();
void initializeWebServer();
void initializeNTP();
void processLoRaData();
void parseAndStoreSensorData(String payload);
void addToHistory();
void printReceivedData();
void loadSensorRanges();
void saveSensorRanges();
void checkSensorRanges(); // Nueva función
void handleRoot();
void handleCSS();
void handleJS();
void handleAPIData();
void handleAPIHistory();
void handleSDInfo();
void handleDownloadData();
void handleAPI_GetRanges();   // Nueva función para obtener rangos
void handleAPI_SetRanges();   // Nueva función para establecer rangos
void handleNotFound();

// === Implementación de Funciones ===

void setup() {
  Serial.begin(115200);
  Serial.println("=== ESP32 RECEPTOR LORA + WIFI + WEB + SD ===");

  pinMode(LED_PIN, OUTPUT); // Configurar el pin del LED como salida
  digitalWrite(LED_PIN, LOW); // Asegurarse de que el LED esté apagado al inicio

  preferences.begin("sensor-config", false); // Usar un namespace para las preferencias
  loadSensorRanges(); // Cargar los rangos al inicio

  initializeSD();
  initializeLoRa();
  initializeWiFi();
  // WiFi se conecta aquí

  if (wifiConnected) {
    initializeNTP();
    // Inicia NTP solo si WiFi está conectado
    initializeWebServer();
    initializeMDNS();
  } else {
    Serial.println("WiFi no conectado. Algunas funciones estarán limitadas.");
  }

  Serial.println("Sistema inicializado");
  if (wifiConnected) {
    Serial.println("IP: " + WiFi.localIP().toString());
  }
  if (sdCardAvailable) {
    Serial.println("SD: Lista para almacenamiento");
  }
}

void loop() {
  if (wifiConnected) {
    server.handleClient();
    checkWiFiConnection();

    // Actualizar hora NTP. Si no está sincronizada, intenta de nuevo.
    if (!timeSynchronized) {
      if (timeClient.update()) {
        timeSynchronized = true;
        Serial.println("Hora NTP sincronizada: " + timeClient.getFormattedTime());
        // Intentar inicializar el archivo de log si NTP se acaba de sincronizar
        if (sdCardAvailable) { // Asegurarse que SD esté disponible antes de intentar crear archivo
            if (!initializeLogFile()) {
                Serial.println("ADVERTENCIA: No se pudo crear el archivo de log con fecha NTP, intentando sin fecha.");
            }
        }
      } else {
        Serial.println("NTP: Fallo al actualizar o ya actualizado.");
      }
    }
  }

  processLoRaData();
  checkSensorRanges(); // Verificar los rangos en cada ciclo del loop

  // Control del LED
  if (ledOnStartTime > 0 && millis() - ledOnStartTime >= LED_ON_DURATION) {
    digitalWrite(LED_PIN, LOW); // Apagar el LED después de 15 segundos
    ledOnStartTime = 0; // Resetear el temporizador
    Serial.println("LED apagado.");
  }

  // Guardar datos en SD periódicamente
  // Solo guarda si la SD está disponible, hay datos válidos y la hora está sincronizada
  if (sdCardAvailable && sensorData.dataValid && timeSynchronized && millis() - lastSdSave >= SD_SAVE_INTERVAL) {
    saveToSD();
    lastSdSave = millis();
  }

  delay(100);
}

void initializeSD() {
  Serial.print("Inicializando SD Card...");
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  // Configurar SPI para SD

  if (!SD.begin(SD_CS)) {
    Serial.println(" ✗ Error al inicializar SD. Revise conexiones y alimentación.");
    sdCardAvailable = false;
    return;
  }

  uint8_t cardType = SD.cardType();
  if (cardType == CARD_NONE) {
    Serial.println(" ✗ No hay tarjeta SD.");
    sdCardAvailable = false;
    return;
  }

  sdCardAvailable = true;
  Serial.println(" ✓ SD inicializada");

  // Mostrar información de la tarjeta
  Serial.print("Tipo: ");
  if (cardType == CARD_MMC) Serial.println("MMC");
  else if (cardType == CARD_SD) Serial.println("SDSC");
  else if (cardType == CARD_SDHC) Serial.println("SDHC");
  else Serial.println("Desconocido");
  Serial.println("Tamaño: " + String(SD.cardSize() / (1024 * 1024 * 1024)) + " GB");
  // Crear directorio si no existe
  if (!SD.exists("/data")) {
    SD.mkdir("/data");
    Serial.println("Directorio /data creado");
  }
  // initializeLogFile() ya no se llama aquí, solo cuando NTP sincroniza
}

// Modificado para retornar un bool
bool initializeLogFile() {
  if (!timeSynchronized) {
    Serial.println("Error: Hora no sincronizada para nombrar archivo de log.");
    return false;
  }
  
  // Generar nombre de archivo basado en la fecha real (AAAA-MM-DD)
  // getFormattedTime() devuelve "HH:MM:SS" si se usa sin getEpochTime()
  // Necesitamos la fecha completa, así que usamos getEpochTime() y gmtime
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  
  char dateBuffer[11]; //YYYY-MM-DD + null terminator
  sprintf(dateBuffer, "%04d-%02d-%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
  String newLogFile = "/data/sensors_" + String(dateBuffer) + ".csv";

  // Solo re-crea si el archivo es nuevo para el día
  if (currentLogFile != newLogFile) {
    currentLogFile = newLogFile;
    if (!SD.exists(currentLogFile)) {
      File file = SD.open(currentLogFile, FILE_WRITE);
      if (file) {
        // Orden de columnas: timestamp,temperature,humidity,soil_moisture,lux
        file.println("timestamp,temperature,humidity,soil_moisture,lux");
        file.close();
        Serial.println("Archivo de log creado/actualizado: " + currentLogFile);
        return true;
      } else {
        Serial.println("Error al abrir/crear archivo de log: " + currentLogFile + ". Verifique la tarjeta SD.");
        return false;
      }
    } else {
      Serial.println("Archivo de log existente para hoy: " + currentLogFile);
      return true; // El archivo ya existe, es válido
    }
  }
  return true;
  // Si el archivo actual es el mismo, no hacemos nada
}

void saveToSD() {
  if (!sdCardAvailable || !sensorData.dataValid || !timeSynchronized) {
    Serial.println("No se puede guardar en SD: SD no disponible, datos inválidos o hora no sincronizada.");
    return;
  }

  // Asegurarse de que el nombre del archivo se haya inicializado correctamente y exista
  // Se llama a initializeLogFile() para asegurar que currentLogFile esté al día y exista.
  if (!initializeLogFile()) { // Llama y verifica si se pudo preparar el archivo
      Serial.println("No se pudo preparar el archivo de log SD para guardar (fallo en initializeLogFile()).");
      return;
  }

  File file = SD.open(currentLogFile, FILE_APPEND);
  if (!file) {
    Serial.println("Error al abrir archivo para escritura: " + currentLogFile);
    return;
  }

  // Formato CSV: hora_fecha, temperatura, humedad, humedad_suelo, nivel_luz
  String dataLine = getFormattedDateTime() + "," + // Hora y Fecha real
                   String(sensorData.temperature, 2) + "," + // Temperatura
                   String(sensorData.humidity, 2) + "," + // Humedad
                   String(sensorData.soilMoisture) + "," + // Humedad del suelo
                   String(sensorData.lux, 1);
  // Nivel de luz

  file.println(dataLine);
  file.close();

  Serial.println("Datos guardados en SD: " + String(sensorData.temperature, 1) + "°C at " + getFormattedDateTime());
}

String getFormattedDateTime() {
  if (!timeSynchronized) {
    return "TIME_NOT_SET";
  }
  // Retorna la fecha y hora en formato YYYY-MM-DD HH:MM:SS
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 
  
  char buffer[20];
  // Formato YYYY-MM-DD HH:MM:SS
  sprintf(buffer, "%04d-%02d-%02d %02d:%02d:%02d", 
          ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
          ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
  return String(buffer);
}


void initializeLoRa() {
  LoRaSerial.begin(115200, SERIAL_8N1, LORA_RX, LORA_TX);
  delay(1000);
  // Dar tiempo al módulo LoRa para inicializar

  String commands[] = {
    "AT+RESET",
    "AT+ADDRESS=" + String(loraConfig.address),
    "AT+NETWORKID=" + String(loraConfig.networkId),
    "AT+PARAMETER=12,4,1,7" // Spreading Factor, Bandwidth, Coding Rate, CRC
  };
  for (int i = 0; i < 4; i++) {
    LoRaSerial.println(commands[i]);
    delay(i == 0 ? 2000 : 500); // Más tiempo para el RESET
    while (LoRaSerial.available()) { // Limpiar buffer serial LoRa
      LoRaSerial.read();
    }
  }

  Serial.println("LoRa configurado como receptor");
}

void initializeWiFi() {
  loadWiFiCredentials();
  if (ssid.length() > 0) {
    Serial.println("Intentando conectar con credenciales guardadas...");
    connectToWiFi();
  }

  if (!wifiConnected) {
    Serial.println("Configuración WiFi necesaria");
    requestWiFiCredentials();
  }
}

void loadWiFiCredentials() {
  ssid = preferences.getString("ssid", "");
  password = preferences.getString("password", "");

  if (ssid.length() > 0) {
    Serial.println("Credenciales WiFi encontradas");
  }
}

void requestWiFiCredentials() {
  Serial.println("\n=== CONFIGURACIÓN WIFI ===");
  Serial.print("SSID: ");
  while (!Serial.available()) delay(100);
  ssid = Serial.readStringUntil('\n');
  ssid.trim();

  Serial.print("Password: ");
  while (!Serial.available()) delay(100);
  password = Serial.readStringUntil('\n');
  password.trim();

  if (connectToWiFi()) {
    preferences.putString("ssid", ssid);
    preferences.putString("password", password);
    Serial.println("Credenciales guardadas");
  } else {
    Serial.println("No se pudo conectar. Reinicie el ESP32 o reingrese las credenciales.");
    // En un entorno de producción, podrías implementar un portal cautivo aquí.
    while(true) delay(1000);
    // Bloquea el ESP32 si no hay conexión WiFi
  }
}

bool connectToWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), password.c_str());
  Serial.print("Conectando a " + ssid);
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 30000) { // Esperar hasta 30 segundos
    delay(500);
    Serial.print(".");
  }

  wifiConnected = (WiFi.status() == WL_CONNECTED);

  if (wifiConnected) {
    Serial.println("\n✓ WiFi conectado: " + WiFi.localIP().toString());
  } else {
    Serial.println("\n✗ Error de conexión WiFi");
  }

  return wifiConnected;
}

void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED && wifiConnected) {
    Serial.println("WiFi desconectado. Intentando reconectar...");
    wifiConnected = false;
    WiFi.reconnect();
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) { // Esperar 10 segundos
      delay(500);
      Serial.print(".");
    }
    wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (wifiConnected) {
      Serial.println("\nWiFi reconectado.");
      // No llamar initializeNTP() aquí directamente, loop() lo manejará.
    } else {
      Serial.println("\nNo se pudo reconectar WiFi.");
    }
  }
}

void initializeMDNS() {
  if (MDNS.begin("esp32-telemetria")) {
    Serial.println("mDNS: http://esp32-telemetria.local");
  } else {
    Serial.println("Error configurando mDNS");
  }
}

void initializeWebServer() {
  server.on("/", handleRoot);
  server.on("/api/data", handleAPIData);
  server.on("/api/history", handleAPIHistory);
  server.on("/api/sd-info", handleSDInfo);
  server.on("/api/download-data", handleDownloadData);
  server.on("/api/ranges", HTTP_GET, handleAPI_GetRanges); // Nueva ruta para obtener rangos
  server.on("/api/ranges", HTTP_POST, handleAPI_SetRanges); // Nueva ruta para establecer rangos
  server.on("/style.css", handleCSS);
  server.on("/script.js", handleJS);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("Servidor web iniciado en puerto 80");
}

void initializeNTP() {
  Serial.print("Sincronizando hora NTP...");
  timeClient.begin();
  timeClient.setTimeOffset(utcOffsetInSeconds);
  unsigned long startTime = millis();
  while(!timeClient.update() && millis() - startTime < 15000) { // Intentar sincronizar por 15 segundos
    Serial.print(".");
    delay(500);
  }
  if (timeClient.isTimeSet()) {
    timeSynchronized = true;
    Serial.println("\n✓ Hora NTP sincronizada: " + timeClient.getFormattedTime());
    if (sdCardAvailable) { // Solo intentar crear el archivo de log si la SD está disponible
        if (!initializeLogFile()) { // Llamar y verificar si se pudo crear
            Serial.println("ADVERTENCIA: Fallo al crear archivo de log inicial con fecha NTP.");
        }
    }
  } else {
    timeSynchronized = false;
    Serial.println("\n✗ No se pudo sincronizar la hora NTP. Los datos en SD no tendrán fecha/hora real.");
  }
}

void processLoRaData() {
  if (!LoRaSerial.available()) return;

  String received = LoRaSerial.readString();
  received.trim();
  // Validar si el mensaje es un +RCV=
  if (received.indexOf("+RCV=") < 0) {
    Serial.println("LoRa (no data): " + received);
    // Mostrar mensajes AT+OK, etc.
    return;
  }

  // Extraer payload: +RCV=address,length,data,RSSI,SNR
  int firstComma = received.indexOf(",");
  int secondComma = received.indexOf(",", firstComma + 1);
  if (firstComma == -1 || secondComma == -1) {
    Serial.println("LoRa (mal formato): " + received);
    return;
  }

  String lengthStr = received.substring(firstComma + 1, secondComma);
  int dataLength = lengthStr.toInt();
  int dataStart = secondComma + 1;
  String payload = received.substring(dataStart, dataStart + dataLength);

  Serial.println("Datos LoRa: " + payload); 
  parseAndStoreSensorData(payload);
}

void parseAndStoreSensorData(String payload) {
  float newTemp = 0, newHum = 0, newLux = 0;
  int newSoil = 0;
  bool tempSet = false, humSet = false, luxSet = false, soilSet = false;
  // Parsear formato: T:temp,H:hum,L:lux,S:soil
  int start = 0, end = 0;
  while (end != -1) {
    end = payload.indexOf(',', start);
    String param = (end == -1) ?
    payload.substring(start) : payload.substring(start, end);
    param.trim();

    if (param.startsWith("T:")) { newTemp = param.substring(2).toFloat(); tempSet = true;
    }
    else if (param.startsWith("H:")) { newHum = param.substring(2).toFloat(); humSet = true;
    }
    else if (param.startsWith("L:")) { newLux = param.substring(2).toFloat(); luxSet = true;
    }
    else if (param.startsWith("S:")) { newSoil = param.substring(2).toInt(); soilSet = true;
    }

    start = end + 1;
  }

  bool changed = false;
  if (sensorData.dataValid == false) { // Primera recepción válida
      changed = true;
  } else { // Si ya tenemos datos, verificar si cambiaron
      if (tempSet && newTemp != sensorData.temperature) changed = true;
      if (humSet && newHum != sensorData.humidity) changed = true;
      if (luxSet && newLux != sensorData.lux) changed = true;
      if (soilSet && newSoil != sensorData.soilMoisture) changed = true;
  }
  
  if (changed) {
    sensorData.temperature = tempSet ?
    newTemp : sensorData.temperature;
    sensorData.humidity = humSet ? newHum : sensorData.humidity;
    sensorData.lux = luxSet ? newLux : sensorData.lux;
    sensorData.soilMoisture = soilSet ? newSoil : sensorData.soilMoisture;
    
    sensorData.lastUpdate = millis();
    sensorData.dataValid = true;
    // Marcamos los datos como válidos

    addToHistory();
    printReceivedData();
    // Guardar inmediatamente en SD si hay datos nuevos y la hora está sincronizada
    if (sdCardAvailable && timeSynchronized) {
      saveToSD();
    } else if (sdCardAvailable && !timeSynchronized) {
        Serial.println("Datos recibidos pero no guardados en SD: Hora no sincronizada.");
    }
  } else {
    Serial.println("Datos LoRa recibidos pero no hubo cambios significativos.");
  }
}


void addToHistory() {
  dataHistory[historyIndex] = {millis(), sensorData.temperature, sensorData.humidity, sensorData.lux, sensorData.soilMoisture};
  historyIndex = (historyIndex + 1) % MAX_HISTORY;
  if (historyCount < MAX_HISTORY) historyCount++;
}

void printReceivedData() {
  Serial.println("--- DATOS RECIBIDOS ---");
  Serial.println("T:" + String(sensorData.temperature, 1) + "°C | H:" + String(sensorData.humidity, 1) +
                "% | L:" + String(sensorData.lux, 0) + "lux | S:" + String(sensorData.soilMoisture) + "%");
  Serial.println("----------------------");
}

void loadSensorRanges() {
  sensorRanges.tempMin = preferences.getFloat("tempMin", -40);
  sensorRanges.tempMax = preferences.getFloat("tempMax", 80);
  sensorRanges.humMin = preferences.getFloat("humMin", 0);
  sensorRanges.humMax = preferences.getFloat("humMax", 100);
  sensorRanges.luxMin = preferences.getFloat("luxMin", 0);
  sensorRanges.luxMax = preferences.getFloat("luxMax", 100000);
  sensorRanges.soilMin = preferences.getInt("soilMin", 0);
  sensorRanges.soilMax = preferences.getInt("soilMax", 100);

  Serial.println("Rangos cargados:");
  Serial.printf("Temp: %.1f - %.1f | Hum: %.1f - %.1f | Lux: %.0f - %.0f | Soil: %d - %d\n",
                sensorRanges.tempMin, sensorRanges.tempMax,
                sensorRanges.humMin, sensorRanges.humMax,
                sensorRanges.luxMin, sensorRanges.luxMax,
                sensorRanges.soilMin, sensorRanges.soilMax);
}

void saveSensorRanges() {
  preferences.putFloat("tempMin", sensorRanges.tempMin);
  preferences.putFloat("tempMax", sensorRanges.tempMax);
  preferences.putFloat("humMin", sensorRanges.humMin);
  preferences.putFloat("humMax", sensorRanges.humMax);
  preferences.putFloat("luxMin", sensorRanges.luxMin);
  preferences.putFloat("luxMax", sensorRanges.luxMax);
  preferences.putInt("soilMin", sensorRanges.soilMin);
  preferences.putInt("soilMax", sensorRanges.soilMax);

  Serial.println("Rangos guardados en memoria flash.");
}

void checkSensorRanges() {
  if (!sensorData.dataValid) return; // No verificar si no hay datos válidos

  bool tempInRange = (sensorData.temperature >= sensorRanges.tempMin && sensorData.temperature <= sensorRanges.tempMax);
  bool humInRange = (sensorData.humidity >= sensorRanges.humMin && sensorData.humidity <= sensorRanges.humMax);
  bool luxInRange = (sensorData.lux >= sensorRanges.luxMin && sensorData.lux <= sensorRanges.luxMax);
  bool soilInRange = (sensorData.soilMoisture >= sensorRanges.soilMin && sensorData.soilMoisture <= sensorRanges.soilMax);

  if (tempInRange && humInRange && luxInRange && soilInRange) {
    if (ledOnStartTime == 0) { // Si el LED no está ya encendido
      digitalWrite(LED_PIN, HIGH);
      ledOnStartTime = millis();
      Serial.println("¡Todos los valores dentro del rango! LED encendido por 15 segundos.");
    }
  } else {
    if (ledOnStartTime != 0) { // Si el LED está encendido y los rangos ya no se cumplen
      digitalWrite(LED_PIN, LOW);
      ledOnStartTime = 0;
      Serial.println("Valores fuera de rango. LED apagado.");
    }
  }
}


void handleRoot() {
  String html = R"(<!DOCTYPE html>
<html lang='es'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>Sistema de Telemetría IoT</title>
    <link rel='stylesheet' href='/style.css'>
    <script src='https://cdn.jsdelivr.net/npm/chart.js'></script>
</head>
<body>
    <div class='container'>
        <header>
            <h1>🌱 Sistema de Telemetría Agrícola</h1>
            <div class='status' id='status'>Conectando...</div>
            <button class='settings-btn' onclick='openModal()'>⚙️ Configurar Rangos</button>
            <div class='led-status-indicator' id='ledStatus'>LED: Apagado</div>
        </header>
        
        <div class='cards-grid'>
            <div class='card temperature'>
                <div class='card-icon'>🌡️</div>
                <div class='card-content'>
                    <h3>Temperatura</h3>
                    <div class='value' id='temperature'>--°C</div>
                </div>
            </div>
            
            <div class='card humidity'>
                <div class='card-icon'>💧</div>
                <div class='card-content'>
                    <h3>Humedad</h3>
                    <div class='value' id='humidity'>--%</div>
                </div>
            </div>
            
            <div class='card light'>
                <div class='card-icon'>☀️</div>
                <div class='card-content'>
                    <h3>Luz</h3>
                    <div class='value' id='light'>-- lux</div>
                </div>
            </div>
            
            <div class='card soil'>
                <div class='card-icon'>🌿</div>
                <div class='card-content'>
                    <h3>Humedad Suelo</h3>
                    <div class='value' id='soil'>--%</div>
                </div>
            </div>
        </div>
        
        <div class='sd-info-container'>
            <div class='card sd-card'>
                <div class='card-icon'>💾</div>
                <div class='card-content'>
                    <h3>Almacenamiento SD</h3>
                    <div class='sd-status' id='sd-status'>Verificando...</div>
                    <button onclick='downloadData()' class='download-btn'>📥 Descargar Datos</button>
                </div>
            </div>
        </div>
        
        <div class='charts-container'>
            <div class='chart-container'>
                <h3>📊 Temperatura, Humedad y Humedad del Suelo</h3>
                <div class='chart-wrapper'>
                    <canvas id='mainChart'></canvas>
                </div>
            </div>
            
            <div class='chart-container'>
                <h3>☀️ Nivel de Luz</h3>
                <div class='chart-wrapper'>
                    <canvas id='lightChart'></canvas>
                </div>
            </div>
        </div>
        
        <div class='info'>
            <p>Última actualización: <span id='lastUpdate'>--</span></p>
        </div>
    </div>
    
    <div id='settingsModal' class='modal'>
        <div class='modal-content'>
            <span class='close-button' onclick='closeModal()'>&times;</span>
            <h2>⚙️ Configurar Rangos de Sensores</h2>
            <div class='range-input-grid'>
                <div class='range-group'>
                    <h4>Temperatura (°C)</h4>
                    <label for='tempMin'>Min:</label>
                    <input type='number' id='tempMin' value='-40'>
                    <label for='tempMax'>Max:</label>
                    <input type='number' id='tempMax' value='80'>
                </div>
                <div class='range-group'>
                    <h4>Humedad (%)</h4>
                    <label for='humMin'>Min:</label>
                    <input type='number' id='humMin' value='0'>
                    <label for='humMax'>Max:</label>
                    <input type='number' id='humMax' value='100'>
                </div>
                <div class='range-group'>
                    <h4>Luz (lux)</h4>
                    <label for='luxMin'>Min:</label>
                    <input type='number' id='luxMin' value='0'>
                    <label for='luxMax'>Max:</label>
                    <input type='number' id='luxMax' value='100000'>
                </div>
                <div class='range-group'>
                    <h4>Humedad Suelo (%)</h4>
                    <label for='soilMin'>Min:</label>
                    <input type='number' id='soilMin' value='0'>
                    <label for='soilMax'>Max:</label>
                    <input type='number' id='soilMax' value='100'>
                </div>
            </div>
            <p class='modal-message' id='modalMessage'></p>
            <div class='modal-buttons'>
                <button onclick='saveRangesFromModal()'>Establecer</button>
                <button onclick='closeModal()' class='cancel-btn'>Cancelar</button>
            </div>
        </div>
    </div>

    <script src='/script.js'></script>
</body>
</html>)";
  server.send(200, "text/html", html);
}

void handleCSS() {
  String css = R"(* {
    margin: 0; padding: 0; box-sizing: border-box;
}

body {
    font-family: 'Arial', sans-serif;
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    min-height: 10vh; padding: 15px;
}

.container { max-width: 1200px; margin: 0 auto; }

header {
    text-align: center; margin-bottom: 25px; color: white;
    position: relative; /* Para posicionar el botón de ajustes */
}

header h1 {
    font-size: 2.2em; margin-bottom: 10px;
    text-shadow: 2px 2px 4px rgba(0,0,0,0.3);
}

.status {
    display: inline-block; padding: 8px 20px;
    background: rgba(255,255,255,0.2); border-radius: 20px;
    font-weight: bold; font-size: 0.9em;
}

.status.online { background: rgba(76, 175, 80, 0.8); }
.status.offline { background: rgba(244, 67, 54, 0.8); }

.settings-btn {
    background: #f39c12; color: white; border: none;
    padding: 8px 15px; border-radius: 8px;
    cursor: pointer; font-size: 0.9em;
    transition: background 0.3s;
    margin-top: 15px;
}
.settings-btn:hover { background: #e67e22; }

.led-status-indicator {
    background: rgba(0,0,0,0.3);
    color: white;
    padding: 5px 10px;
    border-radius: 5px;
    margin-top: 10px;
    font-size: 0.8em;
    display: inline-block;
}
.led-status-indicator.active {
    background: #27ae60; /* Verde */
}

.cards-grid {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
    gap: 15px; margin-bottom: 20px;
}

.sd-info-container {
    margin-bottom: 25px;
}

.sd-card {
    background: rgba(255, 255, 255, 0.95);
    border-radius: 12px;
    padding: 20px;
    box-shadow: 0 6px 20px rgba(0,0,0,0.1);
    text-align: center;
}

.sd-status {
    font-size: 0.9em;
    margin: 10px 0;
    color: #555;
}

.download-btn {
    background: #3498db; color: white; border: none;
    padding: 10px 20px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 0.9em;
    transition: background 0.3s;
}

.download-btn:hover {
    background: #2980b9;
}

.card {
    background: rgba(255, 255, 255, 0.95); border-radius: 12px;
    padding: 20px; box-shadow: 0 6px 20px rgba(0,0,0,0.1);
    backdrop-filter: blur(10px); border: 1px solid rgba(255,255,255,0.3);
    transition: transform 0.3s ease;
}

.card:hover { transform: translateY(-3px);
}

.card-icon {
    font-size: 2.2em; text-align: center; margin-bottom: 10px;
}

.card-content h3 {
    color: #333;
    margin-bottom: 8px; font-size: 1em; text-align: center;
}

.value {
    font-size: 1.8em; font-weight: bold; text-align: center; color: #2c3e50;
}

.temperature .value { color: #e74c3c; }
.humidity .value { color: #3498db; }
.light .value { color: #f39c12; }
.soil .value { color: #27ae60;
}

.charts-container {
    display: grid; grid-template-columns: 1fr 1fr;
    gap: 20px; margin-bottom: 20px;
}

.chart-container {
    background: rgba(255, 255, 255, 0.95); border-radius: 12px;
    padding: 20px; box-shadow: 0 6px 20px rgba(0,0,0,0.1);
    min-height: 350px;
}

.chart-container h3 {
    color: #333; margin-bottom: 15px; text-align: center; font-size: 1.1em;
}

.chart-wrapper {
    position: relative; height: 280px; width: 100%;
}

.chart-wrapper canvas {
    position: absolute;
    top: 0; left: 0;
    width: 100% !important; height: 100% !important;
}

.info {
    text-align: center; color: white;
    font-size: 1em;
    background: rgba(255,255,255,0.1); padding: 12px; border-radius: 8px;
}

/* Modal Styles */
.modal {
    display: none; /* Hidden by default */
    position: fixed; /* Stay in place */
    z-index: 1000; /* Sit on top */
    left: 0;
    top: 0;
    width: 100%; /* Full width */
    height: 100%; /* Full height */
    overflow: auto; /* Enable scroll if needed */
    background-color: rgba(0,0,0,0.6); /* Black w/ opacity */
    justify-content: center;
    align-items: center;
}

.modal-content {
    background-color: #fefefe;
    margin: auto;
    padding: 30px;
    border-radius: 12px;
    width: 90%;
    max-width: 600px;
    box-shadow: 0 8px 25px rgba(0,0,0,0.3);
    position: relative;
    animation: fadeIn 0.3s ease-out;
}

@keyframes fadeIn {
    from { opacity: 0; transform: translateY(-20px); }
    to { opacity: 1; transform: translateY(0); }
}

.close-button {
    color: #aaa;
    float: right;
    font-size: 28px;
    font-weight: bold;
    position: absolute;
    top: 10px;
    right: 20px;
    cursor: pointer;
}

.close-button:hover,
.close-button:focus {
    color: black;
    text-decoration: none;
    cursor: pointer;
}

.modal-content h2 {
    text-align: center;
    color: #333;
    margin-bottom: 25px;
    font-size: 1.5em;
}

.range-input-grid {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 20px;
    margin-bottom: 25px;
}

.range-group {
    background: #e9ecef;
    padding: 15px;
    border-radius: 8px;
    border: 1px solid #dee2e6;
}

.range-group h4 {
    margin-bottom: 10px;
    color: #555;
    font-size: 1.1em;
    text-align: center;
}

.range-group label {
    display: block;
    margin-bottom: 5px;
    color: #333;
    font-size: 0.9em;
}

.range-group input[type='number'] {
    width: calc(100% - 10px);
    padding: 8px;
    margin-bottom: 10px;
    border: 1px solid #ccc;
    border-radius: 4px;
    font-size: 1em;
}

.modal-buttons {
    text-align: center;
}

.modal-buttons button {
    background: #28a745;
    color: white;
    border: none;
    padding: 10px 25px;
    border-radius: 8px;
    cursor: pointer;
    font-size: 1em;
    margin: 0 10px;
    transition: background 0.3s;
}

.modal-buttons button:hover {
    background: #218838;
}

.modal-buttons .cancel-btn {
    background: #6c757d;
}

.modal-buttons .cancel-btn:hover {
    background: #5a6268;
}

.modal-message {
    color: red;
    text-align: center;
    margin-top: -15px;
    margin-bottom: 15px;
    font-size: 0.9em;
}


@media (max-width: 1024px) {
    .charts-container { grid-template-columns: 1fr;
    gap: 15px; }
    .chart-container { min-height: 320px; }
    .chart-wrapper { height: 250px;
    }
}

@media (max-width: 768px) {
    body { padding: 10px; }
    .cards-grid { grid-template-columns: repeat(2, 1fr);
    gap: 12px; }
    header h1 { font-size: 1.8em; }
    .value { font-size: 1.5em;
    }
    .card { padding: 15px; }
    .card-icon { font-size: 1.8em;
    }
    .chart-container { padding: 15px; min-height: 300px; }
    .chart-wrapper { height: 220px;
    }
    .range-input-grid {
        grid-template-columns: 1fr;
    }
}
)" ;
  server.send(200, "text/css", css);
}

void handleJS() {
  String js = R"(let mainChart, lightChart, lastDataTime = 0;
const settingsModal = document.getElementById('settingsModal');
const modalMessage = document.getElementById('modalMessage');
const ledStatusElement = document.getElementById('ledStatus');

document.addEventListener('DOMContentLoaded', function() {
    initCharts();
    updateData();
    updateSDInfo();
    setInterval(updateData, 2000);
    setInterval(updateSDInfo, 10000);
});

function initCharts() {
    const commonOptions = {
        responsive: true, maintainAspectRatio: false,
        plugins: { legend: { position: 'top', labels: { boxWidth: 12, font: { size: 11 }}}},
        elements: { point: { radius: 2 }},
        scales: { x: { display: false }}
    };

    const mainCtx = document.getElementById('mainChart').getContext('2d');
    mainChart = new Chart(mainCtx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [
                { label: 'Temperatura (°C)', data: [], borderColor: '#e74c3c', backgroundColor: 'rgba(231, 76, 60, 0.1)', tension: 0.4, fill: false, borderWidth: 2 },
                { label: 'Humedad (%)', data: [], borderColor: '#3498db', backgroundColor: 'rgba(52, 152, 219, 0.1)', tension: 0.4, fill: false, borderWidth: 2 },
                { label: 'Humedad Suelo (%)', data: [], borderColor: '#27ae60', backgroundColor: 'rgba(39, 174, 96, 0.1)', tension: 0.4, fill: false, borderWidth: 2 }
            ]
        },
        options: { ...commonOptions, scales: { ...commonOptions.scales, y: { beginAtZero: true, max: 100, title: { display: true, text: '% / °C' }}}}
    });
    const lightCtx = document.getElementById('lightChart').getContext('2d');
    lightChart = new Chart(lightCtx, {
        type: 'line',
        data: {
            labels: [],
            datasets: [{ label: 'Nivel de Luz (lux)', data: [], borderColor: '#f39c12', backgroundColor: 'rgba(243, 156, 18, 0.2)', tension: 0.4, fill: true, borderWidth: 2 }]
        },
        options: { ...commonOptions, scales: { ...commonOptions.scales, y: { beginAtZero: true, title: { display: true, text: 'Lux' }}}}
    });
}

async function updateData() {
    try {
        const response = await fetch('/api/data');
        const data = await response.json();
        
        if (data.valid && data.lastUpdate > lastDataTime) {
            document.getElementById('temperature').textContent = data.temperature.toFixed(1) + '°C';
            document.getElementById('humidity').textContent = data.humidity.toFixed(1) + '%';
            document.getElementById('light').textContent = Math.round(data.lux) + ' lux';
            document.getElementById('soil').textContent = data.soilMoisture + '%';
            // Muestra la hora del ESP32, no la del navegador, para reflejar el estado del ESP32
            document.getElementById('lastUpdate').textContent = new Date(data.lastUpdate).toLocaleTimeString();
            document.getElementById('status').textContent = 'En línea';
            document.getElementById('status').className = 'status online';
            
            lastDataTime = data.lastUpdate;
            updateCharts();
        }
        // Actualizar estado del LED basado en la respuesta del ESP32 (asumiendo que 'data' incluye el estado del LED)
        if (data.ledActive) {
            ledStatusElement.textContent = 'LED: Encendido';
            ledStatusElement.classList.add('active');
        } else {
            ledStatusElement.textContent = 'LED: Apagado';
            ledStatusElement.classList.remove('active');
        }

    } catch (error) {
        document.getElementById('status').textContent = 'Desconectado';
        document.getElementById('status').className = 'status offline';
        console.error('Error al actualizar datos:', error); 
        ledStatusElement.textContent = 'LED: Desconocido'; // Si no hay conexión, el estado del LED es desconocido
        ledStatusElement.classList.remove('active');
    }
}

async function updateSDInfo() {
    try {
        const response = await fetch('/api/sd-info');
        const data = await response.json();
        
        const sdStatus = document.getElementById('sd-status');
        if (data.available) {
            sdStatus.textContent = `✓ Activa - ${data.totalEntries} registros guardados`;
            sdStatus.style.color = '#27ae60';
        } else {
            sdStatus.textContent = '✗ No disponible';
            sdStatus.style.color = '#e74c3c';
        }
    } catch (error) {
        document.getElementById('sd-status').textContent = 'Error de conexión';
        console.error('Error al actualizar info de SD:', error); 
    }
}

async function updateCharts() {
    try {
        const response = await fetch('/api/history');
        const history = await response.json();
        
        if (history.length > 0) {
            // Generar etiquetas de tiempo más significativas
            const labels = history.map(d => {
                const date = new Date(d.timestamp); // Asumiendo que timestamp es millis
                return date.toLocaleTimeString(); 
            });

            mainChart.data.labels = labels;
            mainChart.data.datasets[0].data = history.map(d => d.temperature);
            mainChart.data.datasets[1].data = history.map(d => d.humidity);
            mainChart.data.datasets[2].data = history.map(d => d.soilMoisture);
            mainChart.update('none');
            lightChart.data.labels = labels;
            lightChart.data.datasets[0].data = history.map(d => d.lux);
            lightChart.update('none');
        }
    } catch (error) {
        console.error('Error al obtener historial:', error);
    }
}

function downloadData() {
    window.open('/api/download-data', '_blank');
}

function openModal() {
    settingsModal.style.display = 'flex'; // Use flex to center
    loadRangesIntoModal();
    modalMessage.textContent = ''; // Clear previous messages
}

function closeModal() {
    settingsModal.style.display = 'none';
}

async function loadRangesIntoModal() {
    try {
        const response = await fetch('/api/ranges');
        const ranges = await response.json();

        document.getElementById('tempMin').value = ranges.tempMin;
        document.getElementById('tempMax').value = ranges.tempMax;
        document.getElementById('humMin').value = ranges.humMin;
        document.getElementById('humMax').value = ranges.humMax;
        document.getElementById('luxMin').value = ranges.luxMin;
        document.getElementById('luxMax').value = ranges.luxMax;
        document.getElementById('soilMin').value = ranges.soilMin;
        document.getElementById('soilMax').value = ranges.soilMax;
    } catch (error) {
        console.error('Error al cargar rangos:', error);
        modalMessage.textContent = 'Error al cargar rangos. Intente de nuevo.';
        modalMessage.style.color = 'red';
    }
}

async function saveRangesFromModal() {
    const newRanges = {
        tempMin: parseFloat(document.getElementById('tempMin').value),
        tempMax: parseFloat(document.getElementById('tempMax').value),
        humMin: parseFloat(document.getElementById('humMin').value),
        humMax: parseFloat(document.getElementById('humMax').value),
        luxMin: parseFloat(document.getElementById('luxMin').value),
        luxMax: parseFloat(document.getElementById('luxMax').value),
        soilMin: parseInt(document.getElementById('soilMin').value),
        soilMax: parseInt(document.getElementById('soilMax').value)
    };

    // Client-side validation
    if (newRanges.tempMin >= newRanges.tempMax || isNaN(newRanges.tempMin) || isNaN(newRanges.tempMax) ||
        newRanges.humMin >= newRanges.humMax || isNaN(newRanges.humMin) || isNaN(newRanges.humMax) ||
        newRanges.luxMin >= newRanges.luxMax || isNaN(newRanges.luxMin) || isNaN(newRanges.luxMax) ||
        newRanges.soilMin >= newRanges.soilMax || isNaN(newRanges.soilMin) || isNaN(newRanges.soilMax)) {
        modalMessage.textContent = 'Error: Los valores mínimos deben ser menores que los máximos y todos los campos deben ser numéricos.';
        modalMessage.style.color = 'red';
        return;
    }

    try {
        const response = await fetch('/api/ranges', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json'
            },
            body: JSON.stringify(newRanges)
        });

        const result = await response.json();
        if (result.success) {
            modalMessage.textContent = 'Rangos guardados con éxito.';
            modalMessage.style.color = 'green';
            setTimeout(closeModal, 1500); // Close after 1.5 seconds
        } else {
            modalMessage.textContent = 'Error al guardar rangos: ' + result.message;
            modalMessage.style.color = 'red';
        }
    } catch (error) {
        console.error('Error al enviar rangos:', error);
        modalMessage.textContent = 'Error de conexión al guardar rangos. Intente de nuevo.';
        modalMessage.style.color = 'red';
    }
}
)";
  server.send(200, "text/javascript", js);
}

void handleAPIData() {
  DynamicJsonDocument doc(1024);

  doc["temperature"] = sensorData.temperature;
  doc["humidity"] = sensorData.humidity;
  doc["lux"] = sensorData.lux;
  doc["soilMoisture"] = sensorData.soilMoisture;
  doc["lastUpdate"] = sensorData.lastUpdate;
  doc["valid"] = sensorData.dataValid;
  doc["uptime"] = millis();
  doc["sdAvailable"] = sdCardAvailable;
  doc["timeSynchronized"] = timeSynchronized; // Añadir estado de sincronización de hora
  doc["ledActive"] = (ledOnStartTime > 0); // Estado actual del LED

  String response;
  serializeJson(doc, response);

  server.send(200, "application/json", response);
}

void handleAPIHistory() {
  DynamicJsonDocument doc(4096); // Ajustado para un historial más grande
  JsonArray array = doc.to<JsonArray>();

  for (int i = 0; i < historyCount; i++) {
    int index = (historyIndex - historyCount + i + MAX_HISTORY) % MAX_HISTORY;
    
    JsonObject point = array.createNestedObject();
    point["timestamp"] = dataHistory[index].timestamp;
    point["temperature"] = dataHistory[index].temperature;
    point["humidity"] = dataHistory[index].humidity;
    point["lux"] = dataHistory[index].lux;
    point["soilMoisture"] = dataHistory[index].soilMoisture;
  }

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleSDInfo() {
  DynamicJsonDocument doc(512);
  doc["available"] = sdCardAvailable;
  if (sdCardAvailable) {
    doc["cardSize"] = SD.cardSize() / (1024 * 1024);
    doc["usedSpace"] = SD.usedBytes() / (1024 * 1024);
    doc["currentFile"] = currentLogFile;

    // Contar registros en el archivo actual
    int totalEntries = 0;
    File file = SD.open(currentLogFile, FILE_READ);
    if (file) {
      // Saltar la primera línea (cabecera)
      String header = file.readStringUntil('\n');
      while (file.available()) {
        String line = file.readStringUntil('\n');
        if (line.length() > 0) { // Contar solo líneas con contenido
          totalEntries++;
        }
      }
      file.close();
    }
    doc["totalEntries"] = totalEntries;
  } else {
    doc["totalEntries"] = 0;
  }

  String response;
  serializeJson(doc, response);

  server.send(200, "application/json", response);
}

void handleDownloadData() {
  if (!sdCardAvailable) {
    server.send(404, "text/plain", "SD Card no disponible");
    return;
  }

  // Si no hay un archivo de log actual, intentar abrir el último o el primero disponible
  if (currentLogFile == "" || !SD.exists(currentLogFile)) {
      Serial.println("currentLogFile no establecido o no existe, intentando encontrar un archivo CSV.");
      File root = SD.open("/data");
      if (root) {
          File file = root.openNextFile();
          while(file){
              String fileName = file.name();
              if (!file.isDirectory() && fileName.endsWith(".csv")) {
                  currentLogFile = "/data/" + fileName;
                  Serial.println("Usando archivo: " + currentLogFile);
                  break;
              }
              file = root.openNextFile();
          }
          root.close();
      }
      if (currentLogFile == "" || !SD.exists(currentLogFile)) {
          server.send(404, "text/plain", "No se encontró ningún archivo de datos en la SD.");
          return;
      }
  }

  File file = SD.open(currentLogFile, FILE_READ);
  if (!file) {
    server.send(404, "text/plain", "Archivo no encontrado: " + currentLogFile);
    return;
  }

  server.setContentLength(file.size());
  server.sendHeader("Content-Disposition", "attachment; filename=" + currentLogFile.substring(currentLogFile.lastIndexOf('/') + 1));
  server.send(200, "text/csv", "");
  // Envía la cabecera, luego los datos

  WiFiClient client = server.client();
  if (client.connected()) { // Asegurarse de que el cliente aún está conectado
    size_t bytesSent = 0;
    uint8_t buffer[1024];
    while (file.available() && client.connected()) {
      size_t bytesRead = file.read(buffer, sizeof(buffer));
      bytesSent += client.write(buffer, bytesRead);
    }
    Serial.printf("Descarga de %s completa. Bytes enviados: %zu\n", currentLogFile.c_str(), bytesSent);
  } else {
    Serial.println("Cliente desconectado durante la descarga.");
  }
  
  file.close();
}

void handleAPI_GetRanges() {
  DynamicJsonDocument doc(512);
  doc["tempMin"] = sensorRanges.tempMin;
  doc["tempMax"] = sensorRanges.tempMax;
  doc["humMin"] = sensorRanges.humMin;
  doc["humMax"] = sensorRanges.humMax;
  doc["luxMin"] = sensorRanges.luxMin;
  doc["luxMax"] = sensorRanges.luxMax;
  doc["soilMin"] = sensorRanges.soilMin;
  doc["soilMax"] = sensorRanges.soilMax;

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleAPI_SetRanges() {
  DynamicJsonDocument doc(512);
  String requestBody = server.arg("plain");
  DeserializationError error = deserializeJson(doc, requestBody);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.f_str());
    server.send(400, "application/json", "{\"success\":false,\"message\":\"JSON inválido\"}");
    return;
  }

  float newTempMin = doc["tempMin"].as<float>();
  float newTempMax = doc["tempMax"].as<float>();
  float newHumMin = doc["humMin"].as<float>();
  float newHumMax = doc["humMax"].as<float>();
  float newLuxMin = doc["luxMin"].as<float>();
  float newLuxMax = doc["luxMax"].as<float>();
  int newSoilMin = doc["soilMin"].as<int>();
  int newSoilMax = doc["soilMax"].as<int>();

  // Server-side validation
  if (newTempMin >= newTempMax ||
      newHumMin >= newHumMax ||
      newLuxMin >= newLuxMax ||
      newSoilMin >= newSoilMax) {
    server.send(400, "application/json", "{\"success\":false,\"message\":\"El valor mínimo no puede ser mayor o igual al máximo para cualquier rango.\"}");
    return;
  }
    if (newLuxMin < 0 || newSoilMin < 0 || newHumMin < 0) { // Example for minimum values
        server.send(400, "application/json", "{\"success\":false,\"message\":\"Los valores mínimos de lux, humedad y suelo no pueden ser negativos.\"}");
        return;
    }


  sensorRanges.tempMin = newTempMin;
  sensorRanges.tempMax = newTempMax;
  sensorRanges.humMin = newHumMin;
  sensorRanges.humMax = newHumMax;
  sensorRanges.luxMin = newLuxMin;
  sensorRanges.luxMax = newLuxMax;
  sensorRanges.soilMin = newSoilMin;
  sensorRanges.soilMax = newSoilMax;

  saveSensorRanges();
  server.send(200, "application/json", "{\"success\":true,\"message\":\"Rangos actualizados con éxito.\"}");
  Serial.println("Rangos recibidos y actualizados desde la web.");
}


void handleNotFound() {
  server.send(404, "text/plain", "Not Found");
}
