/**************************************************************************************************
 * Multi-Device Fingerprint Attendance System
 * Version: 1.8
 **************************************************************************************************/

// --- LIBRARY INCLUSIONS ---
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LiquidCrystal.h>
#include <Adafruit_Fingerprint.h>
#include <RTClib.h>
#include <SD.h>
#include <SPI.h>
#include "base64.hpp"

// --- FIXED DEFINITIONS FOR FINGERPRINT LIBRARY COMPATIBILITY ---
#define FINGERPRINT_DOWNCHAR 0x09 
#define FINGERPRINT_TEMPLATE_SIZE 512

// --- DEVICE CONFIGURATION ---
const uint8_t ROOM_ID = 22; 

// --- HARDWARE PIN DEFINITIONS ---
const uint8_t rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
const uint8_t FINGERPRINT_RX = 16, FINGERPRINT_TX = 17;
const uint8_t BUTTON_PIN1 = 34;
const uint8_t BUTTON_PIN2 = 35;
const uint8_t SD_CS_PIN = 5;

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.6:7069"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600 * 2
#define DAYLIGHT_OFFSET_SEC 0
#define OFFLINE_LOG_FILE "/attendance_log.txt"

// --- API ENDPOINTS ---
#define ATTENDANCE_ENDPOINT "/api/SensorData/attendance"
#define FP_ENROLL_ENDPOINT "/api/SensorData/enroll"
#define FP_ALL_ENDPOINT "/api/SensorData/all"
#define FP_LAST_ID_ENDPOINT "/api/SensorData/last-id"

// --- CONSTANTS AND TIMERS ---
const uint32_t LONG_PRESS_DELAY = 1500;
const uint32_t SLEEP_TIMEOUT = 30000;
const int MAX_HTTP_RETRIES = 5;

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
RTC_DS3231 rtc;

// --- STATE MANAGEMENT ---
uint32_t lastActivityTime = 0;
bool justWokeUp = false;

// --- FUNCTION PROTOTYPES ---
void displayMessage(String, String, int);
void handleButtons();
void setupWiFi(bool);
bool postJsonToServer(const String&, const String&);
bool logAttendanceToServer(uint16_t, uint8_t, time_t);
uint16_t fetchLastIdFromServer();
void scanForFingerprint();
void enrollNewFingerprint();
void syncFingerprintsFromServer();
void syncOfflineLogs();
void forceSync();
void setupModules();
void setupRtcAndSyncTime();
void logAttendanceOffline(uint16_t, uint8_t, time_t);
void showMainMenu();
void enterLightSleep();
uint8_t uploadTemplate(uint8_t*, uint16_t);

/**************************************************************************************************
 * uploadTemplate
 **************************************************************************************************/
uint8_t uploadTemplate(uint8_t* template_buffer, uint16_t template_size) {
    uint8_t command_payload[] = {FINGERPRINT_DOWNCHAR, 1}; 
    Adafruit_Fingerprint_Packet command_packet(FINGERPRINT_COMMANDPACKET, sizeof(command_payload), command_payload);
    finger.writeStructuredPacket(command_packet);

    Adafruit_Fingerprint_Packet response_packet;
    uint8_t p = finger.getStructuredPacket(&response_packet);
    if (p != FINGERPRINT_OK || response_packet.type != FINGERPRINT_ACKPACKET) {
        return FINGERPRINT_PACKETRECIEVEERR;
    }

    uint16_t bytes_sent = 0;
    while (bytes_sent < template_size) {
        uint16_t to_send = min((uint16_t)(template_size - bytes_sent), (uint16_t)64);
        uint8_t packet_type = (bytes_sent + to_send >= template_size) ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;
        Adafruit_Fingerprint_Packet data_packet(packet_type, to_send, template_buffer + bytes_sent);
        finger.writeStructuredPacket(data_packet);
        p = finger.getStructuredPacket(&response_packet);
        if (p != FINGERPRINT_OK || response_packet.type != FINGERPRINT_ACKPACKET) {
            return FINGERPRINT_UPLOADFAIL;
        }
        bytes_sent += to_send;
    }
    return FINGERPRINT_OK;
}

/**************************************************************************************************
 * SETUP
 **************************************************************************************************/
