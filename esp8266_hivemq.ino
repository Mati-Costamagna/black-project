/*
  ================================================
  Sistema Dual DHT11 + HiveMQ MQTT
  ================================================
  Sensor Control  (D7/GPIO13): Promedio 5 lecturas → PWM
  Sensor Observador (D6/GPIO12): Cada 1 minuto → Publica a HiveMQ
  ================================================
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SimpleDHT.h>
#include <LittleFS.h>
#include <ArduinoJson.h>    // Instalar: ArduinoJson by Benoit Blanchon
#include <time.h>

// ===================================================
// 🔧 CONFIGURACIÓN — MODIFICAR SEGÚN TU ENTORNO
// ===================================================

// WiFi
const char* WIFI_SSID     = "2.4Personal-4839D";
const char* WIFI_PASSWORD = "mati200200";

// HiveMQ Cloud — Creá tu cluster gratis en https://www.hivemq.com/mqtt-cloud-broker/
// Formato del host: xxxxxxxxxxxxxxxx.s1.eu.hivemq.cloud
const char* MQTT_HOST     = "48cfc0d4707d4ab1888a91291ab85110.s1.eu.hivemq.cloud";
const int   MQTT_PORT     = 8883;   // TLS/SSL
const char* MQTT_USER     = "publisher";
const char* MQTT_PASS     = "BlackProject1";
const char* MQTT_CLIENT   = "ESP8266_Camara";

// const char* MQTT_HOST = "broker.hivemq.com";  // Broker público sin auth
// const int   MQTT_PORT = 1883;                  // Sin TLS


// Tópicos MQTT
const char* TOPIC_CONTROL    = "camara/control";      // Promedio control
const char* TOPIC_OBSERVADOR = "camara/observador";   // Lectura cada 1 min
const char* TOPIC_PWM        = "camara/pwm";          // Valores PWM
const char* TOPIC_STATUS     = "camara/status";       // Online/Offline

// ===================================================
// SENSORES
// ===================================================
#define SENSOR_CONTROL_PIN   13   // D7
#define SENSOR_OBSERVADOR_PIN 12  // D6

SimpleDHT11 sensorControl;
SimpleDHT11 sensorObservador;

// ===================================================
// PWM
// ===================================================
int pwmTemp = 0;
int pwmHum  = 0;
int pwm5V   = 1023;

// ===================================================
// BUFFER CIRCULAR — SENSOR CONTROL
// ===================================================
#define LECTURAS_PROMEDIO 5
float tempsControl[LECTURAS_PROMEDIO];
float humsControl[LECTURAS_PROMEDIO];
int indiceControl = 0;
int lecturasControlValidas = 0;
float tempControlPromedio = 0;
float humControlPromedio  = 0;

// ===================================================
// TIMING
// ===================================================
unsigned long ultimaLecturaControl    = 0;
unsigned long ultimaLecturaObservador = 0;
unsigned long ultimaPublicacionControl = 0;
const unsigned long INTERVALO_CONTROL     = 5000;   // 5s
const unsigned long INTERVALO_OBSERVADOR  = 60000;  // 1 min
const unsigned long INTERVALO_PUB_CONTROL = 30000;  // Publica control cada 30s

// ===================================================
// OBJETOS MQTT
// ===================================================
WiFiClientSecure wifiClient;
PubSubClient mqttClient(wifiClient);


void sincronizarHora() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Sincronizando hora NTP...");
  time_t now = time(nullptr);
  int intentos = 0;
  while (now < 8 * 3600 * 2 && intentos < 40) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    intentos++;
  }
  Serial.println(" ✓");
}


// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SISTEMA DUAL + HiveMQ MQTT ===");

  // Inicializar arrays
  for (int i = 0; i < LECTURAS_PROMEDIO; i++) {
    tempsControl[i] = 0;
    humsControl[i]  = 0;
  }

  // LittleFS (log local opcional)
  if (LittleFS.begin()) {
    Serial.println("✓ LittleFS listo");
    if (LittleFS.exists("/datos.csv")) {
      LittleFS.remove("/datos.csv");
    }
  }

  // WiFi
  conectarWiFi();

  // MQTT — sin verificación de certificado (simplificado)
  // Para producción, cargá el certificado raíz de HiveMQ
  sincronizarHora();
  wifiClient.setInsecure();
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setKeepAlive(60);
  mqttClient.setBufferSize(512);

  conectarMQTT();
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  unsigned long ahora = millis();

  // Mantener conexión MQTT
  if (!mqttClient.connected()) {
    conectarMQTT();
  }
  mqttClient.loop();

  // 1. Sensor control: lectura cada 5s
  if (ahora - ultimaLecturaControl >= INTERVALO_CONTROL) {
    leerSensorControl();
    ultimaLecturaControl = ahora;
  }

  // 2. Publicar control al broker cada 30s (si hay promedio válido)
  if (ahora - ultimaPublicacionControl >= INTERVALO_PUB_CONTROL) {
    if (lecturasControlValidas >= LECTURAS_PROMEDIO) {
      publicarControl();
    }
    ultimaPublicacionControl = ahora;
  }

  // 3. Sensor observador: lectura + publicación cada 1 min
  if (ahora - ultimaLecturaObservador >= INTERVALO_OBSERVADOR) {
    leerYPublicarObservador();
    ultimaLecturaObservador = ahora;
  }
}

// ===================================================
// WiFi
// ===================================================
void conectarWiFi() {
  Serial.print("Conectando a WiFi...");
  IPAddress ip(192, 168, 100, 202);
  IPAddress gw(192, 168, 1, 1);
  IPAddress sn(255, 255, 255, 0);
  IPAddress dns(8, 8, 8, 8);

  WiFi.config(ip, gw, sn, dns);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int t = 0;
  while (WiFi.status() != WL_CONNECTED && t < 100) {
    delay(500);
    Serial.print(".");
    t++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(" ✓");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" ✗ Error WiFi");
  }
}

// ===================================================
// MQTT — Conexión con LWT (Last Will)
// ===================================================
void conectarMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi no conectado, abortando MQTT");
    return;
  }
  int reintentos = 0;
  while (!mqttClient.connected() && reintentos < 5) {
    Serial.print("Conectando a HiveMQ...");

    bool ok = mqttClient.connect(
      MQTT_CLIENT,
      MQTT_USER,
      MQTT_PASS,
      TOPIC_STATUS,   // LWT topic
      1,              // LWT QoS
      true,           // LWT retain
      "{\"online\":false}"  // LWT payload
    );

    if (ok) {
      Serial.println(" ✓");
      // Publicar estado online
      mqttClient.publish(TOPIC_STATUS, "{\"online\":true}", true);
    } else {
      Serial.print(" ✗ rc=");
      Serial.print(mqttClient.state());
      Serial.println(" reintentando en 5s...");
      delay(5000);
      reintentos++;
    }
  }
}

// ===================================================
// SENSOR CONTROL: buffer + promedio + PWM
// ===================================================
void leerSensorControl() {
  byte temp, hum;

  if (sensorControl.read(SENSOR_CONTROL_PIN, &temp, &hum, NULL) == SimpleDHTErrSuccess) {
    tempsControl[indiceControl] = temp;
    humsControl[indiceControl]  = hum;
    indiceControl++;
    if (indiceControl >= LECTURAS_PROMEDIO) indiceControl = 0;
    if (lecturasControlValidas < LECTURAS_PROMEDIO) lecturasControlValidas++;

    if (lecturasControlValidas >= LECTURAS_PROMEDIO) {
      float st = 0, sh = 0;
      for (int i = 0; i < LECTURAS_PROMEDIO; i++) {
        st += tempsControl[i];
        sh += humsControl[i];
      }
      tempControlPromedio = st / LECTURAS_PROMEDIO;
      humControlPromedio  = sh / LECTURAS_PROMEDIO;

      pwmTemp = constrain((int)(tempControlPromedio * 4 + 254), 0, 1023);
      pwmHum  = constrain((int)(humControlPromedio  * 4 + 232), 0, 1023);

      Serial.printf("📊 Control → Temp: %.1f°C | Hum: %.1f%% | PWM T:%d H:%d\n",
                    tempControlPromedio, humControlPromedio, pwmTemp, pwmHum);
    }
  } else {
    Serial.println("❌ Error DHT Control");
  }
}

// ===================================================
// PUBLICAR CONTROL → HiveMQ
// ===================================================
void publicarControl() {
  if (!mqttClient.connected()) return;

  // Payload JSON control
  StaticJsonDocument<200> docControl;
  docControl["temp"]     = serialized(String(tempControlPromedio, 1));
  docControl["hum"]      = serialized(String(humControlPromedio, 1));
  docControl["ts"]       = millis() / 1000;

  char bufControl[200];
  serializeJson(docControl, bufControl);
  mqttClient.publish(TOPIC_CONTROL, bufControl, false);

  // Payload JSON PWM
  StaticJsonDocument<200> docPwm;
  docPwm["temp"] = pwmTemp;
  docPwm["hum"]  = pwmHum;
  docPwm["v5"]   = pwm5V;

  char bufPwm[200];
  serializeJson(docPwm, bufPwm);
  mqttClient.publish(TOPIC_PWM, bufPwm, false);

  Serial.println("📤 Control publicado a HiveMQ");
}

// ===================================================
// SENSOR OBSERVADOR → lectura + publicación
// ===================================================
void leerYPublicarObservador() {
  byte temp, hum;
  bool ok = false;
  int intentos = 0;

  while (!ok) {
    intentos++;
    if (sensorObservador.read(SENSOR_OBSERVADOR_PIN, &temp, &hum, NULL) == SimpleDHTErrSuccess) {
      ok = true;

      unsigned long ts = millis() / 1000;

      // Guardar en CSV local
      File f = LittleFS.open("/datos.csv", "a");
      if (f) {
        f.printf("%lu,%d,%d\n", ts, temp, hum);
        f.close();
      }

      // Publicar a HiveMQ
      if (mqttClient.connected()) {
        StaticJsonDocument<200> doc;
        doc["temp"] = temp;
        doc["hum"]  = hum;
        doc["ts"]   = ts;

        char buf[200];
        serializeJson(doc, buf);
        mqttClient.publish(TOPIC_OBSERVADOR, buf, false);

        Serial.printf("✅ Observador (intento #%d) → Temp:%d°C Hum:%d%% → HiveMQ OK\n",
                      intentos, temp, hum);
      } else {
        Serial.println("⚠️ Observador: lectura OK pero MQTT desconectado");
      }

    } else {
      Serial.printf("⏳ Observador intento #%d fallido...\n", intentos);
      delay(200);
    }
  }
}
