#define TINY_GSM_MODEM_SIM7080
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <DNSServer.h>
#include <ESP32httpUpdate.h>
#include <ESPAsyncWebServer.h>
#include <PubSubClient.h>
#include <SPIFFS.h>
#include <TinyGsmClient.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <ctime.h>

// ===== Firmware =====
static const char* firmwareVersion = "1.0.0";
static const char* firmwareURL     = "http://testeagua.ddns.net:5000/firmware-update"; //update URL

// ===== MODO CONFIG =====
const char *AP_SSID = "hugenrede";
const char *AP_PASS = "hugen123";
const IPAddress AP_IP(192, 168, 4, 1);  
const byte DNS_PORT = 53;

// ===== GLOBAL OBJECTS =====
AsyncWebServer server(80);
DNSServer dnsServer;

// ===== CONNECTION MODE =====
enum ConnectionMode {
  MODE_CONFIG = 0,
  MODE_WIFI = 1,
  MODE_GSM = 2
};

ConnectionMode currentMode = MODE_WIFI;
//ConnectionMode fallbackMode = MODE_GSM;

// ===== SENSOR DATA STRUCTURE =====
struct SensorReading {
  unsigned long timestamp;  // Unix timestamp
  float fluxo_agua;
  float fluxo_total;
  float pressao;
  float distancia;
  bool sent;  // Flag to track if sent
};

// ===== AVERAGING STRUCTURE =====
struct SensorAccumulator {
  float sum_fluxo_agua;
  float sum_fluxo_total;
  float sum_pressao;
  float sum_distancia;
  int count;
  unsigned long firstReadTime;
};

// ===== CIRCULAR BUFFER =====
const int BUFFER_SIZE = 1440;  // Store 1440 minutes = 24 hours of data
SensorReading dataBuffer[BUFFER_SIZE];
volatile int writeIndex = 0;
volatile int readIndex = 0;
volatile int bufferCount = 0;
SemaphoreHandle_t bufferMutex;

// Accumulator for averaging (not in buffer yet)
SensorAccumulator accumulator = { 0, 0, 0, 0, 0, 0 };

// ===== MQTT CONNECTION =====
const char *mqtt_server = "testeagua.ddns.net";
const int mqtt_port = 1883;
String SIMREADSTR, SERIALREADSTR, payload, mqtt_addr_str, mqtt_addr_subscription_str, device;
unsigned long stateStartTime = 0;
unsigned long modemPowerTime = 0;
int failedAttempts = 0;
bool modemPowered = false;
bool networkCheckStarted = false;
bool gprsCheckStarted = false;
bool timeInitialized = false;

// ===== NTP CONFIGURATION =====
const char *ntpServer = "pool.ntp.org";
const long gmtOffset_sec = -10800;  // GMT-3 (Brazil)
const int daylightOffset_sec = 0;

// ===== TIMING CONSTANTS/RECONNECTION =====
const int MAX_WIFI_RETRIES = 5;
const int MAX_GSM_RETRIES = 5;
const unsigned long WIFI_CONNECT_TIMEOUT = 10000;
const unsigned long GSM_NETWORK_TIMEOUT = 60000;
const unsigned long GSM_GPRS_TIMEOUT = 30000;
const unsigned long MQTT_RECONNECT_INTERVAL = 10000;
const unsigned long MODE_SWITCH_TIMEOUT = 90000;
const unsigned long SENSOR_READ_INTERVAL = 1000;  // Read sensors every 1 second
const unsigned long AVERAGE_INTERVAL = 60000;     // Send average every 60 seconds (1 minute)

// ===== MQTT MODEM =====
HardwareSerial SIM7080G(0);
TinyGsm modem(SIM7080G);
TinyGsmClient gsmClient(modem);
PubSubClient mqttGSM(gsmClient);
const char apn[] = "meta.br";  // Your APN
const char user[] = "";        // Usually blank
const char pass[] = "";        // Usually blank
unsigned long lastReconnectAttempt = 0;
unsigned long modeStartTime = 0;
int reconnectAttempts = 0;
bool modemInitialized = false;

// ===== MQTT WIFI =====
WiFiClient wifiClient;
PubSubClient mqttWiFi(wifiClient);
PubSubClient *activeMQTT = nullptr;

// ===== TESTE =====
String wifiSSID = "HugenPLUS";
String wifiPass = "H32851112";

// ===== JSON FILE =====
#define config_data "/CONFIG.json"
#define size_config 1024

/*Configs: wifi, senha wifi, prioridade, caixa d'agua limite*/
String Mac, Config2;
String wifiConf, senhaConf, prioridadeConf;
char cMac[20];
char cConfig2[10];

// ===== STATE VARIABLES =====
struct WiFiStatus {
  bool conectado = false;
  bool erro = false;
  bool desconectado = false;
  bool senhaIncorreta = false;
  bool noSsid = false;
  bool espera = false;
} status;

String novoSsid, novaSenha, site_payload;
volatile bool flag_novarede = false;

// SENSORES
// SENSOR DE PRESSÃO HK2404 (0.5 MPa = 5.0 bar)
#define sensor 3
#define voltagemMinima 0.5
#define voltagemMaxima 4.5
#define pressaoMinima 0.0
#define pressaoMaxima 5.0  // Corrigido: 0.5 MPa = 5.0 bar
#define ADC_VOLTAGE 3.3    // ESP32 usa 3.3V, não 5V

