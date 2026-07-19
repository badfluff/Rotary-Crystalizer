#include <Arduino.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <FS.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <stdlib.h>

#include "LED.hpp"
#include "PWMController.hpp"
#include "PIDController.hpp"
#include "TemperatureController.hpp"
#include "Secrets.hpp"

// ===========================================================================
//  Configuration
// ===========================================================================

// --- Hardware pins ---
static const int RELAY_PIN = 19;   // Solid-state relay driving the hot plate
static const int ONEWIRE_PIN = 25; // DS18B20 data line
static const int DEBUG_LED_PIN = 2;

// If your SSR switches the heater ON when the control pin is LOW, set false.
static const bool RELAY_ACTIVE_HIGH = true;

// --- Network ---
static const char *WIFI_SSID = WIFI_SSID_STR;
static const char *WIFI_PASSWORD = WIFI_PASSWORD_STR;

// --- Temperature sensing ---
static const uint8_t SENSOR_RESOLUTION_BITS = 12;
static const unsigned long TEMP_CONVERSION_MS = 750; // 12-bit conversion time

// --- Heater output (slow PWM) ---
static const unsigned long HEATER_PWM_PERIOD_MS = 2000;

// --- Control tuning -------------------------------------------------------
// Gains are expressed as fractions of full heater power (output range 0..1).
//   Kp : how hard to push per degree of error. With the leadband removed the
//        PID now sees the full error to the target, so this is much smaller
//        than before. At 0.04, a 25 C error already commands full power, and
//        the P term falls below the ~15% steady-state hold well before the
//        setpoint (0.04 * 5 C = 20%, 0.04 * 3 C = 12%), which - together with
//        the derivative brake - lets power taper off ahead of arrival.
//   Ki : how fast to learn the steady-state hold power (per degree-second).
//        Only active within PID_INTEGRAL_BAND_C of the setpoint, so it no
//        longer winds up during the long heat-up.
//   Kd : brake against the tank's own rate of rise (per degree-per-second),
//        split into rising/falling gains for the plant's push/coast asymmetry.
static const float PID_KP = 0.04f;
static const float PID_KI = 0.0003f;
// Rising temperature needs a strong brake to kill overshoot; a falling
// temperature only needs a gentle nudge (the plant cools slowly on its own).
static const float PID_KD_RISING = 5.0f;
static const float PID_KD_FALLING = 5.0f;
// Rolling-window rate-of-change estimate (least-squares slope over this span).
static const float PID_DERIV_WINDOW_S = 30.0f;
// Degrees around the setpoint within which the integrator is allowed to learn.
static const float PID_INTEGRAL_BAND_C = 3.0f;



static const float DEFAULT_TARGET_C = 0.0f;

// Guard against absurd dt if the loop ever stalls.
static const float DT_MIN_S = 0.05f;
static const float DT_MAX_S = 5.0f;

// ===========================================================================
//  Globals
// ===========================================================================

LED debugLED(DEBUG_LED_PIN);
OneWire oneWire(ONEWIRE_PIN);
DallasTemperature sensors(&oneWire);
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

SlowPWM heater(RELAY_PIN, HEATER_PWM_PERIOD_MS, RELAY_ACTIVE_HIGH);
PIDController pid(PID_KP, PID_KI, PID_KD_RISING, 0.0f, 1.0f,
                 PID_DERIV_WINDOW_S, PID_INTEGRAL_BAND_C);
TemperatureController controller(pid);

// Shared state (single-threaded loop; web handlers only read/set scalars).
volatile float g_currentTempC = DEFAULT_TARGET_C;
volatile float g_targetTempC = DEFAULT_TARGET_C;
volatile float g_heatingPower = 0.0f; // 0..1
volatile float g_pTerm = 0.0f; // proportional contribution to power
volatile float g_dTerm = 0.0f; // derivative contribution to power
volatile float g_iTerm = 0.0f; // integral contribution to power

// Non-blocking temperature-conversion bookkeeping.
bool g_conversionPending = false;
unsigned long g_conversionRequestedAt = 0;
unsigned long g_lastSampleMillis = 0;
bool g_haveFirstSample = false;

// ===========================================================================
//  Temperature profile loading
// ===========================================================================

