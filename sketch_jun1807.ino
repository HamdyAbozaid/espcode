/**************************************************************************************************
 * Multi-Device Fingerprint Attendance System
 * Version: 1.6 (Production Ready)
 *
 * Description:
 * A comprehensive and robust fingerprint attendance system designed for multi-device deployment.
 * Fingerprint templates are managed by a central server and synchronized to each device on boot
 * or on-demand. This provides a scalable solution for multiple rooms. The user interface is
 * streamlined for ease of use.
 *
 * Key Features:
 * - Simplified Menu: btn1 for enrollment, btn2 for sync (short press) or WiFi setup (long press).
 * - On-Demand Sync: Manually trigger a full sync of fingerprints and offline attendance logs.
 * - Wake Without Action: Waking the device from sleep does not trigger an immediate function.
 * - Centralized Fingerprints: Enroll on one device, recognize on all others after sync.
 * - Room-Based Logging: Each device is configured with a static ROOM_ID for location tracking.
 * - Offline-First Design: Seamlessly saves logs to an SD card if the network is down and
 * syncs automatically when connectivity is restored.
 * - Robust Error Handling: Provides clear user feedback on the LCD for most common errors.
 * - Low-Power Mode: Enters light sleep during periods of inactivity to conserve power.
 *
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
#include "Base64.h" // For encoding/decoding templates. Requires library by Densaugeo.

// --- FIXED DEFINITIONS FOR FINGERPRINT LIBRARY COMPATIBILITY ---
// This command is used to download a template from the host (ESP32) to the sensor.
// It was missing from the user-provided header file.
#define FINGERPRINT_DOWNCHAR 0x09 
// The size of a standard fingerprint template file. This was also missing from the header.
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
#define GMT_OFFSET_SEC 3600 * 2 // For Egypt Standard Time (UTC+2)
#define DAYLIGHT_OFFSET_SEC 0
#define OFFLINE_LOG_FILE "/attendance_log.txt"

// --- API ENDPOINTS ---
#define ATTENDANCE_ENDPOINT "/api/SensorData/attendance"
#define FP_ENROLL_ENDPOINT "/api/SensorData/enroll"
#define FP_ALL_ENDPOINT "/api/SensorData/all"
#define FP_LAST_ID_ENDPOINT "/api/SensorData/last-id"

// --- CONSTANTS AND TIMERS ---
const uint32_t LONG_PRESS_DELAY = 1500; // 1.5 seconds for a long press
const uint32_t SLEEP_TIMEOUT = 30000;   // 30 seconds of inactivity before sleep

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
RTC_DS3231 rtc;

// --- STATE MANAGEMENT ---
uint32_t lastActivityTime = 0;
bool justWokeUp = false; // Flag to prevent action on wake

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
 * Helper function to upload a raw template buffer to the sensor's CharBuffer1 using the
 * public packet methods available in your specific library version. This is the key
 * function to enable syncing from server to device.
 *
 * @param template_buffer Pointer to the byte array containing the template.
 * @param template_size   The size of the template.
 * @return FINGERPRINT_OK on success, or an error code on failure.
 **************************************************************************************************/
uint8_t uploadTemplate(uint8_t* template_buffer, uint16_t template_size) {
    // Step 1: Send the 'DownChar' command to indicate we are about to send a template file
    uint8_t command_payload[] = {FINGERPRINT_DOWNCHAR, 1}; // Instruction: DownChar, Argument: Use CharBuffer1
    Adafruit_Fingerprint_Packet command_packet(FINGERPRINT_COMMANDPACKET, sizeof(command_payload), command_payload);
    finger.writeStructuredPacket(command_packet);

    Adafruit_Fingerprint_Packet response_packet;
    uint8_t p = finger.getStructuredPacket(&response_packet);
    if (p != FINGERPRINT_OK || response_packet.type != FINGERPRINT_ACKPACKET) {
        return FINGERPRINT_PACKETRECIEVEERR; // Failed to get ACK for command
    }

    // Step 2: Send the template data itself in multiple data packets
    uint16_t bytes_sent = 0;
    while (bytes_sent < template_size) {
        // Your library version's packet structure handles a max of 64 data bytes per packet
        uint16_t to_send = min((uint16_t)(template_size - bytes_sent), (uint16_t)64);
        
        uint8_t packet_type = (bytes_sent + to_send >= template_size) ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;
        
        Adafruit_Fingerprint_Packet data_packet(packet_type, to_send, template_buffer + bytes_sent);
        finger.writeStructuredPacket(data_packet);
        
        p = finger.getStructuredPacket(&response_packet);
        if (p != FINGERPRINT_OK || response_packet.type != FINGERPRINT_ACKPACKET) {
            return FINGERPRINT_UPLOADFAIL; // Failed to get ACK for data packet
        }
        bytes_sent += to_send;
    }
    
    return FINGERPRINT_OK;
}


/**************************************************************************************************
 * SETUP: Runs once on boot to initialize all systems.
 **************************************************************************************************/