// SENSOR DE FLUXO YF-S403
const int INTERRUPCAO_SENSOR = 8;
const int PINO_SENSOR = 8;
volatile unsigned long contador = 0;
const float FATOR_CALIBRACAO = 5880.0;  // Pulsos por litro para YF-S403
float fluxo = 0;
float volume_total = 0;
unsigned long tempo_antes = 0;

// SENSOR DE DISTÂNCIA RCWL-1670
const int trigPin = 38;
const int echoPin = 39;
#define SOUND_SPEED 0.0343  // cm/µs a 20°C
long duration;
float distanceCm;
float fluxo_total;

void IRAM_ATTR contarPulso() {
  contador++;
}

// ===== CAPTIVE PORTAL HANDLER =====
class CaptiveRequestHandler : public AsyncWebHandler {
  bool canHandle(AsyncWebServerRequest *request) {
    return true;
  }

  void handleRequest(AsyncWebServerRequest *request) {
    String path = request->url();
    if (path == "/" || path == "/index.html" || path == "/scan" || path == "/wifi_status.json" || path.endsWith(".css") || path.endsWith(".js") || (request->method() == HTTP_POST)) return;
    request->redirect("http://192.168.4.1/index.html");
  }
};

// ===== HELPER FUNCTIONS =====
void buildPayload() {
  site_payload = "{\"Conectado\":" + String(status.conectado) + ",\"NoSsid\":" + String(status.noSsid) + ",\"Desconectado\":" + String(status.desconectado) + ",\"Erro\":" + String(status.erro) + ",\"SenhaIncorreta\":" + String(status.senhaIncorreta) + ",\"Espera\":" + String(status.espera) + "}";
}

void resetToAP() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  dnsServer.start(DNS_PORT, "*", AP_IP);
}

void WiFiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    Serial.println("✓ Connected to WiFi");
    status = { true, false, false, false, false, false };
    buildPayload();
  }

  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    Serial.print("✗ Disconnected (reason ");
    Serial.print(info.wifi_sta_disconnected.reason);
    Serial.println(")");

    resetToAP();
    status = { false, true, true, false, false, false };

    if (info.wifi_sta_disconnected.reason == 15 || info.wifi_sta_disconnected.reason == 204)
      status.senhaIncorreta = true;
    else if (info.wifi_sta_disconnected.reason == 201)
      status.noSsid = true;

    buildPayload();
  } else {
    Serial.print("WiFiEvent not caught: ");
    Serial.println(event);
  }
}

String scanNetworks() {
  String json = "[";
  int n = WiFi.scanComplete();

  if (n == -2) WiFi.scanNetworks(true);
  else if (n > 0) {
    for (int i = 0; i < n; i++) {
      if (i) json += ",";
      json += "{\"rssi\":" + String(WiFi.RSSI(i)) + ",\"ssid\":\"" + WiFi.SSID(i) + "\"" + ",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\"" + ",\"channel\":" + String(WiFi.channel(i)) + ",\"secure\":" + String(WiFi.encryptionType(i)) + "}";
    }
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
  }
  return json + "]";
}

void setupRoutes() {
  // Static files
  server.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/bootstrap.min.css", "text/css");
  });
  server.on("/jquery.min.js", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/jquery.min.js", "text/javascript");
  });
  server.on("/bootstrap.min.js", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/bootstrap.min.js", "text/javascript");
  });

  // Main page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/index.html", "text/html");
  });
  server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(SPIFFS, "/index.html", "text/html");
  });

  // API endpoints
  server.on("/wifi_status.json", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", site_payload);
  });
  server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *req) {
    req->send(200, "application/json", scanNetworks());
  });

  // WiFi connection
  server.on("/", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("RECEBEU POST WIFI");
    if (req->hasParam("ssid", true) && req->hasParam("senha", true)) {
      novoSsid = req->getParam("ssid", true)->value();
      novaSenha = req->getParam("senha", true)->value();
      flag_novarede = true;
      Serial.println("HAS PARADAS");
    }
    req->send(200);
  });

  // Change mode
  server.on("/config", HTTP_POST, [](AsyncWebServerRequest *req) {
    Serial.println("Config requisitada");
    req->send(200);
  });

  // Captive portal detection (iOS, Android, Windows)
  auto redirect = [](AsyncWebServerRequest *req) {
    req->redirect("/index.html");
  };
  server.on("/generate_204", HTTP_GET, redirect);
  server.on("/gen_204", HTTP_GET, redirect);
  server.on("/hotspot-detect.html", HTTP_GET, redirect);
  server.on("/connecttest.txt", HTTP_GET, redirect);

  // Catch-all handler
  server.addHandler(new CaptiveRequestHandler()).setFilter(ON_AP_FILTER);
}

/*Configs: wifi, senha wifi, prioridade, caixa d'agua limite*/
bool loaddata(String filename) {
  if (SPIFFS.begin(false) || SPIFFS.begin(true)) {
    if (SPIFFS.exists(filename)) {
      if (filename == "/CONFIG.json") {
        File configFile = SPIFFS.open(filename, "r");
        if (configFile) {
          StaticJsonDocument<size_config> json;
          DeserializationError error = deserializeJson(json, configFile);
          if (!error) {
            JsonObject parte = json["parte"][0];
            Mac = strcpy(cMac, parte["mac"]);
            Config2 = strcpy(cConfig2, parte["config2"]);
            return true;
          } else {
            Serial.println("Failed to load");
          }
        } else {
          Serial.println("Failed to load");
        }
      } else {
        Serial.println("filename dont match");
      }
    }
  } else {
    Serial.println("Failed to mount FS");
  }
  return false;
}

