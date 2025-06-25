/**************************************************************************************************
 * Improved Fingerprint Attendance System for ESP32
 * Version: 1.2 (Lecture Schedule Integration)
 *
 * Description:
 * A comprehensive fingerprint attendance system using an ESP32. It is designed to be
 * robust and user-friendly, with features for online and offline operation. This version
 * fetches a weekly lecture schedule and logs attendance against specific lecture IDs.
 *
 * Key Features:
 * - Lecture-Based Attendance: Fetches a weekly schedule and sends attendance with a lecture ID.
 * - On-Demand WiFi Setup: Uses WiFiManager for easy WiFi configuration.
 * - Offline Logging: If no WiFi is available, attendance logs (with lecture ID) are saved to an SD card.
 * - Automatic Sync: Saved offline logs are automatically sent to the server once an
 * internet connection is established.
 * - Accurate Timestamps: Uses a DS3231 RTC to determine the correct lecture slot.
 * - Secure Server Communication: Uses HTTPS for secure data transmission.
 * - User-Friendly Interface: A 16x2 LCD provides clear instructions and feedback.
 * - Low-Power Mode: Enters light sleep when idle to conserve battery, waking on button press.
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

// --- HARDWARE PIN DEFINITIONS ---
const uint8_t rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
const uint8_t FINGERPRINT_RX = 16, FINGERPRINT_TX = 17;
const uint8_t BUTTON_PIN1 = 34; // Requires external pull-up resistor
const uint8_t BUTTON_PIN2 = 35; // Requires external pull-up resistor
const uint8_t SD_CS_PIN = 5;

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.6:7069"
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600 * 2
#define DAYLIGHT_OFFSET_SEC 0
#define OFFLINE_LOG_FILE "/attendance_log.txt"
#define LECTURE_SCHEDULE_ENDPOINT "/api/lectures/schedule" // Endpoint to get schedule
#define ENROLL_ENDPOINT "/api/SensorData/enroll"
#define ATTENDANCE_ENDPOINT "/api/SensorData"
#define LAST_ID_ENDPOINT "/api/SensorData/last-id"
#define CLEAR_AUTH_ENDPOINT "/api/SensorData/clear"


// --- CONSTANTS AND TIMERS ---
const uint32_t DEBOUNCE_DELAY = 50;
const uint32_t LONG_PRESS_DELAY = 1000;
const uint32_t MENU_TIMEOUT = 10000;
const uint32_t SLEEP_TIMEOUT = 15000;

// --- GLOBAL OBJECTS ---
LiquidCrystal lcd(rs, en, d4, d5, d6, d7);
HardwareSerial fingerSerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);
RTC_DS3231 rtc;

// --- LECTURE SCHEDULE STORAGE ---
// 5 days (Sat-Wed), 4 lectures per day
uint16_t lectureSchedule[5][4];

// --- STATE MANAGEMENT ---
enum class MenuState {
  MAIN_MENU,
  OPTIONS_MENU
};
MenuState currentMenuState = MenuState::MAIN_MENU;
uint32_t lastActivityTime = 0;

// --- BUTTON STATE VARIABLES ---
uint8_t btn1State = HIGH, btn2State = HIGH;
uint32_t btn1PressTime = 0, btn2PressTime = 0;
bool btn1Held = false, btn2Held = false;

// --- FUNCTION PROTOTYPES ---
void displayMessage(String, String, int);
void handleButtons();
void setupWiFi(bool);
bool logToServer(const String&, const String&);
bool syncAttendanceToServer(uint16_t, uint16_t);
uint16_t fetchLastIdFromServer();
void scanForFingerprint();
void enrollNewFingerprint();
uint8_t getFingerprintImage(int);
uint8_t createAndStoreModel(uint16_t);
void syncOfflineLogs();
void attemptToClearAllData();
void setupModules();
void setupRtcAndSyncTime();
void logAttendanceOffline(uint16_t, uint16_t);
void showMainMenu();
void showOptionsMenu();
void enterLightSleep();
// New functions for lecture schedule
bool fetchLectureSchedule();
uint16_t determineCurrentLectureID(const DateTime&);


/**************************************************************************************************
 * SETUP: Runs once on boot.
 **************************************************************************************************/
