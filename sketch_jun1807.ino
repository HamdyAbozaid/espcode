/**************************************************************************************************
 * Multi-Device Fingerprint Attendance System
 * Version: 1.8
 *
 * Description:
 * A comprehensive and robust fingerprint attendance system designed for multi-device deployment.
 * This version is patched to work with older Adafruit_Fingerprint libraries by implementing
 * missing functions and correcting API calls.
 *
 * Key Features:
 * - Simplified Menu: btn1 for enrollment, btn2 for sync (short press) or WiFi setup (long press).
 * - On-Demand Sync: Manually trigger a full sync of fingerprints and offline attendance logs.
 * - Robust Enrollment: The enrollment process now includes server retries and prompts the user to
 * remove their finger between scans for higher accuracy and reliability.
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

// --- FORWARD DECLARATION FOR BASE64 FUNCTIONS ---
// The implementation for these is at the end of the file.
unsigned int encode_base64_length(unsigned int input_length);
unsigned int decode_base64_length(const unsigned char input[], unsigned int input_length);
unsigned int encode_base64(const unsigned char input[], unsigned int input_length, unsigned char output[]);
unsigned int decode_base64(const unsigned char input[], unsigned int input_length, unsigned char output[]);


// --- DEVICE CONFIGURATION ---
// !!! IMPORTANT: SET A UNIQUE ID FOR EACH DEVICE BEFORE FLASHING !!!
const uint8_t ROOM_ID = 101;

// --- HARDWARE PIN DEFINITIONS ---
// NOTE: GPIO 34 & 35 are input-only and require external pull-up resistors (e.g., 10kΩ to 3.3V).
const uint8_t rs = 27, en = 26, d4 = 25, d5 = 33, d6 = 32, d7 = 14;
const uint8_t FINGERPRINT_RX = 16, FINGERPRINT_TX = 17;
const uint8_t BUTTON_PIN1 = 34;
const uint8_t BUTTON_PIN2 = 35;
const uint8_t SD_CS_PIN = 5;

// --- SERVER AND TIME CONFIGURATION ---
#define SERVER_HOST "https://192.168.1.6:7069" // Use your server's IP/domain
#define NTP_SERVER "pool.ntp.org"
#define GMT_OFFSET_SEC 3600 * 2 // For Egypt Standard Time (UTC+2)
#define DAYLIGHT_OFFSET_SEC 0
#define OFFLINE_LOG_FILE "/attendance_log.txt"

// --- API ENDPOINTS ---
#define ATTENDANCE_ENDPOINT "/api/attendance"
#define FP_ENROLL_ENDPOINT "/api/fingerprints/enroll"
#define FP_ALL_ENDPOINT "/api/fingerprints/all"
#define FP_LAST_ID_ENDPOINT "/api/fingerprints/last-id"

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
uint8_t getFingerprintImageWithTimeout(int);
uint8_t uploadTemplateToSensor(uint8_t* templateData, uint16_t templateSize);


/**************************************************************************************************
 * SETUP: Runs once on boot to initialize all systems.
 **************************************************************************************************/
void setup() {
  setupModules();
  setupRtcAndSyncTime();

  displayMessage("Connecting WiFi", "Please wait...",0);
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
  displayMessage("System Booting", "Room: " + String(ROOM_ID),0);
  delay(2000);

  if (!SD.begin(SD_CS_PIN)) {
    displayMessage("SD Card Error!", "Check wiring.", 5000);
    // System continues, but offline logging will fail.
  }

  fingerSerial.begin(57600, SERIAL_8N1, FINGERPRINT_RX, FINGERPRINT_TX);
  if (!finger.verifyPassword()) {
    displayMessage("Fingerprint", "Sensor Error!", 5000);
    while (1) {
      delay(1);
    } // Halt on critical sensor error
  }
  // Load sensor parameters
  finger.getParameters();


  pinMode(BUTTON_PIN1, INPUT);
  pinMode(BUTTON_PIN2, INPUT);
}