void saveConfigFile(String filename) {
  if (filename == "/CONFIG.json") {
    StaticJsonDocument<size_config> json;
    //LOCAL
    JsonObject parte = json["parte"].createNestedObject();
    parte["mac"] = Mac.c_str();
    parte["config2"] = Config2.c_str();
    File configFile = SPIFFS.open(filename, "w");
    if (!configFile) {
      Serial.println("failed to open json file for writing");
    }
    if (serializeJson(json, configFile) == 0) {
      Serial.println(F("Failed to write to file"));
    }
    configFile.close();
  }
}

void modemOnOff() {
  digitalWrite(40, LOW);
  delay(2000);
  digitalWrite(40, HIGH);
  delay(3000);
}

bool modemStatus() {
  if (modem.testAT()) {
    Serial.println("Modem is ON and communicating.");
    return true;
  } else {
    Serial.println("Modem appears OFF or unresponsive.");
    return false;
  }
}

// ===== MODEM INITIALIZATION =====
bool initModem() {
  if (modemInitialized == false) {
    pinMode(40, OUTPUT);
    digitalWrite(40, HIGH);
  }

  if (!modemStatus()) {
    modemOnOff();
  }

  Serial.println("Initializing modem...");
  modem.restart();
  delay(3000);
  modemInitialized = true;
  return true;
}

// ===== WIFI CONNECTION =====
bool connectWiFi() {
  Serial.println("→ Attempting WiFi connection to: " + wifiSSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSSID.c_str(), wifiPass.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ WiFi connected: " + WiFi.localIP().toString());
    activeMQTT = &mqttWiFi;
    mqttWiFi.setServer(mqtt_server, mqtt_port);
    mqttWiFi.setCallback(callback);
    mqttWiFi.setKeepAlive(60);
    reconnectAttempts = 0;
    initTime();
    return true;
  }

  Serial.println("\n✗ WiFi connection failed");
  return false;
}

// ===== GSM CONNECTION =====
bool connectGSM() {
  Serial.println("→ Attempting GSM connection");
  if (!initModem()) return false;

  Serial.print("Waiting for network...");
  if (!modem.waitForNetwork(180000)) {
    Serial.println("\n✗ Failed to connect to network");
    return false;
  }
  Serial.println("\n✓ Network connected");

  Serial.print("Connecting to GPRS...");
  if (!modem.gprsConnect(apn, user, pass)) {
    Serial.println("\n✗ Failed to connect to GPRS");
    return false;
  }
  Serial.println("\n✓ GPRS connected");
  initTime();

  activeMQTT = &mqttGSM;
  mqttGSM.setServer(mqtt_server, mqtt_port);
  mqttGSM.setCallback(callback);
  mqttGSM.setKeepAlive(60);
  reconnectAttempts = 0;
  return true;
}

// ===== MQTT CONNECTION =====
bool connectMQTT() {
  if (activeMQTT == nullptr) return false;

  Serial.print("Connecting to MQTT... ");
  if (activeMQTT->connect(device.c_str())) {
    Serial.println("✓ Connected");
    activeMQTT->publish(mqtt_addr_str.c_str(), "Device connected");
    activeMQTT->subscribe(mqtt_addr_subscription_str.c_str());
    return true;
  }

  Serial.print("✗ Failed, rc=");
  Serial.println(activeMQTT->state());
  return false;
}

// ===== CONNECTION MANAGER =====
bool isConnected() {
  if (currentMode == MODE_WIFI) {
    return WiFi.status() == WL_CONNECTED && activeMQTT && activeMQTT->connected();
  } else if (currentMode == MODE_GSM) {
    return modem.isNetworkConnected() && modem.isGprsConnected() && activeMQTT && activeMQTT->connected();
  }
  return false;
}

void switchMode() {
  Serial.println("\n═══ SWITCHING CONNECTION MODE ═══");

  // Disconnect current mode
  if (currentMode == MODE_WIFI) {
    WiFi.disconnect();
    currentMode = MODE_GSM;
    Serial.println("Switching from WiFi to GSM");
  } else {
    modem.gprsDisconnect();
    if (modemStatus()) {
      modemOnOff();
    }
    currentMode = MODE_WIFI;
    Serial.println("Switching from GSM to WiFi");
  }

  reconnectAttempts = 0;
  modeStartTime = millis();
  activeMQTT = nullptr;
}

bool attemptConnection() {
  bool connected = false;

  if (currentMode == MODE_WIFI) {
    connected = connectWiFi() && connectMQTT();
  } else if (currentMode == MODE_GSM) {
    connected = connectGSM() && connectMQTT();
  }

  if (!connected) {
    reconnectAttempts++;
    int maxRetries = (currentMode == MODE_WIFI) ? MAX_WIFI_RETRIES : MAX_GSM_RETRIES;

    if (reconnectAttempts >= maxRetries) {
      Serial.println("✗ Max retries reached, switching mode...");
      switchMode();
    }
  } else {
    modeStartTime = millis();
  }

  return connected;
}