void setup() {
  setupModules();
  setupRtcAndSyncTime();

  displayMessage("Connecting WiFi", "Please wait...");
  setupWiFi(false); // false = don't start config portal on boot

  if (WiFi.status() == WL_CONNECTED) {
    displayMessage("WiFi Connected!", WiFi.localIP().toString(), 1500);
    if(fetchLectureSchedule()){
        displayMessage("Schedule OK!", "Syncing logs...", 1500);
        syncOfflineLogs();
    } else {
        displayMessage("Schedule FAIL", "Check Server", 3000);
    }
  } else {
    displayMessage("Offline Mode", "RTC Time Active", 2000);
  }

  lastActivityTime = millis();
  showMainMenu();
}

/**************************************************************************************************
 * LOOP: Main program cycle.
 **************************************************************************************************/
void loop() {
  handleButtons();

  if (currentMenuState == MenuState::MAIN_MENU && !btn1PressTime && !btn2PressTime) {
    scanForFingerprint();
  }

  if (currentMenuState == MenuState::OPTIONS_MENU && (millis() - lastActivityTime > MENU_TIMEOUT)) {
      displayMessage("Timeout", "Returning...", 1500);
      showMainMenu();
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
  displayMessage("System Booting", "Please wait...");
  delay(2000);

  if (!SD.begin(SD_CS_PIN)) {
    displayMessage("SD Card Error!", "Check wiring.", 5000);
  }

  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (finger.verifyPassword()) {
    displayMessage("Fingerprint", "Sensor OK!", 1500);
  } else {
    displayMessage("Fingerprint", "Sensor Error!", 5000);
    while(1) { delay(1); }
  }

  pinMode(BUTTON_PIN1, INPUT); 
  pinMode(BUTTON_PIN2, INPUT);
}

void setupRtcAndSyncTime() {
  if (!rtc.begin()) {
    displayMessage("RTC Error!", "Check wiring.", 5000);
    while (1) { delay(1); }
  }
  
  if (rtc.lostPower()) {
    displayMessage("RTC Power Lost!", "Syncing time...", 2000);
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
}

void displayMessage(String line1, String line2 = "", int delayMs = 0) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(line1.substring(0, 16));
  lcd.setCursor(0, 1);
  lcd.print(line2.substring(0, 16));
  if (delayMs > 0) {
    delay(delayMs);
  }
}

void enterLightSleep() {
    displayMessage("Entering sleep...", "Press btn to wake", 2000);
    lcd.noDisplay();
    // Wake on either button
    esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN1) | (1ULL << BUTTON_PIN2), ESP_EXT1_WAKEUP_ANY_HIGH);
    esp_light_sleep_start();
    // --- Code execution resumes here after wakeup ---
    lcd.display();
    lastActivityTime = millis();
    showMainMenu();
}

/**************************************************************************************************
 * Button Handling Logic
 **************************************************************************************************/