bool parseProfileFloat(const String &value, float &outValue) {
  char *end = nullptr;
  outValue = strtof(value.c_str(), &end);
  while (end != nullptr && *end == ' ') {
    ++end;
  }
  return end != value.c_str() && end != nullptr && *end == '\0' && isfinite(outValue);
}

bool loadProfile(const String &path, String &error) {
  File file = LittleFS.open(path, "r");
  if (!file || file.isDirectory()) {
    error = "Profile file not found";
    return false;
  }

  TemperatureController::ProfilePoint points[TemperatureController::MAX_PROFILE_POINTS];
  size_t pointCount = 0;
  while (file.available()) {
    String line = file.readStringUntil('\n');
    line.trim();
    if (line.isEmpty() || line.startsWith("#")) {
      continue;
    }

    const int comma = line.indexOf(',');
    if (comma < 0) {
      file.close();
      error = "Each profile line must be minutes,target_c";
      return false;
    }

    String minutesText = line.substring(0, comma);
    String targetText = line.substring(comma + 1);
    minutesText.trim();
    targetText.trim();
    if (pointCount == 0 && minutesText.equalsIgnoreCase("minutes")) {
      continue; // Optional CSV header.
    }

    float minutes;
    float targetC;
    if (!parseProfileFloat(minutesText, minutes) || !parseProfileFloat(targetText, targetC)) {
      file.close();
      error = "Profile contains an invalid temperature point";
      return false;
    }
    if (pointCount == TemperatureController::MAX_PROFILE_POINTS) {
      file.close();
      error = "Profile has too many points";
      return false;
    }
    points[pointCount++] = {minutes, targetC};
  }
  file.close();

  if (!controller.startProfile(points, pointCount, millis())) {
    error = "Profile times must be non-negative and strictly increasing";
    return false;
  }
  g_targetTempC = controller.target();
  return true;
}

bool validProfileName(const String &name) {
  if (!name.endsWith(".csv") || name.length() <= 4) {
    return false;
  }
  for (size_t i = 0; i < name.length(); ++i) {
    const char c = name[i];
    if (!(isAlphaNumeric(c) || c == '-' || c == '_' || c == '.')) {
      return false;
    }
  }
  return true;
}

// ===========================================================================
//  Network + web server
// ===========================================================================

void broadcastState() {
  const String payload = String("{\"temperature\":") + String(g_currentTempC, 2) +
                         String(",\"target\":") + String(g_targetTempC, 2) +
                         String(",\"heatingPower\":") + String(g_heatingPower, 4) +
                         String(",\"pTerm\":") + String(g_pTerm, 4) +
                         String(",\"dTerm\":") + String(g_dTerm, 4) +
                         String(",\"iTerm\":") + String(g_iTerm, 4) +
                         String("}");
  ws.textAll(payload);
}

void onWsEvent(AsyncWebSocket *server, AsyncWebSocketClient *client,
               AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.printf("WebSocket connected: client #%u\n", client->id());
    const String payload = String("{\"temperature\":") + String(g_currentTempC, 2) +
                           String(",\"target\":") + String(g_targetTempC, 2) +
                           String(",\"heatingPower\":") + String(g_heatingPower, 4) +
                           String(",\"pTerm\":") + String(g_pTerm, 4) +
                           String(",\"dTerm\":") + String(g_dTerm, 4) +
                           String(",\"iTerm\":") + String(g_iTerm, 4) +
                           String("}");
    client->text(payload);
  }
}

void setupNetwork() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    debugLED.flash(50);
    delay(10);
  }
  debugLED.low();

  Serial.println();
  Serial.print("WiFi connected. IP address: ");
  Serial.println(WiFi.localIP());
}