void manageConnection() {
  // Check if mode has been trying for too long without success
  if (millis() - modeStartTime > MODE_SWITCH_TIMEOUT && !isConnected()) {
    Serial.println("✗ Timeout on current mode, forcing switch...");
    switchMode();
    return;
  }

  // If connected, just maintain
  if (isConnected()) {
    activeMQTT->loop();

    // Additional checks for GSM
    if (currentMode == MODE_GSM) {
      if (!modem.isNetworkConnected() || !modem.isGprsConnected()) {
        Serial.println("✗ GSM connection lost");
        reconnectAttempts++;
      }
    }
    return;
  }

  // Need to reconnect
  unsigned long now = millis();
  if (now - lastReconnectAttempt > MQTT_RECONNECT_INTERVAL) {
    lastReconnectAttempt = now;
    Serial.println("\n→ Connection lost, attempting reconnection...");
    attemptConnection();
  }
}

// ===== MQTT CALLBACK =====
void callback(char *topic, byte *payload, unsigned int length) {
  Serial.print("Message: ");
  String payload_str;
  for (int i = 0; i < length; i++) {
    Serial.print((char)payload[i]);
    payload_str += (char)payload[i];
  }
  Serial.println();

  if (payload_str.startsWith("Update:")) {
    // Send confirmation back
    String confirmTopic = "esp/" + String(Mac) + "/confirm";
    String confirmMsg = "ACK:Update:received";
    activeMQTT->publish(confirmTopic.c_str(), confirmMsg.c_str());
    Serial.println("→ New configuration recieved");
  } else if (payload_str.startsWith("Ping")) {
    // Respond to status check
    String confirmTopic = "esp/" + String(Mac) + "/confirm";
    String statusMsg = "STATUS:online:firmware_v" + String(firmwareVersion);
    activeMQTT->publish(confirmTopic.c_str(), statusMsg.c_str());
    Serial.println("→ Ping received");
  } else if (payload_str.startsWith("Firmware Update")) {
    String confirmTopic = "esp/" + String(Mac) + "/confirm";;
    String statusMsg = "STATUS:online:firmware_v" + String(firmwareVersion);
    activeMQTT->publish(confirmTopic.c_str(), statusMsg.c_str());
    Serial.println("→ Firmware update requested");
    performOTA();  // Add this to trigger update
  }
   /* else if (payload_str.startsWith("Config:")) {
    Serial.println("Configuração recebida");
    
    // Send confirmation
    String confirmTopic = "esp/" + String(Mac) + "/confirm";
    String confirmMsg = "ACK:Config:success";
    mqttClient.publish(confirmTopic.c_str(), confirmMsg.c_str());
    
  } else if (payload_str.startsWith("Ping")) {
    // Respond to status check
    String confirmTopic = "esp/" + String(Mac) + "/status";
    String statusMsg = "STATUS:online:firmware_v1.0";
    mqttClient.publish(confirmTopic.c_str(), statusMsg.c_str());
  } */

  payload_str = "";
}

// ===== BUFFER FUNCTIONS (THREAD-SAFE) =====
bool addToBuffer(const SensorReading &reading) {
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (bufferCount < BUFFER_SIZE) {
      dataBuffer[writeIndex] = reading;
      writeIndex = (writeIndex + 1) % BUFFER_SIZE;
      bufferCount++;
      xSemaphoreGive(bufferMutex);
      return true;
    } else {
      // Buffer full - overwrite oldest (circular buffer behavior)
      Serial.println("⚠ Buffer full, overwriting oldest data");
      dataBuffer[writeIndex] = reading;
      writeIndex = (writeIndex + 1) % BUFFER_SIZE;
      readIndex = (readIndex + 1) % BUFFER_SIZE;  // Move read pointer too
      xSemaphoreGive(bufferMutex);
      return true;
    }
  }
  return false;
}

bool getFromBuffer(SensorReading &reading) {
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (bufferCount > 0) {
      reading = dataBuffer[readIndex];
      xSemaphoreGive(bufferMutex);
      return true;
    }
    xSemaphoreGive(bufferMutex);
  }
  return false;
}

void removeFromBuffer() {
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    if (bufferCount > 0) {
      readIndex = (readIndex + 1) % BUFFER_SIZE;
      bufferCount--;
    }
    xSemaphoreGive(bufferMutex);
  }
}

int getBufferCount() {
  int count = 0;
  if (xSemaphoreTake(bufferMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    count = bufferCount;
    xSemaphoreGive(bufferMutex);
  }
  return count;
}

// ===== TIME FUNCTIONS =====
unsigned long getTimestamp() {
  time_t now;
  time(&now);
  return (unsigned long)now;
}

String formatTimestamp(unsigned long timestamp) {
  struct tm timeinfo;
  time_t t = timestamp;
  localtime_r(&t, &timeinfo);
  char buffer[30];
  strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buffer);
}