void setup() {
  setupModules();
  setupRtcAndSyncTime();

  displayMessage("Connecting WiFi", "Please wait...", 100);
  setupWiFi(false);

  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("WiFi Connected!", "Syncing...", 2000);
    syncFingerprintsFromServer();
    syncOfflineLogs();
  } else {
    displayMessage("Offline Mode", "RTC Time Active", 2000);
  }

  lastActivityTime = millis();
  showMainMenu();
}

/**************************************************************************************************
 * LOOP
 **************************************************************************************************/
void loop() {
  handleButtons();
  if (digitalRead(BUTTON_PIN1) == HIGH && digitalRead(BUTTON_PIN2) == HIGH) {
    scanForFingerprint();
  }
  if (millis() - lastActivityTime > SLEEP_TIMEOUT) {
    enterLightSleep();
  }
}

/**************************************************************************************************
 * Core System & UI Functions
 **************************************************************************************************/
void setupModules() {
  lcd.begin(16, 2);
  displayMessage("System Booting", "Room: " + String(ROOM_ID), 2000);
  
  if (!SD.begin(SD_CS_PIN)) { 
    displayMessage("SD Card Error!", "Check wiring.", 5000); 
  }

  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (!finger.verifyPassword()) {
    displayMessage("Fingerprint", "Sensor Error!", 5000);
    while(1) { delay(1); }
  }
  
  pinMode(BUTTON_PIN1, INPUT_PULLUP); 
  pinMode(BUTTON_PIN2, INPUT_PULLUP);
}

void setupRtcAndSyncTime() {
  if (!rtc.begin()) { 
    displayMessage("RTC Error!", "Check wiring.", 5000); 
    while (1) { delay(1); }
  }
  if (rtc.lostPower()) { 
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__))); 
  }
}

void displayMessage(String line1, String line2 = "", int delayMs = 0) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
  if (delayMs > 0) { delay(delayMs); }
}

void showMainMenu() {
  displayMessage("btn1: enroll", "btn2: sync/wifi");
  lastActivityTime = millis();
}

void enterLightSleep() {
    displayMessage("Entering sleep...", "", 1000);
    lcd.noDisplay();
    esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN1) | (1ULL << BUTTON_PIN2), ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_light_sleep_start();
    justWokeUp = true;
    lcd.display();
    showMainMenu();
}

/**************************************************************************************************
 * Button Handling
 **************************************************************************************************/
void handleButtons() {
    static bool btn1_was_pressed = false;
    static bool btn2_was_pressed = false;
    static uint32_t btn2_press_time = 0;

    lastActivityTime = millis();

    if (digitalRead(BUTTON_PIN1) == LOW) {
        btn1_was_pressed = true;
    } else {
        if (btn1_was_pressed) {
            btn1_was_pressed = false;
            if (!justWokeUp) {
                enrollNewFingerprint();
            }
        }
    }

    if (digitalRead(BUTTON_PIN2) == LOW) {
        if (!btn2_was_pressed) {
            btn2_press_time = millis();
        }
        btn2_was_pressed = true;
    } else {
        if (btn2_was_pressed) {
            btn2_was_pressed = false;
            if (!justWokeUp) {
                ifενδφ (millis() - btn2_press_time < LONG_PRESS_DELAY) {
                    forceSync();
                }
            }
        }
    }
    
    if (btn2_was_pressed && (millis() - btn2_press_time >= LONG_PRESS_DELAY)) {
        if (!justWokeUp) {
            setupWiFi(true);
            btn2_was_pressed = false;
        }
    }

    justWokeUp = false;
}

/**************************************************************************************************
 * Fingerprint Synchronization & Enrollment
 **************************************************************************************************/
void syncFingerprintsFromServer() {
  if(WiFi.status() != WL_CONNECTED) return;
  displayMessage("Syncing DB...", "Please wait.", 10);
  
  if(finger.emptyDatabase() != FINGERPRINT_OK) {
    displayMessage("Error clearing", "sensor memory", 2000);
    return;
  }
  
  HTTPClient http;
  http.setConnectTimeout(10000); 
  String url = String(SERVER_HOST) + FP_ALL_ENDPOINT;
  http.begin(url);
  
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    StaticJsonDocument<2048> doc; 
    DeserializationError error = deserializeJson(doc, http.getStream());
    if(error){
      displayMessage("JSON Parse Err", error.c_str(), 3000);
      http.end();
      return;
    }

    JsonArray users = doc.as<JsonArray>();
    displayMessage("Syncing " + String(users.size()) + " users", "", 10);
    
    for(JsonObject user : users) {
      uint16_t id = user["id"];
      String b64_template = user["template"];
      
      uint16_t decodedLen = decode_base64_length((unsigned char*)b64_template.c_str(), b64_template.length());
      uint8_t decoded_template[decodedLen];
      decode_base64((unsigned char*)b64_template.c_str(), b64_template.length(), decoded_template);

      if (uploadTemplate(decoded_template, decodedLen) == FINGERPRINT_OK) {
        if (finger.storeModel(id) != FINGERPRINT_OK) {
            displayMessage("Error storing ID", String(id), 1000);
        }
      } else {
        displayMessage("Error loading ID", String(id), 1000);
      }
    }
    displayMessage("Sync Complete!", "", 1500);
  } else {
    displayMessage("Sync Failed", "Server Err " + String(httpCode), 2000);
  }
  http.end();
}