void setupServer() {
  if (!LittleFS.begin(true)) {
    Serial.println("Error mounting LittleFS");
    return;
  }

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/index.html", "text/html");
  });
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/style.css", "text/css");
  });
  server.on("/script.js", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(LittleFS, "/script.js", "text/javascript");
  });

  server.on("/temperature", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(g_currentTempC));
  });

  server.on("/heatingPower", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(g_heatingPower));
  });

  server.on("/targetTemp", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", String(g_targetTempC));
  });

  server.on("/profiles", HTTP_GET, [](AsyncWebServerRequest *request) {
    String payload = "[";
    File directory = LittleFS.open("/profiles");
    if (directory && directory.isDirectory()) {
      File file = directory.openNextFile();
      bool first = true;
      while (file) {
        String name = file.name();
        if (!file.isDirectory() && name.endsWith(".csv")) {
          const int slash = name.lastIndexOf('/');
          name = name.substring(slash + 1);
          if (!first) {
            payload += ',';
          }
          payload += '"';
          payload += name;
          payload += '"';
          first = false;
        }
        file = directory.openNextFile();
      }
    }
    payload += ']';
    request->send(200, "application/json", payload);
  });

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

  server.on("/setTargetTemperature", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("target", true)) {
      const float target = request->getParam("target", true)->value().toFloat();
      g_targetTempC = target;
      controller.setTarget(target); // ramp handles the transition smoothly
      request->send(200, "text/plain", String(target));
    } else {
      request->send(400, "text/plain", "Missing 'target' parameter");
    }
  });

  server.on("/startProfile", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("profile", true)) {
      request->send(400, "text/plain", "Missing 'profile' parameter");
      return;
    }

    const String name = request->getParam("profile", true)->value();
    if (!validProfileName(name)) {
      request->send(400, "text/plain", "Invalid profile filename");
      return;
    }

    String error;
    if (!loadProfile("/profiles/" + name, error)) {
      request->send(400, "text/plain", error);
      return;
    }
    request->send(200, "text/plain", "Profile started: " + name);
  });

  server.on("/stopProfile", HTTP_POST, [](AsyncWebServerRequest *request) {
    controller.stopProfile();
    g_targetTempC = controller.target();
    request->send(200, "text/plain", "Profile stopped");
  });

  server.begin();
}

// ===========================================================================
//  Temperature acquisition (non-blocking) and control
// ===========================================================================

// Kicks off conversions and, once ready, returns true with a fresh reading.
bool readTemperature(unsigned long now, float &outTempC) {
  if (!g_conversionPending) {
    sensors.requestTemperatures();
    g_conversionPending = true;
    g_conversionRequestedAt = now;
    return false;
  }

  if (now - g_conversionRequestedAt < TEMP_CONVERSION_MS) {
    return false;
  }

  g_conversionPending = false;
  const float tempC = sensors.getTempCByIndex(0);
  if (tempC == DEVICE_DISCONNECTED_C) {
    return false; // ignore bad reads, keep last known value
  }
  outTempC = tempC;
  return true;
}

void runControlStep(unsigned long now, float tempC) {
  float dt;
  if (!g_haveFirstSample) {
    controller.reset(tempC);
    g_haveFirstSample = true;
    dt = 0.0f;
  } else {
    dt = (now - g_lastSampleMillis) / 1000.0f;
    dt = constrain(dt, DT_MIN_S, DT_MAX_S);
  }
  g_lastSampleMillis = now;

  const float power = controller.update(tempC, dt, now);

  g_currentTempC = tempC;
  g_targetTempC = controller.target();
  g_heatingPower = power;
  g_pTerm = controller.pid().pTerm();
  g_dTerm = controller.pid().dTerm();
  g_iTerm = controller.pid().iTerm();
  heater.setDuty(power);
  broadcastState();
}

// ===========================================================================
//  Arduino entry points
// ===========================================================================

void setup() {
  Serial.begin(115200);

  heater.off();
  controller.setTarget(DEFAULT_TARGET_C);
  g_targetTempC = DEFAULT_TARGET_C;

  // Asymmetric derivative braking: strong on the way up, gentle on the way down.
  controller.pid().setDerivativeGains(PID_KD_RISING, PID_KD_FALLING);

  setupNetwork();
  setupServer();

  sensors.begin();
  sensors.setResolution(SENSOR_RESOLUTION_BITS);
  sensors.setWaitForConversion(false); // non-blocking conversions
}

void loop() {
  const unsigned long now = millis();

  debugLED.flash(1500);

  float tempC;
  if (readTemperature(now, tempC)) {
    runControlStep(now, tempC);
  }

  // Realise the requested heater power via slow PWM every loop.
  heater.update();

  yield();
}