void initTime() {
  if (currentMode == MODE_WIFI && WiFi.status() == WL_CONNECTED) {
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    Serial.println("→ Syncing time with NTP...");

    struct tm timeinfo;
    int attempts = 0;
    while (!getLocalTime(&timeinfo) && attempts < 10) {
      delay(500);
      attempts++;
    }

    if (attempts < 10) {
      timeInitialized = true;
      Serial.println("✓ Time synchronized: " + formatTimestamp(getTimestamp()));
    } else {
      Serial.println("⚠ Time sync failed, using millis()");
    }
  } else if (currentMode == MODE_GSM && modem.isGprsConnected()) {
    Serial.println("→ Syncing time with GSM...");

    // Configure timezone and NTP
    modem.sendAT("+CLTS=1");  // Enable local timestamp
    modem.waitResponse();
    modem.sendAT("+CNTP=\"pool.ntp.org\",-12");  // Set NTP server with GMT-3
    modem.waitResponse();
    modem.sendAT("+CNTP");      // Trigger NTP sync
    modem.waitResponse(10000);  // Wait up to 10 seconds for sync

    delay(3000);  // Additional wait for time to stabilize

    // Try getting time
    int year, month, day, hour, minute, second;
    float timezone;

    if (modem.getNetworkTime(&year, &month, &day, &hour, &minute, &second, &timezone)) {
      // Validate year (should be 2024-2030)
      if (year >= 2024 && year <= 2100) {
        Serial.printf("🛰 GSM Time: %04d-%02d-%02d %02d:%02d:%02d (tz=%.1f)\n",
                      year, month, day, hour, minute, second, timezone);

        struct tm t = {};
        t.tm_year = year - 1900;
        t.tm_mon = month - 1;
        t.tm_mday = day;
        t.tm_hour = hour;
        t.tm_min = minute;
        t.tm_sec = second;

        time_t timeSinceEpoch = mktime(&t);

        // Already in local time from NTP with -12 quarters, so don't add offset again
        Serial.println("🕒 Epoch time: " + String(timeSinceEpoch));

        struct timeval now = { .tv_sec = timeSinceEpoch };
        settimeofday(&now, NULL);
        timeInitialized = true;
        Serial.println("✓ Time from GSM: " + formatTimestamp(getTimestamp()));
      } else {
        Serial.printf("⚠ Invalid year: %d (data not synced yet)\n", year);
      }
    } else {
      Serial.println("⚠ Failed to get GSM time - modem may need more time to sync");
    }
  }
}