void handleButtons() {
    uint8_t currentBtn1 = digitalRead(BUTTON_PIN1);
    uint8_t currentBtn2 = digitalRead(BUTTON_PIN2);

    // --- Button 1 Logic (Add/Options) ---
    if (currentBtn1 == LOW && btn1State == HIGH && (millis() - btn1PressTime > DEBOUNCE_DELAY)) {
        btn1PressTime = millis();
    }
    if(btn1State == LOW && currentBtn1 == HIGH){ // Released
        if(!btn1Held){ // Short Press
            lastActivityTime = millis();
            if (currentMenuState == MenuState::MAIN_MENU) { enrollNewFingerprint(); }
        }
        btn1Held = false;
        btn1PressTime = 0;
    }
    if (btn1PressTime != 0 && !btn1Held && (millis() - btn1PressTime > LONG_PRESS_DELAY)) { // Held
        btn1Held = true;
        lastActivityTime = millis();
        if (currentMenuState == MenuState::MAIN_MENU) { showOptionsMenu(); } 
        else if (currentMenuState == MenuState::OPTIONS_MENU) { attemptToClearAllData(); }
    }
    btn1State = currentBtn1;

    // --- Button 2 Logic (WiFi Manager / Wake) ---
     if (currentBtn2 == LOW && btn2State == HIGH && (millis() - btn2PressTime > DEBOUNCE_DELAY)) {
        btn2PressTime = millis();
    }
    if(btn2State == LOW && currentBtn2 == HIGH){ // Released
        btn2Held = false;
        btn2PressTime = 0;
    }
    if (btn2PressTime != 0 && !btn2Held && (millis() - btn2PressTime > LONG_PRESS_DELAY)) { // Held
        btn2Held = true;
        lastActivityTime = millis();
        setupWiFi(true);
        showMainMenu();
    }
    btn2State = currentBtn2;
}


/**************************************************************************************************
 * Lecture Schedule Functions
 **************************************************************************************************/
/**
 * @brief Fetches the weekly lecture schedule from the server.
 * @return True on success, false on failure.
 */
bool fetchLectureSchedule() {
    if (WiFi.status() != WL_CONNECTED) return false;

    HTTPClient http;
    String url = String(SERVER_HOST) + LECTURE_SCHEDULE_ENDPOINT;
    http.begin(url);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        DynamicJsonDocument doc(2048); // Adjust size as needed
        deserializeJson(doc, http.getString());
        
        JsonArray days = doc.as<JsonArray>();
        if (days.size() != 5) return false; // Expect 5 working days

        int dayIndex = 0;
        for (JsonArray day_lectures : days) {
            if (day_lectures.size() != 4) return false; // Expect 4 lectures per day
            int lectureIndex = 0;
            for (JsonVariant id : day_lectures) {
                lectureSchedule[dayIndex][lectureIndex] = id.as<uint16_t>();
                lectureIndex++;
            }
            dayIndex++;
        }
        http.end();
        return true;
    } else {
        http.end();
        return false;
    }
}

/**
 * @brief Determines the current lecture ID based on day and time.
 * @param now The current DateTime object from the RTC.
 * @return The lecture ID, or 0 if no lecture is active.
 */
uint16_t determineCurrentLectureID(const DateTime& now) {
    // RTClib: Sunday=0, Monday=1, ..., Saturday=6
    // Our schedule: Saturday=0, Sunday=1, ..., Wednesday=4
    int dayOfWeek = now.dayOfTheWeek();
    int scheduleDay = -1;

    if (dayOfWeek == 6) scheduleDay = 0; // Saturday
    else if (dayOfWeek >= 0 && dayOfWeek <= 3) scheduleDay = dayOfWeek + 1; // Sunday-Wednesday
    else return 0; // It's Thursday or Friday

    int hour = now.hour();
    int minute = now.minute();

    if (hour == 9 || (hour == 10 && minute < 30)) { // 9:00 - 10:29
        return lectureSchedule[scheduleDay][0];
    } else if ((hour == 10 && minute >= 30) || hour == 11) { // 10:30 - 11:59
        return lectureSchedule[scheduleDay][1];
    } else if ((hour == 12 && minute >= 30) || hour == 13) { // 12:30 - 13:59
        return lectureSchedule[scheduleDay][2];
    } else if (hour == 14 || (hour == 15 && minute < 30)) { // 14:00 - 15:29
        return lectureSchedule[scheduleDay][3];
    }

    return 0; // Not within any lecture time
}

/**************************************************************************************************
 * WiFi & Server Functions
 **************************************************************************************************/

void setupWiFi(bool portal) {
  WiFiManager wm;
  wm.setConnectTimeout(20);
  
  if (portal) {
    displayMessage("Setup Mode", "Connect to AP...", 0);
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
        displayMessage("Time Synced!", "", 1500);
    }
  }
}