void enrollNewFingerprint() {
    displayMessage("Enrollment:", "Fetching ID...", 10);
    uint16_t newId = fetchLastIdFromServer() + 1;
    
    while (uint8_t timeout = 0 ; newId == 0 && timeout < 5 ; timeout++;) {
        delay(1000);
        newId = fetchLastIdFromServer() + 1;
    } 
    if (newId == 0) {
        displayMessage("Enroll Failed", "Check Server/WiFi", 2000);
        showMainMenu(); return;
    }
    for (timeout = 0 ; p != FINGERPRINT_OK ; timeout++) {
    {
        displayMessage("Place finger", "ID: " + String(newId), 1000);
        uint8_t p = finger.getImage();
        if (p == FINGERPRINT_OK) break;
        if (timeout > 5) { displayMessage("Failed to capture", "Timed out", 1500); showMainMenu(); return; }
    }

    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) { displayMessage("Image Error", "", 1500); showMainMenu(); return; }
    displayMessage("Image 1 OK", "Remove finger");
    
    delay(1000);
    while (finger.getImage() != FINGERPRINT_NOFINGER) { delay(100); }
    
    displayMessage("Place again", "same finger");

    p = finger.getImage();
    if (p != FINGERPRINT_OK) { displayMessage("Timed out", "", 1500); showMainMenu(); return; }
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) { displayMessage("Image Error", "", 1500); showMainMenu(); return; }
    displayMessage("Image 2 OK", "Creating Model...");

    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        displayMessage("Enroll Failed", "Fingers no match", 2000);
        showMainMenu(); return;
    }
    
    p = finger.getModel();
    if (p != FINGERPRINT_OK) {
        displayMessage("Enroll Failed", "Template Error", 2000);
        showMainMenu(); return;
    }

    unsigned int encodedLen = encode_base64_length(FINGERPRINT_TEMPLATE_SIZE);
    char b64_template[encodedLen + 1];
    encode_base64(finger.templateBuffer, FINGERPRINT_TEMPLATE_SIZE, (unsigned char*)b64_template);
    b64_template[encodedLen] = '\0';

    StaticJsonDocument<1024> doc;
    doc["id"] = newId;
    doc["template"] = b64_template;
    String payload;
    serializeJson(doc, payload);

    displayMessage("Uploading...", "ID: " + String(newId));
    if(postJsonToServer(FP_ENROLL_ENDPOINT, payload)) {
        displayMessage("Enroll Success!", "DB updated.", 2000);
        forceSync(); 
    } else {
        displayMessage("Upload Failed!", "Check Server.", 2000);
    }
    showMainMenu();
    }
}

/**************************************************************************************************
 * Attendance Logging & Server Communication
 **************************************************************************************************/
void scanForFingerprint() {
  if (finger.getImage() != FINGERPRINT_OK) return;
  if (finger.image2Tz() != FINGERPRINT_OK) return;
  if (finger.fingerSearch() == FINGERPRINT_OK) {
    lastActivityTime = millis();
    uint16_t fingerID = finger.fingerID;
    
    displayMessage("Welcome!", "ID: " + String(fingerID), 1500);
    
    time_t currentTimestamp = rtc.now().unixtime();
    if(logAttendanceToServer(fingerID, ROOM_ID, currentTimestamp)){
        displayMessage("Attendance Logged", "Thank You", 1500);
    } else {
        displayMessage("Saved Offline", "Will sync later", 1500);
        logAttendanceOffline(fingerID, ROOM_ID, currentTimestamp);
    }
    showMainMenu();
  }
}