// ===== SENSOR READING (RUNS ON CORE 1) =====
void sensorTask(void *parameter) {
  unsigned long lastRead = 0;
  unsigned long lastAverage = 0;

  Serial.println("✓ Sensor task started on Core " + String(xPortGetCoreID()));

  while (true) {
    unsigned long now = millis();

    // Read sensors every second
    if (now - lastRead >= SENSOR_READ_INTERVAL) {
      lastRead = now;

      // Read your sensors here (REPLACE WITH ACTUAL SENSOR CODE)
      float fluxo_agua = sensor_fluxo();    // Example: 0-10.0
      float pressao = sensor_pressao();    // Example: 90-110 kPa
      float distancia = sensor_distancia();     // Example: 0-50.0 cm

      if (distancia >= 400) {
        Serial.println("distancia maxima ultrapassada");
        distancia = 0;
      }

      //Serial.print("Fluxo de agua: "); Serial.println(fluxo_agua);
      //Serial.print("Total de agua: "); Serial.println(fluxo_total);
      //Serial.print("pressao de agua: "); Serial.println(pressao);
      //Serial.print("distancia de agua: "); Serial.println(distancia);

      // Accumulate for averaging
      if (accumulator.count == 0) {
        accumulator.firstReadTime = timeInitialized ? getTimestamp() : (now / 1000);
      }
      accumulator.sum_fluxo_agua += fluxo_agua;
      accumulator.sum_fluxo_total += fluxo_total;
      accumulator.sum_pressao += pressao;
      accumulator.sum_distancia += distancia;
      accumulator.count++;
    }

    // Calculate and store average every minute
    if (now - lastAverage >= AVERAGE_INTERVAL) {
      lastAverage = now;

      if (accumulator.count > 0) {
        SensorReading avgReading;
        avgReading.timestamp = accumulator.firstReadTime;
        avgReading.fluxo_agua = accumulator.sum_fluxo_agua / accumulator.count;
        avgReading.fluxo_total = accumulator.sum_fluxo_total / accumulator.count;
        avgReading.pressao = accumulator.sum_pressao / accumulator.count;
        avgReading.distancia = accumulator.sum_distancia / accumulator.count;
        avgReading.sent = false;

        // Add averaged reading to buffer
        if (addToBuffer(avgReading)) {
          Serial.print("📊 Avg from ");
          Serial.print(accumulator.count);
          Serial.print(" samples | Buffer: ");
          Serial.print(getBufferCount());
          Serial.println(" minutes");
        }

        // Reset accumulator
        accumulator = { 0, 0, 0, 0, 0, 0 };
      }
    }

    // Small delay to prevent watchdog issues
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void publishBufferedData() {

  if (activeMQTT && activeMQTT->connected()) {
    // Try to send up to 10 readings per loop iteration (faster catch-up)
    int sent = 0;
    while (sent < 10 && getBufferCount() > 0) {
      SensorReading reading;
      if (getFromBuffer(reading)) {
        // Build JSON payload
        StaticJsonDocument<256> doc;
        doc["mac_addr"] = Mac;
        doc["pasw"] = "1234";

        //doc["timestamp"] = reading.timestamp;
        doc["data_hora"] = formatTimestamp(reading.timestamp);
        doc["fluxo_agua"] = round(reading.fluxo_agua * 100) / 100;
        doc["fluxo_total"] = round(reading.fluxo_total * 100) / 100;
        doc["pressao"] = round(reading.pressao * 100) / 100;
        doc["distancia"] = round(reading.distancia * 100) / 100;
        doc["aviso"] = 0;
        //doc["type"] = "averaged";  // Indicates this is 1-minute average

        String payload;
        serializeJson(doc, payload);

        String topic = "esp/" + Mac + "/data";

        if (activeMQTT->publish(topic.c_str(), payload.c_str())) {
          removeFromBuffer();
          sent++;

          // Print status
          if (sent == 1 || getBufferCount() % 10 == 0) {
            Serial.print("✓ Sent ");
            Serial.print(sent);
            Serial.print(" | Remaining: ");
            Serial.print(getBufferCount());
            Serial.println(" min");
          }
        } else {
          Serial.println("✗ Publish failed");
          break;  // Stop trying if publish fails
        }
      }
    }
  }
}

// ===== MESSAGE LOOP =====
/* void publishData() {
  static unsigned long lastMsg = 0;
  
  if (millis() - lastMsg >= 10000) {
    lastMsg = millis();
    
    if (activeMQTT && activeMQTT->connected()) {
      String topic = "esp/" + Mac + "/data";
      String payload = "{\"mac_addr\":\"" + Mac + "\",\"mode\":" + 
                      String(currentMode) + ",\"rssi\":" + String(WiFi.RSSI()) + "}";
      
      if (activeMQTT->publish(topic.c_str(), payload.c_str())) {
        Serial.println("✓ Message published");
      } else {
        Serial.println("✗ Publish failed");
      }
    }
  }
} */

void configMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  WiFi.onEvent(WiFiEvent);

  dnsServer.start(DNS_PORT, "*", AP_IP);
  setupRoutes();
  server.begin();

  buildPayload();
  Serial.println("✓ Captive Portal Ready at " + WiFi.softAPIP().toString());

  while (true) {
    dnsServer.processNextRequest();

    if (flag_novarede) {
      Serial.println("→ Connecting to: " + novoSsid + " " + novaSenha);
      dnsServer.stop();
      WiFi.begin(novoSsid, novaSenha);
      buildPayload();
      flag_novarede = false;
    }
  }
}

// Test function to check server response (call this before actual OTA)
void testOTAServer() {
  Serial.println("═══ TESTING OTA SERVER ═══");
  
  String url = String(firmwareURL);
  if (url.startsWith("http://")) url = url.substring(7);
  
  int slashIndex = url.indexOf('/');
  String hostPort = url.substring(0, slashIndex);
  String path = url.substring(slashIndex);
  
  String host;
  int port = 80;
  int colonIndex = hostPort.indexOf(':');
  if (colonIndex > 0) {
    host = hostPort.substring(0, colonIndex);
    port = hostPort.substring(colonIndex + 1).toInt();
  } else {
    host = hostPort;
  }
  
  Serial.println("Testing: " + host + ":" + String(port) + path);
  
  Client* testClient = nullptr;
  
  if (currentMode == MODE_WIFI && WiFi.status() == WL_CONNECTED) {
    testClient = new WiFiClient();
  } else if (currentMode == MODE_GSM && modem.isGprsConnected()) {
    testClient = new TinyGsmClient(modem);
  } else {
    Serial.println("✗ No active connection");
    return;
  }
  
  if (!testClient->connect(host.c_str(), port)) {
    Serial.println("✗ Cannot connect to server");
    delete testClient;
    return;
  }
  
  Serial.println("✓ Connected to server");
  
  // Send HEAD request to get headers only
  testClient->print(String("HEAD ") + path + " HTTP/1.1\r\n");
  testClient->print(String("Host: ") + host + "\r\n");
  testClient->print("Connection: close\r\n\r\n");
  
  delay(1000);
  
  Serial.println("\n--- SERVER RESPONSE ---");
  while (testClient->available()) {
    String line = testClient->readStringUntil('\n');
    Serial.println(line);
  }
  Serial.println("--- END RESPONSE ---\n");
  
  testClient->stop();
  delete testClient;
}

// Helper function to perform OTA via HTTP (works with both WiFi and GSM)
bool performHTTPUpdate(Client& client, const char* host, int port, const char* path) {
  Serial.println("→ Connecting to update server...");
  
  if (!client.connect(host, port)) {
    Serial.println("✗ Connection to server failed");
    return false;
  }
  
  Serial.println("✓ Connected to server");
  
  // Send HTTP GET request
  client.print(String("GET ") + path + " HTTP/1.1\r\n");
  client.print(String("Host: ") + host + "\r\n");
  client.print("Cache-Control: no-cache\r\n");
  client.print("Connection: close\r\n\r\n");
  
  // Wait for response
  unsigned long timeout = millis();
  while (client.available() == 0) {
    if (millis() - timeout > 30000) {
      Serial.println("✗ Server response timeout");
      client.stop();
      return false;
    }
    delay(10);
  }
  
  // Read headers
  bool headerEnd = false;
  int contentLength = -1;
  String line;
  int httpStatus = 0;
  int headerCount = 0;
  
  Serial.println("→ Reading HTTP headers...");
  Serial.println("========== RAW HEADERS ==========");
  timeout = millis();
  
  while (client.connected()) {
    if (client.available()) {
      line = client.readStringUntil('\n');
      
      // Show raw line with length
      Serial.print("  [");
      Serial.print(line.length());
      Serial.print("] ");
      Serial.println(line);
      
      line.trim();
      headerCount++;
      
      // Check for content length (case insensitive)
      String lineLower = line;
      lineLower.toLowerCase();
      if (lineLower.startsWith("content-length:")) {
        String lengthStr = line.substring(line.indexOf(':') + 1);
        lengthStr.trim();
        contentLength = lengthStr.toInt();
        Serial.println("    ✓ PARSED Content-Length: " + String(contentLength));
      }
      
      // Check for HTTP response code
      if (line.startsWith("HTTP/1.")) {
        httpStatus = line.substring(9, 12).toInt();
        Serial.println("    ✓ PARSED Status: " + String(httpStatus));
      }
      
      // Empty line marks end of headers
      if (line.length() == 0) {
        Serial.println("========== END HEADERS ==========");
        Serial.println("Total headers received: " + String(headerCount));
        headerEnd = true;
        break;
      }
    }
    
    // Timeout for reading headers
    if (millis() - timeout > 10000) {
      Serial.println("✗ Timeout reading headers after " + String(headerCount) + " lines");
      break;
    }
    delay(1);
  }
  
  // Summary
  Serial.println("\n--- HEADER SUMMARY ---");
  Serial.println("Headers complete: " + String(headerEnd ? "YES" : "NO"));
  Serial.println("HTTP Status: " + String(httpStatus));
  Serial.println("Content-Length: " + String(contentLength));
  Serial.println("----------------------\n");
  
  if (!headerEnd) {
    Serial.println("✗ Failed to read complete headers");
    client.stop();
    return false;
  }
  
  if (httpStatus != 200 && httpStatus != 0) {
    Serial.println("✗ Server returned error: " + String(httpStatus));
    client.stop();
    return false;
  }
  
  if (contentLength <= 0) {
    Serial.println("✗ Invalid or missing Content-Length header");
    Serial.println("  Server must provide Content-Length for OTA updates");
    Serial.println("  Received value: " + String(contentLength));
    client.stop();
    return false;
  }
  
  // Additional validation - firmware should be at least 100KB
  if (contentLength < 100000) {
    Serial.println("⚠ Warning: Firmware size seems too small (" + String(contentLength) + " bytes)");
    Serial.println("  Typical ESP32 firmware is 500KB-1MB+");
  }
  
  // Start update process
  Serial.println("→ Starting firmware update (" + String(contentLength) + " bytes)");
  
  if (!Update.begin(contentLength)) {
    Serial.println("✗ Not enough space for update");
    client.stop();
    return false;
  }
  
  // Write firmware data
  uint8_t buffer[512];
  int written = 0;
  int lastPercent = 0;
  
  while (written < contentLength) {
    int available = client.available();
    if (available > 0) {
      int toRead = min(available, (int)sizeof(buffer));
      int bytesRead = client.read(buffer, toRead);
      
      if (bytesRead > 0) {
        if (Update.write(buffer, bytesRead) != bytesRead) {
          Serial.println("✗ Write error");
          Update.abort();
          client.stop();
          return false;
        }
        written += bytesRead;
        
        // Print progress every 10%
        int percent = (written * 100) / contentLength;
        if (percent >= lastPercent + 10) {
          Serial.print("Progress: ");
          Serial.print(percent);
          Serial.println("%");
          lastPercent = percent;
        }
      }
    } else {
      delay(10);
    }
    
    // Timeout check
    if (millis() - timeout > 600000) {  // 10 minute timeout
      Serial.println("✗ Download timeout");
      Update.abort();
      client.stop();
      return false;
    }
  }
  
  client.stop();
  
  if (Update.end(true)) {
    Serial.println("✓ Update complete! Rebooting...");
    return true;
  } else {
    Serial.print("✗ Update error: ");
    Serial.println(Update.getError());
    return false;
  }
}

// Main OTA function that works in both modes
void performOTA() {
  Serial.println("═══ STARTING OTA UPDATE ═══");
  Serial.println("Current mode: " + String(currentMode == MODE_WIFI ? "WiFi" : "GSM"));
  Serial.println("Firmware URL: " + String(firmwareURL));
  
  // Parse URL (expecting format: http://host:port/path)
  String url = String(firmwareURL);
  
  // Remove "http://"
  if (url.startsWith("http://")) {
    url = url.substring(7);
  } else if (url.startsWith("https://")) {
    Serial.println("✗ HTTPS not supported, use HTTP");
    return;
  }
  
  // Extract host and path
  int slashIndex = url.indexOf('/');
  String hostPort = url.substring(0, slashIndex);
  String path = url.substring(slashIndex);
  
  // Extract host and port
  String host;
  int port = 80;
  int colonIndex = hostPort.indexOf(':');
  if (colonIndex > 0) {
    host = hostPort.substring(0, colonIndex);
    port = hostPort.substring(colonIndex + 1).toInt();
  } else {
    host = hostPort;
  }
  
  Serial.println("Host: " + host);
  Serial.println("Port: " + String(port));
  Serial.println("Path: " + path);
  
  bool success = false;
  
  if (currentMode == MODE_WIFI) {
    // Use WiFi client
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("✗ WiFi not connected");
      return;
    }
    
    Serial.println("→ Using WiFi for OTA update");
    WiFiClient client;
    success = performHTTPUpdate(client, host.c_str(), port, path.c_str());
    
  } else if (currentMode == MODE_GSM) {
    // Use GSM client
    if (!modem.isGprsConnected()) {
      Serial.println("✗ GPRS not connected");
      return;
    }
    
    Serial.println("→ Using GSM for OTA update");
    TinyGsmClient client(modem);
    success = performHTTPUpdate(client, host.c_str(), port, path.c_str());
  }
  
  if (success) {
    delay(1000);
    ESP.restart();
  } else {
    Serial.println("✗ OTA update failed");
  }
}