uint16_t fetchLastIdFromServer() {
  if (WiFi.status() != WL_CONNECTED) {
    displayMessage("No WiFi", "Cannot get ID", 2000);
    return 0;
  }
  
  HTTPClient http;
  String url = String(SERVER_HOST) + LAST_ID_ENDPOINT;
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    uint16_t id = http.getString().toInt();
    http.end();
    return id;
  } else {
    displayMessage("Server Error", "Code: " + String(httpCode), 2000);
    http.end();
    return 0; // Return 0 to indicate failure
  }
}

bool syncAttendanceToServer(uint16_t fingerID, uint16_t lectureID) {
  if (WiFi.status() != WL_CONNECTED) return false;

  StaticJsonDocument<128> doc;
  doc["fingerid"] = fingerID;
  doc["lectureid"] = lectureID;

  String payload;
  serializeJson(doc, payload);

  return logToServer(ATTENDANCE_ENDPOINT, payload);
}

bool logToServer(const String& endpoint, const String& payload) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient http;
    String url = String(SERVER_HOST) + endpoint;
    http.begin(url); 
    http.addHeader("Content-Type", "application/json");
    int httpCode = http.POST(payload);
    http.end();
    return (httpCode >= 200 && httpCode < 300);
}


/**************************************************************************************************
 * Fingerprint Operation Functions
 **************************************************************************************************/

void scanForFingerprint() {
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK) return;

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK) return;

  p = finger.fingerSearch();
  if (p == FINGERPRINT_OK) {
    lastActivityTime = millis();
    uint16_t fingerID = finger.fingerID;
    
    DateTime now = rtc.now();
    uint16_t lectureID = determineCurrentLectureID(now);

    if (lectureID == 0) {
        displayMessage("Welcome ID:" + String(fingerID), "No Lecture Now", 2000);
        showMainMenu();
        return;
    }

    displayMessage("ID:" + String(fingerID) + " LECT:" + lectureID, "Logging...", 2000);
    
    if(syncAttendanceToServer(fingerID, lectureID)){
        displayMessage("Attendance Sent!", "Thank You", 1500);
    } else {
        displayMessage("Saved Offline", "Will sync later", 1500);
        logAttendanceOffline(fingerID, lectureID);
    }
    showMainMenu();
  }
}


void enrollNewFingerprint() {
  displayMessage("Enrollment:", "Fetching ID...");
  uint16_t newId = fetchLastIdFromServer() + 1;

  if (newId == 1 && WiFi.status() != WL_CONNECTED) { // Check if fetch failed due to WiFi
    displayMessage("Enroll Failed", "Check WiFi", 2000);
    showMainMenu(); return;
  }
  if (newId == 1 && fetchLastIdFromServer() == 0) { // Check if server returned 0
    displayMessage("Enroll Failed", "Check Server", 2000);
    showMainMenu(); return;
  }
  
  displayMessage("Place finger", "ID: " + String(newId));
  if (getFingerprintImage(1) != FINGERPRINT_OK) { showMainMenu(); return; }
  
  displayMessage("Place again", "ID: " + String(newId));
  if (getFingerprintImage(2) != FINGERPRINT_OK) { showMainMenu(); return; }
  
  displayMessage("Creating model", "Please wait...");
  if (createAndStoreModel(newId) == FINGERPRINT_OK) {
      displayMessage("Enrolled!", "ID: " + String(newId), 2000);
      StaticJsonDocument<64> doc;
      doc["id"] = newId;
      String payload;
      serializeJson(doc, payload);
      if(!logToServer(ENROLL_ENDPOINT, payload)) {
          displayMessage("Server Sync Failed", "Enrollment local", 2000);
      }
  } else {
      displayMessage("Enroll Failed", "Error storing", 2000);
  }
  showMainMenu();
}

