/*
  ================================================
  Sistema Dual DHT11 + InfluxDB Cloud
  ================================================
  Sensor Control  (D7/GPIO13): Promedio 5 lecturas
  Sensor Observador (D6/GPIO12): Cada 1 minuto
  Publica directo a InfluxDB via HTTP
  ================================================
*/

#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <ESP8266HTTPClient.h>
#include <SimpleDHT.h>
#include <LittleFS.h>

// ===================================================
// CONFIGURACION WiFi
// ===================================================
const char* WIFI_SSID     = "2.4Personal-4839D";
const char* WIFI_PASSWORD = "mati200200";

// ===================================================
// CONFIGURACION InfluxDB Cloud
// ===================================================
const char* INFLUX_URL    = "https://us-east-1-1.aws.cloud2.influxdata.com";
const char* INFLUX_TOKEN  = "-hiUhGzOIyi3LG0oi3mwP_UTFJ3aZzlwpu62E69srFbg-TMW4LyDn7tZw94VGjpC7OgjT1g1WAllg9fB2J7kuw==";
const char* INFLUX_ORG    = "Universidad Nacional de Cordoba";
const char* INFLUX_BUCKET = "camara";

// ===================================================
// SENSORES
// ===================================================
#define SENSOR_CONTROL_PIN    13  // D7
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
// BUFFER CIRCULAR - SENSOR CONTROL
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
unsigned long ultimaLecturaControl     = 0;
unsigned long ultimaLecturaObservador  = 0;
unsigned long ultimaPublicacionControl = 0;
const unsigned long INTERVALO_CONTROL      = 5000;   // 5s
const unsigned long INTERVALO_OBSERVADOR   = 60000;  // 1 min
const unsigned long INTERVALO_PUB_CONTROL  = 30000;  // 30s

// ===================================================
// SETUP
// ===================================================
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== SISTEMA DUAL + InfluxDB Cloud ===");

  for (int i = 0; i < LECTURAS_PROMEDIO; i++) {
    tempsControl[i] = 0;
    humsControl[i]  = 0;
  }

  if (LittleFS.begin()) {
    Serial.println("v LittleFS listo");
    if (LittleFS.exists("/datos.csv")) LittleFS.remove("/datos.csv");
  }

  conectarWiFi();
}

// ===================================================
// LOOP
// ===================================================
void loop() {
  unsigned long ahora = millis();

  // Reconectar WiFi si se cae
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi caido, reconectando...");
    conectarWiFi();
  }

  // 1. Sensor control: lectura cada 5s
  if (ahora - ultimaLecturaControl >= INTERVALO_CONTROL) {
    leerSensorControl();
    ultimaLecturaControl = ahora;
  }

  // 2. Publicar control a InfluxDB cada 30s
  if (ahora - ultimaPublicacionControl >= INTERVALO_PUB_CONTROL) {
    if (lecturasControlValidas >= LECTURAS_PROMEDIO) {
      publicarInflux("control", tempControlPromedio, humControlPromedio);
    }
    ultimaPublicacionControl = ahora;
  }

  // 3. Sensor observador: cada 1 min
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
  IPAddress gw(192, 168, 100, 1);
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
    Serial.println(" v");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(" x Error WiFi");
  }
}

// ===================================================
// PUBLICAR A INFLUXDB
// sensor: "control" o "observador"
// ===================================================
void publicarInflux(const char* sensor, float temp, float hum) {
  if (WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();  // Sin validacion de certificado

  HTTPClient http;

  // Construir URL con org y bucket como parametros
  String url = String(INFLUX_URL) + "/api/v2/write?org=" + urlencode(INFLUX_ORG) + "&bucket=" + INFLUX_BUCKET + "&precision=s";

  http.begin(client, url);
  http.addHeader("Authorization", String("Token ") + INFLUX_TOKEN);
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.setTimeout(10000);

  // Line Protocol: measurement,tag=value field=value timestamp
  // Ejemplo: camara,sensor=control temp=28.5,hum=65.0
  String payload = "camara,sensor=" + String(sensor) +
                   " temp=" + String(temp, 1) +
                   ",hum=" + String(hum, 1);

  int code = http.POST(payload);

  if (code == 204) {
    Serial.printf("v InfluxDB [%s] Temp:%.1fC Hum:%.1f%%\n", sensor, temp, hum);
  } else {
    Serial.printf("x InfluxDB error %d: %s\n", code, http.getString().c_str());
  }

  http.end();
}

// ===================================================
// URL encode simple (para el nombre de la org)
// ===================================================
String urlencode(const char* str) {
  String encoded = "";
  for (int i = 0; str[i] != '\0'; i++) {
    char c = str[i];
    if (c == ' ') encoded += "%20";
    else encoded += c;
  }
  return encoded;
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

      Serial.printf("Control -> Temp:%.1fC Hum:%.1f%% PWM T:%d H:%d\n",
                    tempControlPromedio, humControlPromedio, pwmTemp, pwmHum);
    }
  } else {
    Serial.println("x Error DHT Control");
  }
}

// ===================================================
// SENSOR OBSERVADOR: lectura + publicacion
// ===================================================
void leerYPublicarObservador() {
  byte temp, hum;
  bool ok = false;
  int intentos = 0;

  while (!ok) {
    intentos++;
    if (sensorObservador.read(SENSOR_OBSERVADOR_PIN, &temp, &hum, NULL) == SimpleDHTErrSuccess) {
      ok = true;

      // Guardar CSV local
      File f = LittleFS.open("/datos.csv", "a");
      if (f) {
        f.printf("%lu,%d,%d\n", millis() / 1000, temp, hum);
        f.close();
      }

      // Publicar a InfluxDB
      publicarInflux("observador", (float)temp, (float)hum);

      Serial.printf("Observador (intento #%d) -> Temp:%dC Hum:%d%%\n", intentos, temp, hum);

    } else {
      Serial.printf("Observador intento #%d fallido...\n", intentos);
      delay(200);
    }
  }
}