float sensor_pressao() {
  int valorSensor = analogRead(sensor);
  float tensao = (valorSensor * ADC_VOLTAGE) / 4095.0;
  
  // Calcula a pressão em bar (0.5 MPa = 5.0 bar)
  float pressao = ((tensao - voltagemMinima) * (pressaoMaxima - pressaoMinima) / 
                   (voltagemMaxima - voltagemMinima) + pressaoMinima);

  // AVISO: ESP32 lê até 3.3V, mas o sensor vai até 4.5V
  // Você só conseguirá medir até ~3.6 bar com ESP32 de 3.3V
  if (tensao < voltagemMinima) {
    //Serial.println("Pressão: Tensão abaixo do mínimo");
    return 0.0;
  } else if (tensao > ADC_VOLTAGE) {
    Serial.println("Pressão: Limite do ADC atingido (3.3V)");
    return (ADC_VOLTAGE - voltagemMinima) * (pressaoMaxima - pressaoMinima) / 
           (voltagemMaxima - voltagemMinima);
  } else {
    return pressao;
  }
}

float sensor_fluxo() {
  detachInterrupt(digitalPinToInterrupt(PINO_SENSOR));

  unsigned long intervalo_ms = millis() - tempo_antes;
  
  // Evita divisão por zero
  if (intervalo_ms == 0) {
    attachInterrupt(digitalPinToInterrupt(PINO_SENSOR), contarPulso, FALLING);
    return 0.0;
  }

  float tempo_segundos = intervalo_ms / 1000.0;
  
  // Calcula o fluxo em L/min
  // contador pulsos / FATOR_CALIBRACAO = litros
  // litros / tempo_segundos * 60 = L/min
  float fluxo = (contador / FATOR_CALIBRACAO) * (60.0 / tempo_segundos);
  
  // Calcula o volume em litros para este intervalo
  float volume = contador / FATOR_CALIBRACAO;
  
  volume_total += volume;
  fluxo_total = volume_total;

  contador = 0;
  tempo_antes = millis();

  attachInterrupt(digitalPinToInterrupt(PINO_SENSOR), contarPulso, FALLING);
  return fluxo;
}