bool logAttendanceToServer(uint16_t fingerID, uint8_t roomID, time_t timestamp) {
  if (WiFi.status() != WL_CONNECTED) return false;
  StaticJsonDocument<256> doc;
  doc["fingerid"] = fingerID;
  doc["roomid"] = roomID;
  doc["timestamp"] = timestamp;
  String payload;
  serializeJson(doc, payload);
  return postJsonToServer(ATTENDANCE_ENDPOINT, payload);
}

void logAttendanceOffline(uint16_t id, uint8_t roomID, time_t timestamp) {
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_APPEND);
  if (logFile) {
    logFile.print(String(id) + "," + String(roomID) + "," + String(timestamp) + "\n");
    logFile.close();
  } else {
    displayMessage("SD Write Error!", "", 2000);
  }
}

void syncOfflineLogs() {
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_READ);
  if (!logFile || !logFile.size()) { 
    if(logFile) logFile.close(); 
    return; 
  }

  displayMessage("Syncing Logs...", "", 1000);
  String tempLogName = "/temp_log.txt";
  File tempFile = SD.open(tempLogName, FILE_WRITE);
  if(!tempFile) { 
      displayMessage("Temp File Error", "", 2000);
      logFile.close(); 
      return; 
  }
  
  bool allSynced = true;
  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int firstComma = line.indexOf(',');
    int secondComma = line.indexOf(',', firstComma + 1);

    if (firstComma > 0 && secondComma > 0) {
      uint16_t id = line.substring(0, firstComma).toInt();
      uint8_t roomID_from_log = line.substring(firstComma + 1, secondComma).toInt();
      time_t timestamp = (time_t)strtoul(line.substring(secondComma + 1).c_str(), NULL, 10);
      if (!logAttendanceToServer(id, roomID_from_log, timestamp)) {
        allSynced = false;
        tempFile.println(line);
      }
    }
  }
  logFile.close();
  tempFile.close();

  SD.remove(OFFLINE_LOG_FILE);
  if (!allSynced) {
    SD.rename(tempLogName, OFFLINE_LOG_FILE);
  } else {
    SD.remove(tempLogName);
  }
}

/**************************************************************************************************
 * Network & Utility Functions
 **************************************************************************************************/
void setupWiFi(bool portal) {
  WiFiManager wm;
  wm.setConnectTimeout(20);
  
  if (portal) {
    displayMessage("WiFi Setup Mode", "Connect to AP...");
    if (!wm.startConfigPortal("FingerprintSetupAP")) {
      displayMessage("Setup Failed", "No credentials.", 2000);
    } else {
      displayMessage("Setup Success!", "Rebooting...", 2000);
      delay(1000);
      ESP.restart();
    }
  } else {
    wm.autoConnect("FingerprintSetupAP");
  }

  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)){
        rtc.adjust(DateTime(timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday, timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec));
    }
  }
}

bool postJsonToServer(const String& endpoint, const String& payload) {
    if (WiFi.status() != WL_CONNECTED) return false;
    
    int attempt = 0;
    while (attempt < MAX_HTTP_RETRIES) {
        HTTPClient http;
        http.setConnectTimeout(10000);
        String url = String(SERVER_HOST) + endpoint;
        http.begin(url);
        http.addHeader("Content-Type", "application/json");
        int httpCode = http.POST(payload);
        http.end();
        
        if (httpCode >= 200 && httpCode < 300) {
            return true;
        }
        attempt++;
        if (attempt < MAX_HTTP_RETRIES) {
            displayMessage("Retry " + String(attempt + 1), "Server Error", 1000);
            delay(1000);
        }
    }
    return false;
}

uint16_t fetchLastIdFromServer() {
  if (WiFi.status() != WL_CONNECTED) { return 0; }
  HTTPClient http;
  http.setConnectTimeout(10000);
  String url = String(SERVER_HOST) + FP_LAST_ID_ENDPOINT;
  http.begin(url);
  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    uint16_t id = http.getString().toInt();
    http.end();
    return id;
  }
  http.end();
  return 0;
}

void forceSync() {
    displayMessage("Force Syncing...", "Please wait");
    if (WiFi.status() != WL_CONNECTED) {
        displayMessage("No WiFi", "Sync failed.", 2000);
    } else {
        syncFingerprintsFromServer();
        syncOfflineLogs();
    }
    showMainMenu();
}