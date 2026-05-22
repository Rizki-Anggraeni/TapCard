#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>

const char* ssid     = "";      
const char* password = "";  

const char* API_BASE_URL = "https://iot.airashi.biz.id/api/v1";
const char* DEVICE_NODE_ID = "";
const char* DEVICE_API_KEY = "YOUR_DEVICE_API_KEY";
const char* DEFAULT_DEVICE_NAME = "TapCard ESP32";

#define SS_PIN        5   // SDA RFID  -> G5
#define RST_PIN      22   // RST RFID  -> G22
#define PIN_BUZZER    2   // Positif   -> G2


#define PIN_LED_HIJAU 14  // Sukses    -> G14
#define PIN_LED_MERAH 15  // Gagal     -> G15
#define PIN_SERVO    13   // Sinyal    -> G13

MFRC522 rfid(SS_PIN, RST_PIN);
Servo palangPintu;
WiFiClientSecure secureClient;
Preferences preferences;
String deviceNodeId;
String deviceApiKey;
String currentDeviceMode = "unknown";
unsigned long lastHeartbeatAt = 0;
const unsigned long HEARTBEAT_INTERVAL_MS = 60000;
unsigned long lastRegisterBlink = 0;
bool registerLedState = false;
const unsigned long REGISTER_BLINK_INTERVAL = 500;

// Konek Wi-Fi
void connectToWiFi() {
  Serial.print("[WIFI] Menghubungkan ke ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);

  int timeoutCounter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    timeoutCounter++;
    if(timeoutCounter > 20) { 
      Serial.println("\n[WIFI] Gagal konek, mencoba ulang...");
      WiFi.begin(ssid, password);
      timeoutCounter = 0;
    }
  }
  Serial.println("\n[WIFI] Berhasil Terkoneksi!");
  Serial.print("[WIFI] IP Address: ");
  Serial.println(WiFi.localIP());
}

String buildApiUrl(const char* path) {
  String base = String(API_BASE_URL);
  if (base.endsWith("/")) {
    base.remove(base.length() - 1);
  }
  String p = String(path);
  if (!p.startsWith("/")) {
    p = "/" + p;
  }
  return base + p;
}

void configureHttpClient(HTTPClient& http) {
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(5);
}