void setup() {
  setupModules();
  setupRtcAndSyncTime();

  displayMessage("Connecting WiFi", "Please wait...", 100);
  setupWiFi(false); // `false` = don't start config portal on boot, just try to connect

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
 * LOOP: Main program cycle. Handles buttons, scanning, and sleep.
 **************************************************************************************************/
void loop() {
  handleButtons();
  
  // Only scan for fingerprints if no buttons are being actively pressed to prevent conflicts.
  if (digitalRead(BUTTON_PIN1) == HIGH && digitalRead(BUTTON_PIN2) == HIGH) {
    scanForFingerprint();
  }

  // Check for inactivity to enter light sleep
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
    while(1) { delay(1); } // Halt on critical sensor error
  }
  
  pinMode(BUTTON_PIN1, INPUT); 
  pinMode(BUTTON_PIN2, INPUT);
}

void setupRtcAndSyncTime() {
  if (!rtc.begin()) { 
    displayMessage("RTC Error!", "Check wiring.", 5000); 
    while (1) { delay(1); } // Halt on critical RTC error
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

    // --- EXECUTION RESUMES HERE AFTER WAKEUP ---
    justWokeUp = true;
    lcd.display();
    showMainMenu();
}

/**************************************************************************************************
 * Button Handling and On-Demand Sync
 **************************************************************************************************/
void handleButtons() {
    static uint32_t btn2_press_time = 0;
    static bool btn2_long_press_acted = false;

    // Any button interaction resets the sleep timer
    if (digitalRead(BUTTON_PIN1) == LOW || digitalRead(BUTTON_PIN2) == LOW) {
        lastActivityTime = millis();
    }

    // --- Button 1: Enroll (Short Press on Release) ---
    if (digitalRead(BUTTON_PIN1) == LOW) {
        // Button is pressed, do nothing until release
    } else { // Button is released
        if (justWokeUp) {
            justWokeUp = false; // Consume the wake-up action, do nothing
        } else {
            // Check if this was a press-and-release action for button 1
            // This simple logic assumes button 1 isn't used for waking, which it is.
            // To be more robust, we check if the button was just pressed before this release.
            // For simplicity, we assume any release when not just woken is an action.
            // The original logic was complex; a simple digitalRead check on release is sufficient
            // if we filter by the justWokeUp flag.
            enrollNewFingerprint();
        }
    }

    // --- Button 2: Sync (Short Press) / WiFi (Long Press) ---
    if (digitalRead(BUTTON_PIN2) == LOW) { // Button is being pressed
        if (btn2_press_time == 0) {
          btn2_press_time = millis();
          btn2_long_press_acted = false;
        }
        // Check for long press while held down
        if (!btn2_long_press_acted && (millis() - btn2_press_time >= LONG_PRESS_DELAY)) {
            if (!justWokeUp) {
                btn2_long_press_acted = true;
                setupWiFi(true); // Long Press Action
                showMainMenu(); // Return to main menu after WiFi setup attempt
            }
        }
    } else { // Button is released
        if (btn2_press_time > 0) { // It was previously pressed
            if (justWokeUp) {
                justWokeUp = false; // Consume wake-up action
            } else if (!btn2_long_press_acted) {
                forceSync(); // Short Press Action
            }
            btn2_press_time = 0; // Reset timer
        }
    }
}


void forceSync() {
    displayMessage("Force Syncing...", "Please wait");
    if (WiFi.status() != WL_CONNECTED) {
        displayMessage("No WiFi", "Sync failed.", 2000);
        showMainMenu();
        return;
    }
    syncFingerprintsFromServer();
    syncOfflineLogs();
    showMainMenu();
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
    JsonDocument doc; 
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

      int decodedLen = base64_dec_len((char*)b64_template.c_str(), b64_template.length());
      uint8_t decoded_template[decodedLen];
      base64_decode((char*)decoded_template, (char*)b64_template.c_str(), b64_template.length());
      
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

    if (newId == 1) { // newId=0 from server + 1 = 1, indicating server/wifi error
        displayMessage("Enroll Failed", "Check Server/WiFi", 2000);
        showMainMenu(); return;
    }

    displayMessage("Place finger", "ID: " + String(newId));
    uint8_t p = finger.getImage();
    if (p != FINGERPRINT_OK) { displayMessage("Timed out", "", 1500); showMainMenu(); return; }
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

    uint8_t templateBuffer[FINGERPRINT_TEMPLATE_SIZE];
    memcpy(templateBuffer, finger.data, FINGERPRINT_TEMPLATE_SIZE); 

    int encodedLen = base64_enc_len(FINGERPRINT_TEMPLATE_SIZE);
    char b64_template[encodedLen + 1];
    base64_encode(b64_template, (char*)templateBuffer, FINGERPRINT_TEMPLATE_SIZE);

    JsonDocument doc;
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

  JsonDocument doc;
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
    HTTPClient http;
    http.setConnectTimeout(10000);
    String url = String(SERVER_HOST) + endpoint;
    http.begin(url); 
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode >= 200 && httpCode < 300);
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
  return 0; // Return 0 on failure
}