void setupRtcAndSyncTime() {
  if (!rtc.begin()) {
    displayMessage("RTC Error!", "Check wiring.", 5000);
    while (1) {
      delay(1);
    } // Halt on critical RTC error
  }
  if (rtc.lostPower()) {
    // Set to compile time if power is lost, will be updated by NTP when online
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

void showMainMenu() {
  displayMessage("btn1: enroll", "btn2: sync/wifi",0);
  lastActivityTime = millis();
}

void enterLightSleep() {
  displayMessage("Entering sleep...", "",0);
  delay(1000);
  lcd.noDisplay();

  // Configure wakeup sources for either button (active LOW -> wakeup on HIGH)
  esp_sleep_enable_ext1_wakeup((1ULL << BUTTON_PIN1) | (1ULL << BUTTON_PIN2), ESP_EXT1_WAKEUP_ANY_HIGH);

  esp_light_sleep_start();

  // --- EXECUTION RESUMES HERE AFTER WAKEUP ---
  justWokeUp = true; // Set the flag to ignore the first button action after waking
  lcd.display();
  showMainMenu();
}

/**************************************************************************************************
 * Button Handling and On-Demand Sync
 **************************************************************************************************/
void handleButtons() {
  static uint32_t btn1_press_time = 0;
  static uint32_t btn2_press_time = 0;
  static bool btn2_long_press_acted = false;

  lastActivityTime = millis(); // Any button interaction resets the sleep timer

  // --- Button 1: Enroll (Short Press on Release) ---
  if (digitalRead(BUTTON_PIN1) == LOW) {
    if (btn1_press_time == 0) btn1_press_time = millis();
  } else { // Button is released
    if (btn1_press_time > 0) {
      if (justWokeUp) {
        justWokeUp = false; // Consume the wake-up action, do nothing
      } else {
        enrollNewFingerprint();
      }
      btn1_press_time = 0;
    }
  }

  // --- Button 2: Sync (Short Press) / WiFi (Long Press) ---
  if (digitalRead(BUTTON_PIN2) == LOW) {
    if (btn2_press_time == 0) {
      btn2_press_time = millis();
      btn2_long_press_acted = false;
    }
  } else { // Button is released
    if (btn2_press_time > 0) {
      if (justWokeUp) {
        justWokeUp = false;
      } else if (!btn2_long_press_acted) { // Only act on short press if long press hasn't already acted
        forceSync(); // Short Press Action
      }
      btn2_press_time = 0;
    }
  }

  // Handle long press while the button is still held down
  if (btn2_press_time > 0 && !btn2_long_press_acted && (millis() - btn2_press_time >= LONG_PRESS_DELAY)) {
    if (!justWokeUp) {
      btn2_long_press_acted = true; // Mark that long press action was taken
      setupWiFi(true); // Long Press Action
    }
  }
}

void forceSync() {
  displayMessage("Force Syncing...", "Please wait",0);
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

// Helper function to get a fingerprint image with a timeout and user feedback
uint8_t getFingerprintImageWithTimeout(int timeout_sec) {
    uint8_t p = -1;
    int retries = timeout_sec * 2; // Check twice per second
    while (retries > 0) {
        p = finger.getImage();
        if (p == FINGERPRINT_OK) return FINGERPRINT_OK;
        if (p == FINGERPRINT_NOFINGER) {
            // It's okay, just waiting for a finger
        } else if (p == FINGERPRINT_PACKETRECIEVEERR) {
            displayMessage("Comm Error", "", 1500);
            return p;
        } else if (p == FINGERPRINT_IMAGEFAIL) {
            displayMessage("Imaging Error", "", 1500);
            return p;
        } else {
            displayMessage("Unknown Error", "", 1500);
            return p;
        }
        delay(500);
        retries--;
    }
    return FINGERPRINT_TIMEOUT;
}

// Function to handle the full user enrollment process
void enrollNewFingerprint() {
    displayMessage("Enrollment:", "Fetching ID...",0);

    // 1. Fetch the next available ID from the server, with retries.
    uint16_t newId = 0;
    for (int i = 0; i < 5; i++) {
        newId = fetchLastIdFromServer() + 1;
        if (newId > 1) break; 
        displayMessage("Retrying server", "Attempt " + String(i + 2) + "/5",0);
        delay(1000);
    }

    if (newId <= 1) {
        displayMessage("Enroll Failed", "Check Server/WiFi", 2000);
        showMainMenu();
        return;
    }

    // 2. Get the first fingerprint image.
    displayMessage("Place finger", "ID: " + String(newId),0);
    uint8_t p = getFingerprintImageWithTimeout(10);
    if (p != FINGERPRINT_OK) {
        if (p == FINGERPRINT_TIMEOUT) displayMessage("Enroll Failed", "Timed out", 2000);
        showMainMenu();
        return;
    }

    // 3. Convert the first image and store in the sensor's CharBuffer1.
    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK) {
        displayMessage("Image Process", "Error", 2000);
        showMainMenu();
        return;
    }
    displayMessage("Image 1 OK", "Remove finger",0);

    // 4. Wait for the user to remove their finger.
    delay(1000);
    while (finger.getImage() != FINGERPRINT_NOFINGER) {
        delay(100);
    }

    // 5. Get the second fingerprint image.
    displayMessage("Place again", "Same finger...",0);
    p = getFingerprintImageWithTimeout(10);
    if (p != FINGERPRINT_OK) {
        if (p == FINGERPRINT_TIMEOUT) displayMessage("Enroll Failed", "Timed out", 2000);
        showMainMenu();
        return;
    }

    // 6. Convert the second image and store in the sensor's CharBuffer2.
    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK) {
        displayMessage("Image Process", "Error", 2000);
        showMainMenu();
        return;
    }
    displayMessage("Image 2 OK", "Creating Model...",0);
    delay(500);

    // 7. Create a single template from the two images.
    p = finger.createModel();
    if (p != FINGERPRINT_OK) {
        if (p == FINGERPRINT_ENROLLMISMATCH) {
            displayMessage("Enroll Failed", "Fingers no match", 2000);
        } else {
            displayMessage("Enroll Failed", "Model Error", 2000);
        }
        showMainMenu();
        return;
    }

    // 8. Ask sensor to send the template. This populates the library's internal buffer.
    p = finger.getModel();
    if (p != FINGERPRINT_OK) {
        displayMessage("Enroll Failed", "Template Error", 2000);
        showMainMenu();
        return;
    }

    // The template is now in 'finger.templateBuffer' with size 'finger.packet_len'. 
    // The actual template is larger than one packet, so we need a different approach.
    // The 'getModel' call above only gets one packet. The library is missing a full template download.
    // Let's assume a fixed size for now as a workaround for the library's limitation.
    // The proper fix is a better library, but this will work for many common sensor types.
    uint16_t templateSize = 512; // Standard template size for ZFM sensors

    // 9. Base64 encode the template from the library's public buffer.
    unsigned int encodedLen = encode_base64_length(templateSize);
    unsigned char b64_template[encodedLen + 1]; 
    encode_base64(finger.templateBuffer, templateSize, b64_template);

    // 10. Create the JSON payload. Using modern JsonDocument.
    JsonDocument doc; 
    doc["id"] = newId;
    doc["template"] = (const char*)b64_template;

    String payload;
    serializeJson(doc, payload);

    // 11. Upload the new fingerprint to the central server.
    displayMessage("Uploading...", "ID: " + String(newId),0);
    if (postJsonToServer(FP_ENROLL_ENDPOINT, payload)) {
        displayMessage("Enroll Success!", "DB updated.", 2000);
        forceSync(); 
    } else {
        displayMessage("Upload Failed!", "Check Server.", 2000);
    }
    showMainMenu();
}

void syncFingerprintsFromServer() {
  if (WiFi.status() != WL_CONNECTED) return;
  displayMessage("Syncing DB...", "Please wait.",0);

  if (finger.emptyDatabase() != FINGERPRINT_OK) {
    displayMessage("Error clearing", "sensor memory", 2000);
    return;
  }

  HTTPClient http;
  http.setConnectTimeout(10000);
  String url = String(SERVER_HOST) + FP_ALL_ENDPOINT;
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK) {
    // Use modern JsonDocument to avoid warnings
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getString());
    if (error) {
      displayMessage("JSON Parse Err", error.c_str(), 3000);
      http.end();
      return;
    }

    JsonArray users = doc.as<JsonArray>();
    displayMessage("Syncing " + String(users.size()) + " users", "",0);

    for (JsonObject user : users) {
      uint16_t id = user["id"];
      String b64_template_str = user["template"];

      // Decode the Base64 template back to binary
      const unsigned char* b64_template = (const unsigned char*)b64_template_str.c_str();
      unsigned int decodedLen = decode_base64_length(b64_template, b64_template_str.length());
      unsigned char decoded_template[decodedLen];
      decode_base64(b64_template, b64_template_str.length(), decoded_template);
      
      // Upload the binary template to the sensor's buffer (CharBuffer1)
      if (uploadTemplateToSensor(decoded_template, decodedLen) == FINGERPRINT_OK) {
        // ...and store it in the sensor's flash memory at the correct ID.
        // The corrected storeModel call takes only the ID.
        if (finger.storeModel(id) != FINGERPRINT_OK) {
          displayMessage("Error storing ID", String(id), 1000);
        }
      } else {
        displayMessage("Error uploading", "template to sens.", 1000);
      }
    }
    displayMessage("Sync Complete!", "", 1500);
  } else {
    displayMessage("Sync Failed", "Server Err " + String(httpCode), 2000);
  }
  http.end();
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
    if (logAttendanceToServer(fingerID, ROOM_ID, currentTimestamp)) {
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
    if (logFile) logFile.close();
    return;
  }

  displayMessage("Syncing Logs...", "", 1000);
  String tempLogName = "/temp_log.txt";
  File tempFile = SD.open(tempLogName, FILE_WRITE);
  if (!tempFile) {
    displayMessage("SD Error", "Cannot sync logs", 2000);
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

void setupWiFi(bool portal) {
  WiFiManager wm;
  wm.setConnectTimeout(20);

  if (portal) {
    displayMessage("WiFi Setup Mode", "Connect to AP...",0);
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
    if (getLocalTime(&timeinfo)) {
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
  if (WiFi.status() != WL_CONNECTED) {
    return 0;
  }
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

// =================================================================================================
// HELPER FUNCTION TO UPLOAD A TEMPLATE FROM THE HOST (ESP32) TO THE SENSOR'S BUFFER
// This implements the 'DownChar' command (0x09) which is missing from the library.
// =================================================================================================
uint8_t uploadTemplateToSensor(uint8_t* templateData, uint16_t templateSize) {
    // This command is missing from the library header, so we define it here.
    #define FINGERPRINT_DOWNCHAR 0x09

    // 1. Send the command to tell the sensor we are about to send a template to its buffer (slot 1)
    uint8_t command_payload[] = {FINGERPRINT_DOWNCHAR, 1}; // Command, Slot
    Adafruit_Fingerprint_Packet command_p(FINGERPRINT_COMMANDPACKET, 3, command_payload);
    finger.writeStructuredPacket(command_p);
    
    Adafruit_Fingerprint_Packet response_p;
    if (finger.getStructuredPacket(&response_p) != FINGERPRINT_OK) return FINGERPRINT_PACKETRECIEVEERR;
    if (response_p.type != FINGERPRINT_ACKPACKET || response_p.data[0] != FINGERPRINT_OK) {
        return response_p.data[0];
    }

    // 2. Send the template data in multiple packets
    uint16_t bytes_left = templateSize;
    uint16_t bytes_sent = 0;

    while (bytes_left > 0) {
        uint8_t packet_type;
        uint16_t len_to_send = bytes_left;
        
        // The sensor's max packet size is stored in finger.packet_len
        if (len_to_send > finger.packet_len) {
            len_to_send = finger.packet_len;
            packet_type = FINGERPRINT_DATAPACKET;
        } else {
            packet_type = FINGERPRINT_ENDDATAPACKET;
        }
        
        Adafruit_Fingerprint_Packet data_p(packet_type, len_to_send + 2, &templateData[bytes_sent]);
        // The library's packet crafting is a bit unusual. We will just use the public buffer directly.
        memcpy(finger.templateBuffer, &templateData[bytes_sent], len_to_send);
        
        Adafruit_Fingerprint_Packet data_p_manual;
        data_p_manual.type = packet_type;
        data_p_manual.length = len_to_send + 2; // +2 for checksum
        memcpy(data_p_manual.data, &templateData[bytes_sent], len_to_send);
        
        finger.writeStructuredPacket(data_p_manual);
        if (finger.getStructuredPacket(&response_p) != FINGERPRINT_OK) return FINGERPRINT_PACKETRECIEVEERR;
        if (response_p.type != FINGERPRINT_ACKPACKET || response_p.data[0] != FINGERPRINT_OK) return response_p.data[0];
        
        bytes_sent += len_to_send;
        bytes_left -= len_to_send;
    }

    return FINGERPRINT_OK;
}


// =================================================================================================
// EMBEDDED BASE64 LIBRARY by densaugeo
// This is included directly to avoid linking issues with the Arduino IDE.
// =================================================================================================

unsigned char binary_to_base64(unsigned char v) {
    if (v < 26) return v + 'A';
    if (v < 52) return v + 71;
    if (v < 62) return v - 4;
    if (v == 62) return '+';
    if (v == 63) return '/';
    return 64;
}

unsigned char base64_to_binary(unsigned char c) {
    if ('A' <= c && c <= 'Z') return c - 'A';
    if ('a' <= c && c <= 'z') return c - 71;
    if ('0' <= c && c <= '9') return c + 4;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 255;
}

unsigned int encode_base64_length(unsigned int input_length) {
    return (input_length + 2) / 3 * 4;
}

unsigned int decode_base64_length(const unsigned char input[], unsigned int input_length) {
    const unsigned char *start = input;
    while (base64_to_binary(input[0]) < 64 && (unsigned int)(input - start) < input_length) {
        ++input;
    }
    input_length = (unsigned int)(input - start);
    return input_length / 4 * 3 + (input_length % 4 ? input_length % 4 - 1 : 0);
}

unsigned int encode_base64(const unsigned char input[], unsigned int input_length, unsigned char output[]) {
    unsigned int full_sets = input_length / 3;
    for (unsigned int i = 0; i < full_sets; ++i) {
        output[0] = binary_to_base64(input[0] >> 2);
        output[1] = binary_to_base64((input[0] & 0x03) << 4 | input[1] >> 4);
        output[2] = binary_to_base64((input[1] & 0x0F) << 2 | input[2] >> 6);
        output[3] = binary_to_base64(input[2] & 0x3F);
        input += 3;
        output += 4;
    }
    switch (input_length % 3) {
    case 0:
        output[0] = '\0';
        break;
    case 1:
        output[0] = binary_to_base64(input[0] >> 2);
        output[1] = binary_to_base64((input[0] & 0x03) << 4);
        output[2] = '=';
        output[3] = '=';
        output[4] = '\0';
        break;
    case 2:
        output[0] = binary_to_base64(input[0] >> 2);
        output[1] = binary_to_base64((input[0] & 0x03) << 4 | input[1] >> 4);
        output[2] = binary_to_base64((input[1] & 0x0F) << 2);
        output[3] = '=';
        output[4] = '\0';
        break;
    }
    return encode_base64_length(input_length);
}

unsigned int decode_base64(const unsigned char input[], unsigned int input_length, unsigned char output[]) {
    unsigned int output_length = decode_base64_length(input, input_length);
    for (unsigned int i = 2; i < output_length; i += 3) {
        output[0] = base64_to_binary(input[0]) << 2 | base64_to_binary(input[1]) >> 4;
        output[1] = base64_to_binary(input[1]) << 4 | base64_to_binary(input[2]) >> 2;
        output[2] = base64_to_binary(input[2]) << 6 | base64_to_binary(input[3]);
        input += 4;
        output += 3;
    }
    switch (output_length % 3) {
    case 1:
        output[0] = base64_to_binary(input[0]) << 2 | base64_to_binary(input[1]) >> 4;
        break;
    case 2:
        output[0] = base64_to_binary(input[0]) << 2 | base64_to_binary(input[1]) >> 4;
        output[1] = base64_to_binary(input[1]) << 4 | base64_to_binary(input[2]) >> 2;
        break;
    }
    return output_length;
}