uint8_t getFingerprintImage(int step) {
  uint8_t p = -1;
  while (p != FINGERPRINT_OK) {
    p = finger.getImage();
    if (digitalRead(BUTTON_PIN1) == LOW) return FINGERPRINT_ENROLLMISMATCH;
  }
  
  p = finger.image2Tz(step);
  if (p == FINGERPRINT_OK) {
    displayMessage("Image taken", "", 1000);
    return FINGERPRINT_OK;
  } else {
    displayMessage("Error", "Could not process", 2000);
    return p;
  }
}

uint8_t createAndStoreModel(uint16_t id) {
  uint8_t p = finger.createModel();
  if (p != FINGERPRINT_OK) {
    displayMessage("Model Error", "Try again", 2000);
    return p;
  }
  p = finger.storeModel(id);
  if (p != FINGERPRINT_OK) {
    displayMessage("Storage Error", "Code: " + String(p), 2000);
  }
  return p;
}


/**************************************************************************************************
 * SD Card and Offline Logging
 **************************************************************************************************/

void logAttendanceOffline(uint16_t id, uint16_t lectureId) {
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_APPEND);
  if (logFile) {
    logFile.println(String(id) + "," + String(lectureId));
    logFile.close();
  } else {
    displayMessage("SD Write Error!", "", 2000);
  }
}

void syncOfflineLogs() {
  File logFile = SD.open(OFFLINE_LOG_FILE, FILE_READ);
  if (!logFile || !logFile.size()) {
    if(logFile) logFile.close();
    return; // No file or empty file
  }
  displayMessage("Syncing logs...", "", 0);

  String tempLogName = "/temp_log.txt";
  File tempFile = SD.open(tempLogName, FILE_WRITE);
  bool allSynced = true;

  while (logFile.available()) {
    String line = logFile.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) continue;

    int commaIndex = line.indexOf(',');
    if (commaIndex != -1) {
      uint16_t id = line.substring(0, commaIndex).toInt();
      uint16_t lectureId = line.substring(commaIndex + 1).toInt();

      if (!syncAttendanceToServer(id, lectureId)) {
        allSynced = false;
        if(tempFile) tempFile.println(line);
      }
    }
  }

  logFile.close();
  if(tempFile) tempFile.close();

  SD.remove(OFFLINE_LOG_FILE);
  if (!allSynced) {
    SD.rename(tempLogName, OFFLINE_LOG_FILE);
    displayMessage("Sync complete.", "Some logs remain.", 2000);
  } else {
    SD.remove(tempLogName);
    displayMessage("Sync complete!", "All logs sent.", 2000);
  }
}

/**************************************************************************************************
 * Menu Actions
 **************************************************************************************************/
void showOptionsMenu() {
  currentMenuState = MenuState::OPTIONS_MENU;
  displayMessage("Hold btn1: Clear", "Timeout: 10s");
  lastActivityTime = millis();
}

void showMainMenu() {
  currentMenuState = MenuState::MAIN_MENU;
  displayMessage("Scan Finger or", "Press Button");
  lastActivityTime = millis();
}

void attemptToClearAllData() {
  displayMessage("Authorizing...", "Please wait", 0);
  
  if (WiFi.status() != WL_CONNECTED) {
      displayMessage("No WiFi", "Cannot clear data", 2000);
      showMainMenu();
      return;
  }

  HTTPClient http;
  String url = String(SERVER_HOST) + CLEAR_AUTH_ENDPOINT;
  http.begin(url);
  int httpCode = http.GET();
  http.end();
  
  if (httpCode >= 200 && httpCode < 300) {
      displayMessage("Authorized!", "Deleting data...", 1000);
      if (finger.emptyDatabase() == FINGERPRINT_OK) {
          displayMessage("All data cleared!", "", 2000);
      } else {
          displayMessage("Delete Failed", "Sensor error", 2000);
      }
  } else {
      displayMessage("Authorization", "Failed! Code: " + String(httpCode), 3000);
  }
  showMainMenu();
}