String getHardwareId() {
  uint64_t mac = ESP.getEfuseMac();
  char hardwareId[32];
  snprintf(hardwareId, sizeof(hardwareId), "ESP32-%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(hardwareId);
}

String getDeviceName() {
  return String(DEFAULT_DEVICE_NAME) + " " + getHardwareId().substring(6);
}

bool loadDeviceCredentials() {
  preferences.begin("tapcard", false);
  deviceNodeId = preferences.getString("node_id", "");
  deviceApiKey = preferences.getString("api_key", "");
  return deviceNodeId.length() > 0 && deviceApiKey.length() > 0;
}

void saveDeviceCredentials(const String& nodeId, const String& apiKey) {
  preferences.putString("node_id", nodeId);
  preferences.putString("api_key", apiKey);
  deviceNodeId = nodeId;
  deviceApiKey = apiKey;
}

bool parseStandardResponse(const String& body, String& action, String& message, String& deviceMode) {
  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, body);
  if (error) {
    Serial.print("[HTTP] Gagal parse JSON: ");
    Serial.println(error.c_str());
    return false;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  action = data["action"] | "";
  message = data["message"] | doc["message"] | "";
  deviceMode = data["device_mode"] | "";
  if (deviceMode.length() == 0) {
    deviceMode = data["mode"] | "";
  }
  return true;
}

bool provisionDevice() {
  if (deviceNodeId.length() > 0 && deviceApiKey.length() > 0) {
    return true;
  }

  HTTPClient http;
  secureClient.setInsecure();
  configureHttpClient(http);

  String url = buildApiUrl("/devices/provision");
  DynamicJsonDocument payload(256);
  payload["hardware_id"] = getHardwareId();
  payload["device_name"] = getDeviceName();
  payload["provision_key"] = DEVICE_API_KEY;

  String requestBody;
  serializeJson(payload, requestBody);

  Serial.println("[PROVISION] Mengirim hardware ID ke API...");
  Serial.println("[PROVISION] Hardware ID: " + getHardwareId());
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  int httpResponseCode = http.POST(requestBody);
  if (httpResponseCode <= 0) {
    Serial.print("[PROVISION] Gagal tersambung ke web server! Error: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
    http.end();
    return false;
  }

  String responseBody = http.getString();
  Serial.println("[PROVISION] Status Code: " + String(httpResponseCode));
  Serial.println("[PROVISION] Balasan Web: " + responseBody);

  DynamicJsonDocument doc(1024);
  DeserializationError error = deserializeJson(doc, responseBody);
  if (error) {
    Serial.print("[PROVISION] Gagal parse JSON: ");
    Serial.println(error.c_str());
    http.end();
    return false;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  String nodeId = data["node_id"] | "";
  String apiKey = data["api_key"] | "";

  http.end();

  if (nodeId.length() == 0 || apiKey.length() == 0) {
    Serial.println("[PROVISION] Response tidak berisi node_id/api_key.");
    return false;
  }

  saveDeviceCredentials(nodeId, apiKey);
  Serial.println("[PROVISION] Device berhasil diprovision.");
  Serial.println("[PROVISION] Node ID: " + deviceNodeId);
  return true;
}

void sendHeartbeat() {
  if (deviceNodeId.length() == 0 || deviceApiKey.length() == 0) {
    return;
  }

  HTTPClient http;
  secureClient.setInsecure();
  configureHttpClient(http);

  String url = buildApiUrl("/devices/me/status");
  Serial.println("[HEARTBEAT] Cek status device...");
  http.begin(secureClient, url);
  http.addHeader("X-API-Key", deviceApiKey);
  http.addHeader("X-Node-ID", deviceNodeId);
  http.addHeader("X-Hardware-ID", getHardwareId());

  int code = http.GET();
  if (code > 0) {
    String responseBody = http.getString();
    String action, message, deviceMode;
    if (parseStandardResponse(responseBody, action, message, deviceMode)) {
      if (deviceMode.length() > 0) {
        if (currentDeviceMode != deviceMode) {
          Serial.println("[HEARTBEAT] Device mode changed: " + deviceMode);
        }
        currentDeviceMode = deviceMode;
      }
      Serial.println("[HEARTBEAT] OK: " + message);
      Serial.println("[HEARTBEAT] Device mode: " + currentDeviceMode);
    }
  } else {
    Serial.print("[HEARTBEAT] Gagal: ");
    Serial.println(http.errorToString(code).c_str());
  }

  http.end();
}

bool sendCardTap(const String& unixId, String& actionOut, String& messageOut, String& deviceModeOut) {
  if (deviceNodeId.length() == 0 || deviceApiKey.length() == 0) {
    return false;
  }

  HTTPClient http;
  secureClient.setInsecure();
  configureHttpClient(http);

  String url = buildApiUrl("/card/tap");
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", deviceApiKey);
  http.addHeader("X-Node-ID", deviceNodeId);
  http.addHeader("X-Hardware-ID", getHardwareId());

  DynamicJsonDocument payload(128);
  payload["unix_id"] = unixId;
  String requestBody;
  serializeJson(payload, requestBody);

  Serial.println("[HTTP] POST " + url);
  int httpResponseCode = http.POST(requestBody);

  bool success = false;
  if (httpResponseCode > 0) {
    String responseBody = http.getString();
    Serial.println("[HTTP] Status Code: " + String(httpResponseCode));
    Serial.println("[HTTP] Balasan Web: " + responseBody);

    // Parse full response to extract member/pending when available
    DynamicJsonDocument doc(2048);
    DeserializationError derr = deserializeJson(doc, responseBody);
    if (derr) {
      Serial.print("[HTTP] Gagal parse JSON: ");
      Serial.println(derr.c_str());
      http.end();
      return false;
    }
    JsonObject data = doc["data"].as<JsonObject>();
    actionOut = data["action"] | "";
    messageOut = data["message"] | "";
    deviceModeOut = data["device_mode"] | "";

    // If registered, try to verify pending/member match and send confirmation
    if (actionOut == "registered") {
      String memberId = "";
      String memberName = "";
      String memberPhone = "";
      String pendingName = "";
      String pendingPhone = "";

      if (data.containsKey("member")) {
        JsonObject m = data["member"].as<JsonObject>();
        memberId = m["id"] | "";
        memberName = m["name"] | "";
        memberPhone = m["phone"] | "";
      }
      if (data.containsKey("pending")) {
        JsonObject p = data["pending"].as<JsonObject>();
        pendingName = p["name"] | "";
        pendingPhone = p["phone"] | "";
      }

      bool verified = true;
      if (pendingName.length() > 0) {
        String pcopy = pendingName;
        String mcopy = memberName;
        pcopy.toLowerCase();
        mcopy.toLowerCase();
        verified = (pcopy == mcopy);
      }
      if (verified && pendingPhone.length() > 0) {
        // compare digits only
        String pnorm = pendingPhone;
        String mnorm = memberPhone;
        for (int i = pnorm.length() - 1; i >= 0; i--) if (!isDigit(pnorm[i])) pnorm.remove(i,1);
        for (int i = mnorm.length() - 1; i >= 0; i--) if (!isDigit(mnorm[i])) mnorm.remove(i,1);
        verified = (pnorm == mnorm);
      }

      if (verified && memberId.length() > 0) {
        // Send confirmation to server
        String confirmUrl = buildApiUrl("/card/confirm");
        HTTPClient http2;
        configureHttpClient(http2);
        http2.begin(secureClient, confirmUrl);
        http2.addHeader("Content-Type", "application/json");
        http2.addHeader("X-API-Key", deviceApiKey);
        http2.addHeader("X-Node-ID", deviceNodeId);
        http2.addHeader("X-Hardware-ID", getHardwareId());

        DynamicJsonDocument payload2(256);
        payload2["unix_id"] = unixId;
        payload2["member_id"] = memberId;
        String body2;
        serializeJson(payload2, body2);
        Serial.println("[CONFIRM] POST " + confirmUrl + " payload: " + body2);
        int rc2 = http2.POST(body2);
        if (rc2 > 0) {
          String resp2 = http2.getString();
          Serial.println("[CONFIRM] Status " + String(rc2) + " resp: " + resp2);
        } else {
          Serial.println("[CONFIRM] Failed to POST confirmation: " + http2.errorToString(rc2));
        }
        http2.end();
      }

    }

    success = true;
  } else {
    Serial.print("[HTTP] Gagal tersambung ke web server! Error: ");
    Serial.println(http.errorToString(httpResponseCode).c_str());
  }

  http.end();
  return success;
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();
  secureClient.setInsecure();
  loadDeviceCredentials();
  if (deviceNodeId.length() == 0 && String(DEVICE_NODE_ID).length() > 0) {
    saveDeviceCredentials(DEVICE_NODE_ID, DEVICE_API_KEY);
  }
  
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_HIJAU, OUTPUT);
  pinMode(PIN_LED_MERAH, OUTPUT);
  
  digitalWrite(PIN_BUZZER, LOW);
  digitalWrite(PIN_LED_HIJAU, LOW);
  digitalWrite(PIN_LED_MERAH, LOW);
  
  palangPintu.setPeriodHertz(50);
  palangPintu.attach(PIN_SERVO, 500, 2400);
  palangPintu.write(0); 

  connectToWiFi();
  if (!provisionDevice()) {
    Serial.println("[PROVISION] Menunggu provisioning berhasil sebelum melanjutkan.");
  }
  sendHeartbeat();
  lastHeartbeatAt = millis();

  Serial.println("Tapcard untuk akses...");
}

void loop() {
  digitalWrite(PIN_LED_HIJAU, LOW);
  digitalWrite(PIN_LED_MERAH, LOW);
  digitalWrite(PIN_BUZZER, LOW);

  // Proteksi jika Wi-Fi putus
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  if (deviceNodeId.length() == 0 || deviceApiKey.length() == 0) {
    if (provisionDevice()) {
      sendHeartbeat();
      lastHeartbeatAt = millis();
    } else {
      delay(2000);
      return;
    }
  }

  if (millis() - lastHeartbeatAt >= HEARTBEAT_INTERVAL_MS) {
    sendHeartbeat();
    lastHeartbeatAt = millis();
  }

  // Show register mode indicator (non-blocking blink)
  if (currentDeviceMode == "register") {
    unsigned long now = millis();
    if (now - lastRegisterBlink >= REGISTER_BLINK_INTERVAL) {
      lastRegisterBlink = now;
      registerLedState = !registerLedState;
      digitalWrite(PIN_LED_HIJAU, registerLedState ? HIGH : LOW);
    }
  } else {
    // ensure indicator off when not in register mode
    digitalWrite(PIN_LED_HIJAU, LOW);
  }

  // Standby nunggu kartu ditempel
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial()) return;

  String uidString = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uidString += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uidString += String(rfid.uid.uidByte[i], HEX);
    if (i < rfid.uid.size - 1) uidString += " ";
  }
  uidString.toUpperCase();
  
  Serial.println("------------------------------------------------");
  Serial.println("[SCAN] Kartu Terdeteksi! UID: " + uidString);

  String action;
  String message;
  String deviceMode;
  bool apiOk = sendCardTap(uidString, action, message, deviceMode);

  if (apiOk) {
    if (deviceMode.length() > 0) {
      currentDeviceMode = deviceMode;
    }

    if (action == "granted") {
      Serial.println("[STATUS] VALID! Akses Diterima.");
      digitalWrite(PIN_LED_HIJAU, HIGH);
      
      for (int i = 0; i < 3; i++) {
        digitalWrite(PIN_BUZZER, HIGH); delay(120);
        digitalWrite(PIN_BUZZER, LOW);  delay(120);
      }
      
      Serial.println("[SERVO] Membuka Kunci...");
      for (int pos = 0; pos <= 90; pos += 1) { 
        palangPintu.write(pos);
        delay(20); 
      }
      
      Serial.println("[SERVO] Menahan posisi terbuka 5 detik...");
      delay(5000); 
      
      Serial.println("[SERVO] Mengunci Kembali...");
      for (int pos = 90; pos >= 0; pos -= 1) { 
        palangPintu.write(pos);
        delay(20); 
      }
      
      digitalWrite(PIN_LED_HIJAU, LOW);
    } else if (action == "registered") {
      Serial.println("[STATUS] KARTU TERDAFTAR / MEMBER DIAKTIFKAN.");
      if (currentDeviceMode == "register") {
        // Registration-specific feedback: short double-beep and brighter flash
        for (int i = 0; i < 2; i++) {
          digitalWrite(PIN_LED_HIJAU, HIGH);
          digitalWrite(PIN_BUZZER, HIGH);
          delay(200);
          digitalWrite(PIN_BUZZER, LOW);
          digitalWrite(PIN_LED_HIJAU, LOW);
          delay(150);
        }
        Serial.println("[REGISTER] Device was in register mode; tap created member.");
      } else {
        digitalWrite(PIN_LED_HIJAU, HIGH);
        digitalWrite(PIN_BUZZER, HIGH);
        delay(300);
        digitalWrite(PIN_BUZZER, LOW);
        delay(300);
        digitalWrite(PIN_LED_HIJAU, LOW);
      }
    } else {
      Serial.println("[STATUS] KARTU BELUM TERDAFTAR / AKSES DITOLAK!");
      digitalWrite(PIN_LED_MERAH, HIGH);
      
      digitalWrite(PIN_BUZZER, HIGH);
      delay(2000); 
      digitalWrite(PIN_BUZZER, LOW);
      
      digitalWrite(PIN_LED_MERAH, LOW);
    }
  } else {
    Serial.println("[HTTP] Tap request gagal diproses oleh API.");
    digitalWrite(PIN_LED_MERAH, HIGH); digitalWrite(PIN_BUZZER, HIGH);
    delay(500);
    digitalWrite(PIN_LED_MERAH, LOW);  digitalWrite(PIN_BUZZER, LOW);
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
}