float sensor_distancia() {
  // Limpa o trigger
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);
  
  // Envia pulso de 10µs
  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  // Lê o tempo de retorno do echo
  duration = pulseIn(echoPin, HIGH, 30000);  // Timeout de 30ms
  
  // Se timeout, retorna 0
  if (duration == 0) {
    return 0;
  }
  
  // Calcula a distância em cm
  distanceCm = duration * SOUND_SPEED / 2;
  return distanceCm;
}

// ===== SETUP =====
void setup() {
  SIM7080G.begin(115200, SERIAL_8N1, 44, 43);
  Serial.begin(115200);
  // Inicio dos sensores
  pinMode(PINO_SENSOR, INPUT_PULLUP);
  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);

  // Inicializa o sensor de fluxo
  attachInterrupt(digitalPinToInterrupt(PINO_SENSOR), contarPulso, FALLING);
  Serial.println("\n\n═══ ESP32 MQTT CLIENT ═══");

  if (!SPIFFS.begin(true)) {
    Serial.println("✗ SPIFFS failed");
    return;
  }

  // Get MAC address
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  char macFormatted[18];
  sprintf(macFormatted, "%02X:%02X:%02X:%02X:%02X:%02X",
          mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  Mac = String(macFormatted);

  mqtt_addr_str = "esp/" + Mac;
  mqtt_addr_subscription_str = mqtt_addr_str + "/config";

  device = "Device_" + Mac;

  Serial.println("Device ID: " + device);
  Serial.println("Starting in WiFi mode, will fallback to GSM if needed");

  modeStartTime = millis();
  currentMode = MODE_WIFI;
  if (currentMode == MODE_CONFIG) {
    configMode();
  }
  //currentMode = MODE_GSM;

  Serial.println("Device: " + device);
  Serial.println("Buffer size: " + String(BUFFER_SIZE) + " minutes (24 hours)");
  Serial.println("Reading sensors every 1s, averaging every 60s");

  // Create mutex for thread-safe buffer access
  bufferMutex = xSemaphoreCreateMutex();

  // Start sensor task on Core 1
  xTaskCreatePinnedToCore(
    sensorTask,    // Function
    "SensorTask",  // Name
    10000,         // Stack size
    NULL,          // Parameters
    1,             // Priority
    NULL,          // Task handle
    1              // Core 1
  );

  Serial.println("✓ Network task running on Core " + String(xPortGetCoreID()));

  attemptConnection();
}

// ===== LOOP =====
void loop() {
  manageConnection();
  publishBufferedData();
  delay(10);
}

/* ESPECIFICAÇÕES DOS SENSORES:
 * 
 * 1. Sensor de Pressão: HK2404 G1/4 0.5 MPa 5V
 *    - Faixa: 0 a 0.5 MPa (0 a 5.0 bar ou 0 a 72.5 PSI)
 *    - Saída: 0.5V a 4.5V linear
 *    - ATENÇÃO: ESP32 lê até 3.3V, então você só medirá até ~3.6 bar
 *    - Solução: Use um divisor de tensão ou módulo de nível lógico
 * 
 * 2. Sensor de Fluxo: YF-S403 G3/4 1-30 L/min
 *    - Faixa: 1 a 30 L/min
 *    - Calibração: ~5880 pulsos por litro (pode variar, calibre se necessário)
 *    - Fórmula: Flow (L/min) = Pulsos / Fator × 60 / Tempo(s)
 * 
 * 3. Sensor de Distância: RCWL-1670
 *    - Faixa: 2cm a 400cm
 *    - Tensão: 3-5V (compatível com ESP32)
 *    - Velocidade do som: 343 m/s (0.0343 cm/µs a 20°C)
 */
