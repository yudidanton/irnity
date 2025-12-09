#include <Wire.h>
#include <VL53L0X.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
#include <qrcode.h>
#include <Ticker.h>
#include <ArduinoJson.h>

// ===== PRODUCTION CONFIGURATION =====
#define FIRMWARE_VERSION "3.1.0"
#define MODEL_NUMBER "KW-2024-PRO"
#define SERIAL_NUMBER "KW" + String(ESP.getChipId())

// ===== HARDWARE DEFINITIONS =====
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C

#define RELAY_PIN D1
#define LED_PIN D2
#define SDA_PIN D6
#define SCL_PIN D7
#define XSHUT_1 D3
#define XSHUT_2 D4
#define FACTORY_RESET_PIN D5

// ===== SYSTEM COMPONENTS =====
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
VL53L0X sensor1, sensor2;
ESP8266WebServer server(80);
Ticker watchdogTicker;

// ===== SECURITY CONFIGURATION =====
const char* ap_ssid = "WudhuConfig";
String ap_password;
String admin_password;
String device_id;

// ===== EEPROM LAYOUT =====
#define EEPROM_SIZE 1024
#define EEPROM_SIGNATURE 0xAA55

typedef struct {
  uint16_t signature;
  uint16_t version;
  uint32_t total_activations;
  uint32_t total_water_duration;
  uint16_t hand_min_distance;
  uint16_t hand_max_distance;
  uint16_t swipe_distance;
  uint32_t water_off_delay;
  uint32_t max_water_time;
  uint8_t admin_password[32];
  uint32_t operation_hours;
  uint16_t error_count;
  uint16_t crc;
} SystemConfig;

SystemConfig config;

// ===== SYSTEM STATE =====
enum SystemMode { 
  MODE_AUTO, 
  MODE_MANUAL, 
  MODE_DEGRADED, 
  MODE_MAINTENANCE,
  MODE_ERROR 
};

enum WaterState {
  WATER_OFF,
  WATER_ON,
  WATER_TURNING_OFF
};

struct SystemState {
  SystemMode mode;
  WaterState water_state;
  bool ap_mode;
  bool authenticated;
  bool factory_reset_pending;
  unsigned long operation_start_time;
};

SystemState system_state = { MODE_AUTO, WATER_OFF, false, false, false, 0 };

// ===== TIMING & DEBOUNCE =====
struct Timing {
  unsigned long last_swipe_time;
  unsigned long water_start_time;
  unsigned long hand_detection_time;
  unsigned long last_display_update;
  unsigned long ap_mode_start_time;
  unsigned long last_error_time;
  unsigned long last_maintenance;
  unsigned long last_sensor_read;
  unsigned long last_watchdog_feed;
  unsigned long last_config_save;
};

Timing timing = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// ===== CONSTANTS =====
const unsigned long DEBOUNCE_TIME = 300;
const unsigned long AP_TIMEOUT = 300000;
const unsigned long SCREEN_INTERVAL = 5000;
const unsigned long ERROR_RECOVERY_INTERVAL = 10000;
const unsigned long MAINTENANCE_INTERVAL = 3600000;
const unsigned long WATCHDOG_INTERVAL = 8000;
const unsigned long SENSOR_READ_INTERVAL = 50;
const unsigned long CONFIG_SAVE_INTERVAL = 300000; // 5 minutes

// ===== SENSOR MANAGEMENT =====
struct SensorData {
  uint16_t distance1;
  uint16_t distance2;
  bool sensor1_healthy;
  bool sensor2_healthy;
  uint8_t error_count;
  uint16_t consecutive_errors;
};

SensorData sensors = {0, 0, true, true, 0, 0};

// ===== SWIPE DETECTION =====
struct SwipeDetection {
  bool in_swipe_zone;
  unsigned long swipe_start_time;
  uint16_t last_distance;
  uint8_t swipe_count;
  unsigned long first_swipe_time;
  bool swipe_detected;
};

SwipeDetection swipe = {false, 0, 1000, 0, 0, false};

// ===== STATISTICS & LOGGING =====
struct Statistics {
  uint32_t sensor_timeouts;
  uint32_t recovery_attempts;
  uint32_t successful_recoveries;
  uint32_t max_water_time_hits;
  uint32_t factory_resets;
};

Statistics stats = {0, 0, 0, 0, 0};

// ===== SECURITY =====
struct SecurityState {
  uint8_t failed_attempts;
  unsigned long lockout_until;
};

SecurityState security = {0, 0};

// ===== EVENT LOG =====
#define LOG_SIZE 50
struct LogEntry {
  unsigned long timestamp;
  char event[32];
};

LogEntry event_log[LOG_SIZE];
uint8_t log_index = 0;

// ===== WATCHDOG & SAFETY =====
volatile bool watchdog_triggered = false;
static SystemConfig last_saved_config;

void ICACHE_RAM_ATTR watchdogISR() {
  watchdog_triggered = true;
}

void feedWatchdog() {
  timing.last_watchdog_feed = millis();
  watchdog_triggered = false;
}

void safeRestart() {
  Serial.println("SAFE RESTART INITIATED");
  
  // Ensure water is off
  digitalWrite(RELAY_PIN, HIGH);
  delay(100);
  
  // Save critical data
  saveConfiguration();
  
  // Log restart
  logError("SYSTEM_RESTART");
  
  // Wait for serial
  Serial.flush();
  delay(100);
  
  ESP.restart();
}

// ===== INITIALIZATION =====
void initializeProductionSystem() {
  Serial.begin(115200);
  Serial.println(F("\n=== KRAN WUDHU PRODUCTION SYSTEM ==="));
  Serial.print(F("Model: ")); Serial.println(MODEL_NUMBER);
  Serial.print(F("Firmware: ")); Serial.println(FIRMWARE_VERSION);
  Serial.print(F("Serial: ")); Serial.println(SERIAL_NUMBER);
  
  initializeHardware();
  
  if (!loadConfiguration()) {
    initializeFactoryDefaults();
  }
  
  initializeSecurity();
  initializeDisplay();
  
  if (!initializeSensorsWithRetry(3)) {
    system_state.mode = MODE_ERROR;
    logError("SENSOR_INIT_FAILED");
  }
  
  setupWatchdog();
  
  system_state.operation_start_time = millis();
  
  Serial.println(F("=== SYSTEM INITIALIZATION COMPLETE ==="));
}

void initializeHardware() {
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);
  pinMode(XSHUT_1, OUTPUT);
  pinMode(XSHUT_2, OUTPUT);
  pinMode(FACTORY_RESET_PIN, INPUT_PULLUP);
  
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(LED_PIN, LOW);
  digitalWrite(XSHUT_1, LOW);
  digitalWrite(XSHUT_2, LOW);
  
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(400000);
  
  delay(100);
}

bool initializeSensorsWithRetry(uint8_t max_retries) {
  for (uint8_t attempt = 1; attempt <= max_retries; attempt++) {
    Serial.printf("Sensor init attempt %d/%d\n", attempt, max_retries);
    
    if (initializeSensors()) {
      Serial.println(F("Sensors initialized successfully"));
      return true;
    }
    
    if (attempt < max_retries) {
      delay(1000 * attempt);
    }
  }
  
  return false;
}

bool initializeSensors() {
  digitalWrite(XSHUT_1, HIGH);
  digitalWrite(XSHUT_2, LOW);
  delay(50);
  
  if (!sensor1.init()) {
    Serial.println(F("Sensor 1 init failed"));
    return false;
  }
  
  sensor1.setAddress(0x30);
  sensor1.setTimeout(500);
  sensor1.setMeasurementTimingBudget(50000);
  sensor1.startContinuous(50);
  
  digitalWrite(XSHUT_2, HIGH);
  delay(50);
  
  if (!sensor2.init()) {
    Serial.println(F("Sensor 2 init failed"));
    return false;
  }
  
  sensor2.setAddress(0x31);
  sensor2.setTimeout(500);
  sensor2.setMeasurementTimingBudget(50000);
  sensor2.startContinuous(50);
  
  return true;
}

void initializeDisplay() {
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println(F("OLED init failed"));
    return;
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  showBootScreen();
  delay(2000);
}

void initializeSecurity() {
  device_id = String(ESP.getChipId());
  
  // Generate secure AP password
  uint32_t seed = ESP.getChipId() ^ ESP.getCycleCount();
  randomSeed(seed);
  ap_password = "Wudhu_" + String(random(100000, 999999));
  
  admin_password = String((char*)config.admin_password);
  
  if (admin_password.length() == 0) {
    admin_password = "Admin_" + device_id;
    strncpy((char*)config.admin_password, admin_password.c_str(), sizeof(config.admin_password) - 1);
    config.admin_password[sizeof(config.admin_password) - 1] = '\0';
    saveConfiguration();
  }
}

// ===== CONFIGURATION MANAGEMENT =====
bool loadConfiguration() {
  EEPROM.begin(EEPROM_SIZE);
  
  EEPROM.get(0, config);
  
  if (config.signature != EEPROM_SIGNATURE) {
    Serial.println(F("Invalid EEPROM signature"));
    return false;
  }
  
  // Validate CRC
  uint16_t calculated_crc = calculateCRC((uint8_t*)&config, sizeof(config) - sizeof(config.crc));
  if (calculated_crc != config.crc) {
    Serial.println(F("CRC mismatch"));
    return false;
  }
  
  // Validate ranges
  config.hand_min_distance = constrain(config.hand_min_distance, 50, 200);
  config.hand_max_distance = constrain(config.hand_max_distance, config.hand_min_distance + 50, 1000);
  config.swipe_distance = constrain(config.swipe_distance, 20, 100);
  config.water_off_delay = constrain(config.water_off_delay, 500, 10000);
  config.max_water_time = constrain(config.max_water_time, 30000, 1800000);
  
  // Copy to last_saved for change detection
  memcpy(&last_saved_config, &config, sizeof(config));
  
  Serial.println(F("Configuration loaded"));
  return true;
}

void initializeFactoryDefaults() {
  Serial.println(F("Initializing factory defaults"));
  
  config.signature = EEPROM_SIGNATURE;
  config.version = 0x0310;
  config.total_activations = 0;
  config.total_water_duration = 0;
  config.hand_min_distance = 80;
  config.hand_max_distance = 300;
  config.swipe_distance = 50;
  config.water_off_delay = 1500;
  config.max_water_time = 600000;
  config.operation_hours = 0;
  config.error_count = 0;
  memset(config.admin_password, 0, sizeof(config.admin_password));
  
  saveConfiguration();
}

void saveConfiguration() {
  config.crc = calculateCRC((uint8_t*)&config, sizeof(config) - sizeof(config.crc));
  
  EEPROM.put(0, config);
  EEPROM.commit();
  
  memcpy(&last_saved_config, &config, sizeof(config));
  timing.last_config_save = millis();
  
  Serial.println(F("Configuration saved"));
}

void saveConfigurationIfChanged() {
  if (memcmp(&config, &last_saved_config, sizeof(config)) != 0) {
    saveConfiguration();
  }
}

uint16_t calculateCRC(uint8_t* data, size_t length) {
  uint16_t crc = 0xFFFF;
  
  for (size_t i = 0; i < length; i++) {
    crc ^= (uint16_t)data[i] << 8;
    
    for (uint8_t bit = 0; bit < 8; bit++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  
  return crc;
}

// ===== MAIN LOOP =====
void loop() {
  feedWatchdog();
  
  checkFactoryReset();
  
  if (system_state.ap_mode) {
    handleAPMode();
    return;
  }
  
  if (system_state.mode == MODE_ERROR) {
    handleErrorState();
    return;
  }
  
  readSensors();
  detectSwipeGesture();
  handleWaterControl();
  updateDisplay();
  updateLED();
  performMaintenance();
  
  delay(SENSOR_READ_INTERVAL);
}

void readSensors() {
  if (millis() - timing.last_sensor_read < SENSOR_READ_INTERVAL) {
    return;
  }
  
  timing.last_sensor_read = millis();
  
  // Read sensors with error handling
  sensors.distance1 = sensor1.readRangeContinuousMillimeters();
  sensors.distance2 = sensor2.readRangeContinuousMillimeters();
  
  bool sensor1_error = sensor1.timeoutOccurred() || sensors.distance1 > 8000;
  bool sensor2_error = sensor2.timeoutOccurred() || sensors.distance2 > 8000;
  
  // FIX: Update sensor health correctly
  if (sensor1_error) {
    sensors.sensor1_healthy = false;
    stats.sensor_timeouts++;
  } else {
    sensors.sensor1_healthy = true;
  }
  
  if (sensor2_error) {
    sensors.sensor2_healthy = false;
    stats.sensor_timeouts++;
  } else {
    sensors.sensor2_healthy = true; // FIX: Was always set to false!
  }
  
  // FIX: Only reset consecutive errors if BOTH sensors are OK
  if (sensor1_error || sensor2_error) {
    sensors.consecutive_errors++;
  } else {
    sensors.consecutive_errors = 0;
  }
  
  // Handle degraded mode
  if (sensors.consecutive_errors >= 10) {
    if (system_state.mode != MODE_DEGRADED && system_state.mode != MODE_ERROR) {
      system_state.mode = MODE_DEGRADED;
      logError("DEGRADED_MODE_ENTERED");
    }
  }
}

void detectSwipeGesture() {
  if (sensors.distance1 < config.swipe_distance && !swipe.in_swipe_zone) {
    swipe.in_swipe_zone = true;
    swipe.swipe_start_time = millis();
    swipe.last_distance = sensors.distance1;
  }
  
  if (swipe.in_swipe_zone && sensors.distance1 < config.swipe_distance) {
    swipe.last_distance = sensors.distance1;
    
    if (millis() - swipe.swipe_start_time > 1000) {
      swipe.in_swipe_zone = false;
      swipe.swipe_count = 0;
    }
  }
  
  if (sensors.distance1 >= config.swipe_distance + 30 && swipe.in_swipe_zone) {
    unsigned long swipe_duration = millis() - swipe.swipe_start_time;
    
    if (swipe_duration >= 100 && swipe_duration <= 1000) {
      handleValidSwipe();
    }
    
    swipe.in_swipe_zone = false;
  }
}

void handleValidSwipe() {
  if (swipe.swipe_count == 0) {
    swipe.first_swipe_time = millis();
    swipe.swipe_count = 1;
  } else if (millis() - swipe.first_swipe_time < 3000) {
    swipe.swipe_count++;
    
    if (swipe.swipe_count >= 5) {
      activateAPMode();
      return;
    }
  } else {
    swipe.swipe_count = 1;
    swipe.first_swipe_time = millis();
  }
  
  if (millis() - timing.last_swipe_time > DEBOUNCE_TIME) {
    timing.last_swipe_time = millis();
    
    if (system_state.mode == MODE_AUTO) {
      system_state.mode = MODE_MANUAL;
      turnWaterOn();
      logEvent("MANUAL_MODE_ACTIVATED");
    } else if (system_state.mode == MODE_MANUAL) {
      system_state.mode = MODE_AUTO;
      turnWaterOff();
      logEvent("AUTO_MODE_ACTIVATED");
    }
  }
}

void handleWaterControl() {
  switch (system_state.mode) {
    case MODE_AUTO:
      handleAutoMode();
      break;
      
    case MODE_MANUAL:
      handleManualMode();
      break;
      
    case MODE_DEGRADED:
      handleDegradedMode();
      break;
      
    default:
      break;
  }
  
  if (system_state.water_state == WATER_ON) {
    unsigned long water_duration = millis() - timing.water_start_time;
    
    if (water_duration >= (config.max_water_time - 60000) && 
        water_duration < (config.max_water_time - 59000)) {
      displayWarning("TIMEOUT_WARNING");
    }
    
    if (water_duration >= config.max_water_time) {
      turnWaterOff();
      system_state.mode = MODE_AUTO;
      stats.max_water_time_hits++;
      logEvent("SAFETY_TIMEOUT");
    }
  }
}

void handleAutoMode() {
  if (sensors.sensor2_healthy) {
    if (sensors.distance2 >= config.hand_min_distance && 
        sensors.distance2 <= config.hand_max_distance) {
      
      if (system_state.water_state != WATER_ON) {
        turnWaterOn();
      }
    } else if (system_state.water_state == WATER_ON) {
      if (system_state.water_state != WATER_TURNING_OFF) {
        system_state.water_state = WATER_TURNING_OFF;
        timing.hand_detection_time = millis();
      }
      
      if (millis() - timing.hand_detection_time >= config.water_off_delay) {
        turnWaterOff();
      }
    }
  }
}

void handleManualMode() {
  // Manual mode keeps water on until mode switch
}

void handleDegradedMode() {
  if (millis() - timing.last_error_time > ERROR_RECOVERY_INTERVAL) {
    attemptSensorRecovery();
  }
}

// ===== WATER CONTROL =====
void turnWaterOn() {
  if (system_state.water_state != WATER_ON) {
    digitalWrite(RELAY_PIN, LOW);
    system_state.water_state = WATER_ON;
    timing.water_start_time = millis();
    config.total_activations++;
    
    logEvent("WATER_ON");
  }
}

void turnWaterOff() {
  if (system_state.water_state != WATER_OFF) {
    digitalWrite(RELAY_PIN, HIGH);
    
    if (timing.water_start_time > 0) {
      config.total_water_duration += (millis() - timing.water_start_time) / 1000;
    }
    
    system_state.water_state = WATER_OFF;
    logEvent("WATER_OFF");
  }
}

// ===== ERROR HANDLING =====
void handleErrorState() {
  digitalWrite(RELAY_PIN, HIGH);
  
  static unsigned long last_blink = 0;
  if (millis() - last_blink > 500) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    last_blink = millis();
  }
  
  if (millis() - timing.last_error_time > 30000) {
    if (attemptSensorRecovery()) {
      system_state.mode = MODE_AUTO;
      logEvent("ERROR_RECOVERY_OK");
    }
    timing.last_error_time = millis();
  }
  
  updateErrorDisplay();
}

bool attemptSensorRecovery() {
  stats.recovery_attempts++;
  
  Serial.println(F("Attempting recovery..."));
  
  digitalWrite(XSHUT_1, LOW);
  digitalWrite(XSHUT_2, LOW);
  delay(100);
  
  if (initializeSensors()) {
    sensors.consecutive_errors = 0;
    sensors.sensor1_healthy = true;
    sensors.sensor2_healthy = true;
    stats.successful_recoveries++;
    
    logEvent("RECOVERY_SUCCESS");
    return true;
  }
  
  logError("RECOVERY_FAILED");
  return false;
}

// ===== AP MODE =====
void activateAPMode() {
  system_state.ap_mode = true;
  timing.ap_mode_start_time = millis();
  swipe.swipe_count = 0;
  
  turnWaterOff();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password.c_str());
  
  Serial.println(F("AP Mode activated"));
  Serial.print(F("SSID: ")); Serial.println(ap_ssid);
  Serial.print(F("Pass: ")); Serial.println(ap_password);
  Serial.print(F("IP: ")); Serial.println(WiFi.softAPIP());
  
  setupWebServer();
  server.begin();
  
  displayAPMode();
  logEvent("AP_MODE_ON");
}

void handleAPMode() {
  server.handleClient();
  
  if (millis() - timing.ap_mode_start_time > AP_TIMEOUT) {
    exitAPMode();
  }
  
  static unsigned long last_ap_led = 0;
  if (millis() - last_ap_led > 200) {
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
    last_ap_led = millis();
  }
}

void exitAPMode() {
  system_state.ap_mode = false;
  system_state.authenticated = false;
  security.failed_attempts = 0;
  
  server.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  
  Serial.println(F("AP Mode exited"));
  logEvent("AP_MODE_OFF");
  
  saveConfigurationIfChanged();
  
  displayStatus("Restarting...");
  delay(2000);
  ESP.restart();
}

// ===== DISPLAY MANAGEMENT =====
void updateDisplay() {
  if (millis() - timing.last_display_update < 100) return;
  timing.last_display_update = millis();
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  
  if (system_state.water_state == WATER_ON || system_state.water_state == WATER_TURNING_OFF) {
    displayActiveScreen();
  } else {
    displayStandbyScreen();
  }
  
  display.display();
}

void displayActiveScreen() {
  unsigned long duration = (millis() - timing.water_start_time) / 1000;
  unsigned long remaining = (config.max_water_time / 1000) - duration;
  
  display.setTextSize(1);
  display.setCursor(0, 0);
  
  switch (system_state.mode) {
    case MODE_AUTO: display.print(F("AUTO")); break;
    case MODE_MANUAL: display.print(F("MANUAL")); break;
    case MODE_DEGRADED: display.print(F("DEGRADED")); break;
    default: display.print(F("UNKNOWN")); break;
  }
  
  display.setCursor(70, 0);
  if (system_state.water_state == WATER_TURNING_OFF) {
    unsigned long rem = config.water_off_delay - (millis() - timing.hand_detection_time);
    display.print(F("OFF:"));
    display.print(rem / 100);
    display.print(F("s"));
  } else {
    display.print(F("ON "));
    if (millis() % 500 < 250) {
      display.fillCircle(120, 4, 2, SSD1306_WHITE);
    }
  }
  
  display.drawLine(0, 10, 128, 10, SSD1306_WHITE);
  
  display.setTextSize(2);
  display.setCursor(20, 16);
  if (duration < 60) {
    display.print(F("0:"));
    if (duration < 10) display.print(F("0"));
    display.print(duration);
  } else {
    display.print(duration / 60);
    display.print(F(":"));
    int secs = duration % 60;
    if (secs < 10) display.print(F("0"));
    display.print(secs);
  }
  
  int barWidth = map(duration, 0, config.max_water_time / 1000, 0, 128);
  barWidth = constrain(barWidth, 0, 128);
  display.drawRect(0, 40, 128, 10, SSD1306_WHITE);
  display.fillRect(0, 40, barWidth, 10, SSD1306_WHITE);
  
  display.setTextSize(1);
  display.setCursor(0, 52);
  display.print(F("Sisa: "));
  display.print(remaining / 60);
  display.print(F("m "));
  display.print(remaining % 60);
  display.print(F("s"));
  
  if (remaining < 60 && millis() % 1000 < 500) {
    display.setCursor(70, 52);
    display.print(F("WARNING!"));
  }
}

void displayStandbyScreen() {
  static uint8_t screen_index = 0;
  static unsigned long last_screen_change = 0;
  
  if (millis() - last_screen_change > SCREEN_INTERVAL) {
    screen_index = (screen_index + 1) % 4;
    last_screen_change = millis();
  }
  
  display.setTextSize(1);
  
  switch (screen_index) {
    case 0:
      display.setTextSize(2);
      display.setCursor(5, 5);
      display.println(F("WUDHU"));
      display.setCursor(5, 25);
      display.println(F("OTOMATIS"));
      
      display.setTextSize(1);
      display.setCursor(10, 45);
      display.println(F("PRODUCTION"));
      display.setCursor(30, 55);
      display.println(F("READY"));
      break;
      
    case 1:
      display.setTextSize(2);
      display.setCursor(10, 5);
      display.println(F("AUTO MODE"));
      
      display.setTextSize(1);
      display.setCursor(5, 30);
      display.println(F("Sensor aktif"));
      display.setCursor(5, 45);
      display.println(F("Deteksi otomatis"));
      break;
      
    case 2:
      display.setTextSize(2);
      display.setCursor(0, 5);
      display.println(F("MANUAL"));
      display.setCursor(0, 25);
      display.println(F("MODE"));
      
      display.setTextSize(1);
      display.setCursor(5, 45);
      display.println(F("Swipe samping"));
      display.setCursor(20, 55);
      display.println(F("ON / OFF"));
      break;
      
    case 3:
      displayStatsScreen();
      break;
  }
}

void displayStatsScreen() {
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(F("STATISTICS:"));
  
  display.setCursor(0, 12);
  display.print(F("Aktivasi: "));
  display.println(config.total_activations);
  
  display.setCursor(0, 22);
  display.print(F("Waktu: "));
  display.print(config.total_water_duration / 60);
  display.println(F("m"));
  
  display.setCursor(0, 32);
  display.print(F("Error: "));
  display.println(config.error_count);
  
  display.setCursor(0, 42);
  display.print(F("Uptime: "));
  display.print((millis() - system_state.operation_start_time) / 3600000);
  display.println(F("h"));
  
  display.setCursor(0, 52);
  display.print(F("Mode: "));
  switch (system_state.mode) {
    case MODE_AUTO: display.println(F("AUTO")); break;
    case MODE_MANUAL: display.println(F("MANUAL")); break;
    case MODE_DEGRADED: display.println(F("DEGRADE")); break;
    default: display.println(F("ERROR")); break;
  }
}

void displayAPMode() {
  display.clearDisplay();
  display.setTextSize(1);
  
  display.setCursor(15, 0);
  display.println(F("CONFIG MODE"));
  
  QRCode qrcode;
  uint8_t qrcodeData[qrcode_getBufferSize(3)];
  String url = "http://" + WiFi.softAPIP().toString();
  qrcode_initText(&qrcode, qrcodeData, 3, 0, url.c_str());
  
  int offsetX = 32;
  int offsetY = 12;
  int scale = 2;
  
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        display.fillRect(offsetX + x*scale, offsetY + y*scale, scale, scale, SSD1306_WHITE);
      }
    }
  }
  
  display.setCursor(10, 56);
  display.println(F("Scan config"));
  
  display.display();
}

void showBootScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 10);
  display.println(F("KRAN"));
  display.setCursor(10, 30);
  display.println(F("WUDHU"));
  
  display.setTextSize(1);
  display.setCursor(5, 52);
  display.print(F("v"));
  display.print(FIRMWARE_VERSION);
  
  display.display();
}

void displayStatus(const char* msg) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.println(msg);
  display.display();
}

void displayWarning(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 15);
  display.println(F("WARNING!"));
  display.setTextSize(1);
  display.setCursor(20, 45);
  display.println(msg);
  display.display();
}

void displayError(const char* msg) {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 10);
  display.println(F("ERROR!"));
  display.setTextSize(1);
  display.setCursor(10, 35);
  display.println(msg);
  display.display();
}

void updateErrorDisplay() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20, 10);
  display.println(F("ERROR!"));
  
  display.setTextSize(1);
  display.setCursor(10, 35);
  display.println(F("System Error"));
  display.setCursor(10, 45);
  display.println(F("Recovery..."));
  
  display.display();
}

// ===== LED INDICATION =====
void updateLED() {
  if (system_state.ap_mode) {
    digitalWrite(LED_PIN, (millis() % 200) < 100);
  } else if (system_state.mode == MODE_MANUAL) {
    digitalWrite(LED_PIN, (millis() % 1000) < 500);
  } else if (system_state.water_state == WATER_ON) {
    digitalWrite(LED_PIN, HIGH);
  } else if (system_state.mode == MODE_DEGRADED) {
    digitalWrite(LED_PIN, (millis() % 1000) < 100);
  } else {
    digitalWrite(LED_PIN, LOW);
  }
}

// ===== WEB SERVER =====
void setupWebServer() {
  server.on("/", HTTP_GET, []() {
    if (!system_state.authenticated) {
      server.send(200, "text/html", getLoginPage());
    } else {
      server.send(200, "text/html", getDashboardPage());
    }
  });
  
  server.on("/login", HTTP_POST, []() {
    // FIX: Rate limiting
    if (security.failed_attempts >= 3) {
      if (millis() < security.lockout_until) {
        server.send(429, "text/plain", "Too many attempts. Try again later.");
        return;
      }
      security.failed_attempts = 0;
    }
    
    String user = server.arg("username");
    String pass = server.arg("password");
    
    if (user == "admin" && pass == admin_password) {
      system_state.authenticated = true;
      security.failed_attempts = 0;
      server.sendHeader("Location", "/");
      server.send(303);
      logEvent("LOGIN_SUCCESS");
    } else {
      security.failed_attempts++;
      security.lockout_until = millis() + (security.failed_attempts * 5000);
      server.send(200, "text/html", getLoginPage() + "<script>alert('Login gagal!');</script>");
      logEvent("LOGIN_FAILED");
    }
  });
  
  server.on("/api/config", HTTP_GET, []() {
    if (!system_state.authenticated) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    
    StaticJsonDocument<512> doc;
    doc["hand_min_distance"] = config.hand_min_distance;
    doc["hand_max_distance"] = config.hand_max_distance;
    doc["swipe_distance"] = config.swipe_distance;
    doc["water_off_delay"] = config.water_off_delay;
    doc["max_water_time"] = config.max_water_time;
    doc["total_activations"] = config.total_activations;
    doc["total_water_duration"] = config.total_water_duration;
    doc["operation_hours"] = config.operation_hours;
    doc["error_count"] = config.error_count;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
  
  server.on("/api/config", HTTP_POST, []() {
    if (!system_state.authenticated) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    
    StaticJsonDocument<512> doc;
    deserializeJson(doc, server.arg("plain"));
    
    config.hand_min_distance = doc["hand_min_distance"];
    config.hand_max_distance = doc["hand_max_distance"];
    config.swipe_distance = doc["swipe_distance"];
    config.water_off_delay = doc["water_off_delay"];
    config.max_water_time = doc["max_water_time"];
    
    saveConfiguration();
    
    server.send(200, "application/json", "{\"status\":\"success\"}");
  });
  
  server.on("/api/status", HTTP_GET, []() {
    StaticJsonDocument<512> doc;
    doc["mode"] = system_state.mode;
    doc["water_state"] = system_state.water_state;
    doc["sensor1_distance"] = sensors.distance1;
    doc["sensor2_distance"] = sensors.distance2;
    doc["sensor1_healthy"] = sensors.sensor1_healthy;
    doc["sensor2_healthy"] = sensors.sensor2_healthy;
    doc["uptime"] = millis() - system_state.operation_start_time;
    
    String response;
    serializeJson(doc, response);
    server.send(200, "application/json", response);
  });
  
  server.on("/api/reset", HTTP_POST, []() {
    if (!system_state.authenticated) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    
    config.total_activations = 0;
    config.total_water_duration = 0;
    config.operation_hours = 0;
    config.error_count = 0;
    
    saveConfiguration();
    
    server.send(200, "application/json", "{\"status\":\"success\"}");
  });
  
  server.on("/api/factory-reset", HTTP_POST, []() {
    if (!system_state.authenticated) {
      server.send(401, "text/plain", "Unauthorized");
      return;
    }
    
    system_state.factory_reset_pending = true;
    server.send(200, "application/json", "{\"status\":\"pending\"}");
  });
  
  server.on("/exit", HTTP_GET, []() {
    server.send(200, "text/html", "<html><body><h2>Exiting...</h2></body></html>");
    delay(1000);
    exitAPMode();
  });
}

String getLoginPage() {
  String html = F("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Login</title><style>");
  html += F("body{font-family:Arial;text-align:center;padding:50px;background:#f0f0f0}");
  html += F(".container{background:white;padding:30px;border-radius:10px;max-width:400px;margin:0 auto;box-shadow:0 2px 10px rgba(0,0,0,0.1)}");
  html += F("input{width:90%;padding:12px;margin:10px 0;border:1px solid #ddd;border-radius:5px}");
  html += F("button{width:95%;padding:12px;background:#4CAF50;color:white;border:none;border-radius:5px;cursor:pointer;font-size:16px}");
  html += F("button:hover{background:#45a049}");
  html += F("h1{color:#333}");
  html += F(".info{margin-top:20px;padding:10px;background:#f8f8f8;border-radius:5px;font-size:12px}");
  html += F("</style></head><body>");
  html += F("<div class=\"container\"><h1>🕌 KRAN WUDHU</h1><h3>Production System</h3>");
  html += F("<form action=\"/login\" method=\"POST\">");
  html += F("<input type=\"text\" name=\"username\" placeholder=\"Username\" value=\"admin\" required>");
  html += F("<input type=\"password\" name=\"password\" placeholder=\"Password\" required>");
  html += F("<button type=\"submit\">LOGIN</button></form>");
  html += F("<div class=\"info\">Device: ");
  html += device_id;
  html += F("<br>IP: ");
  html += WiFi.softAPIP().toString();
  html += F("<br>Pass: ");
  html += ap_password;
  html += F("</div></div></body></html>");
  return html;
}

String getDashboardPage() {
  String html = F("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">");
  html += F("<title>Dashboard</title><style>");
  html += F("body{font-family:Arial;padding:20px;background:#f0f0f0}");
  html += F(".container{background:white;padding:20px;border-radius:10px;max-width:800px;margin:0 auto;box-shadow:0 2px 10px rgba(0,0,0,0.1)}");
  html += F("h1{color:#333;text-align:center}");
  html += F(".section{margin:20px 0;padding:15px;border:1px solid #ddd;border-radius:5px}");
  html += F(".section h3{margin-top:0;color:#4CAF50}");
  html += F("input[type=number]{width:100px;padding:8px;margin:5px;border:1px solid #ddd;border-radius:3px}");
  html += F("button{padding:10px 20px;margin:5px;border:none;border-radius:5px;cursor:pointer;font-size:14px}");
  html += F(".btn-save{background:#4CAF50;color:white}");
  html += F(".btn-reset{background:#f44336;color:white}");
  html += F(".btn-factory{background:#ff9800;color:white}");
  html += F(".btn-exit{background:#2196F3;color:white}");
  html += F(".status{display:flex;justify-content:space-around;text-align:center;flex-wrap:wrap}");
  html += F(".stat-box{padding:15px;background:#e8f5e9;border-radius:5px;flex:1;margin:5px;min-width:120px}");
  html += F("label{display:inline-block;width:180px;text-align:right;margin-right:10px}");
  html += F(".sensor-status{display:flex;justify-content:space-around;margin:10px 0}");
  html += F(".sensor-box{padding:10px;border-radius:5px;text-align:center}");
  html += F(".sensor-healthy{background:#c8e6c9}");
  html += F(".sensor-error{background:#ffcdd2}");
  html += F("</style></head><body>");
  html += F("<div class=\"container\"><h1>🕌 KRAN WUDHU - Dashboard</h1>");
  
  html += F("<div class=\"section\"><h3>📊 System Status</h3>");
  html += F("<div class=\"sensor-status\">");
  html += F("<div class=\"sensor-box ");
  html += sensors.sensor1_healthy ? F("sensor-healthy") : F("sensor-error");
  html += F("\"><h4>Sensor Bawah</h4><div>");
  html += String(sensors.distance1);
  html += F(" mm</div><div>");
  html += sensors.sensor1_healthy ? F("HEALTHY") : F("ERROR");
  html += F("</div></div>");
  
  html += F("<div class=\"sensor-box ");
  html += sensors.sensor2_healthy ? F("sensor-healthy") : F("sensor-error");
  html += F("\"><h4>Sensor Atas</h4><div>");
  html += String(sensors.distance2);
  html += F(" mm</div><div>");
  html += sensors.sensor2_healthy ? F("HEALTHY") : F("ERROR");
  html += F("</div></div></div>");
  
  html += F("<div class=\"status\">");
  html += F("<div class=\"stat-box\"><h4>Aktivasi</h4><h2>");
  html += String(config.total_activations);
  html += F("</h2></div>");
  html += F("<div class=\"stat-box\"><h4>Durasi</h4><h2>");
  html += String(config.total_water_duration / 60);
  html += F("m</h2></div>");
  html += F("<div class=\"stat-box\"><h4>Jam Operasi</h4><h2>");
  html += String(config.operation_hours);
  html += F("</h2></div>");
  html += F("<div class=\"stat-box\"><h4>Error</h4><h2>");
  html += String(config.error_count);
  html += F("</h2></div></div></div>");
  
  html += F("<div class=\"section\"><h3>⚙️ Configuration</h3>");
  html += F("<form id=\"cfg\"><p><label>Sensor Atas Min:</label><input type=\"number\" id=\"hmin\" value=\"");
  html += String(config.hand_min_distance);
  html += F("\"> mm</p>");
  html += F("<p><label>Sensor Atas Max:</label><input type=\"number\" id=\"hmax\" value=\"");
  html += String(config.hand_max_distance);
  html += F("\"> mm</p>");
  html += F("<p><label>Swipe Distance:</label><input type=\"number\" id=\"sdist\" value=\"");
  html += String(config.swipe_distance);
  html += F("\"> mm</p>");
  html += F("<p><label>Water Off Delay:</label><input type=\"number\" id=\"doff\" value=\"");
  html += String(config.water_off_delay);
  html += F("\"> ms</p>");
  html += F("<p><label>Max Time:</label><input type=\"number\" id=\"mtime\" value=\"");
  html += String(config.max_water_time / 60000);
  html += F("\"> min</p>");
  html += F("<button type=\"button\" class=\"btn-save\" onclick=\"save()\">💾 Save</button></form></div>");
  
  html += F("<div class=\"section\"><h3>🛠️ Maintenance</h3>");
  html += F("<button class=\"btn-reset\" onclick=\"reset()\">🔄 Reset Stats</button>");
  html += F("<button class=\"btn-factory\" onclick=\"factory()\">🏭 Factory Reset</button>");
  html += F("<button class=\"btn-exit\" onclick=\"exit()\">🚪 Exit AP</button></div></div>");
  
  html += F("<script>");
  html += F("function save(){");
  html += F("const c={hand_min_distance:parseInt(document.getElementById('hmin').value),");
  html += F("hand_max_distance:parseInt(document.getElementById('hmax').value),");
  html += F("swipe_distance:parseInt(document.getElementById('sdist').value),");
  html += F("water_off_delay:parseInt(document.getElementById('doff').value),");
  html += F("max_water_time:parseInt(document.getElementById('mtime').value)*60000};");
  html += F("fetch('/api/config',{method:'POST',body:JSON.stringify(c)})");
  html += F(".then(r=>r.json()).then(d=>alert('Saved!'));}");
  html += F("function reset(){if(confirm('Reset stats?')){");
  html += F("fetch('/api/reset',{method:'POST'}).then(r=>r.json()).then(d=>{alert('Reset!');location.reload();});}}");
  html += F("function factory(){if(confirm('Factory reset ALL?')){");
  html += F("fetch('/api/factory-reset',{method:'POST'}).then(r=>r.json()).then(d=>{alert('Resetting...');setTimeout(()=>location.reload(),3000);});}}");
  html += F("function exit(){if(confirm('Exit AP?')){location.href='/exit';}}");
  html += F("setInterval(()=>{fetch('/api/status').then(r=>r.json()).then(d=>console.log(d));},5000);");
  html += F("</script></body></html>");
  
  return html;
}

// ===== WATCHDOG =====
void setupWatchdog() {
  watchdogTicker.attach_ms(WATCHDOG_INTERVAL, []() {
    if (millis() - timing.last_watchdog_feed > WATCHDOG_INTERVAL * 2) {
      Serial.println(F("Watchdog timeout!"));
      safeRestart();
    }
  });
}

// ===== FACTORY RESET =====
void checkFactoryReset() {
  static unsigned long reset_press_time = 0;
  
  if (digitalRead(FACTORY_RESET_PIN) == LOW) {
    if (reset_press_time == 0) {
      reset_press_time = millis();
    }
    
    if (millis() - reset_press_time > 10000) {
      performFactoryReset();
    }
  } else {
    reset_press_time = 0;
  }
  
  if (system_state.factory_reset_pending) {
    performFactoryReset();
  }
}

void performFactoryReset() {
  Serial.println(F("FACTORY RESET"));
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println(F("FACTORY RESET"));
  display.setCursor(0, 35);
  display.println(F("Please wait..."));
  display.display();
  
  for (int i = 0; i < EEPROM_SIZE; i++) {
    EEPROM.write(i, 0);
  }
  EEPROM.commit();
  
  stats.factory_resets++;
  
  delay(1000);
  
  Serial.println(F("Reset complete"));
  ESP.restart();
}

// ===== LOGGING =====
void logEvent(const char* event) {
  Serial.printf("[EVENT] %s\n", event);
  logEventToBuffer(event);
}

void logError(const char* error) {
  Serial.printf("[ERROR] %s\n", error);
  config.error_count++;
  timing.last_error_time = millis();
  logEventToBuffer(error);
}

void logEventToBuffer(const char* event) {
  strncpy(event_log[log_index].event, event, sizeof(event_log[0].event) - 1);
  event_log[log_index].event[sizeof(event_log[0].event) - 1] = '\0';
  event_log[log_index].timestamp = millis();
  log_index = (log_index + 1) % LOG_SIZE;
}

void performMaintenance() {
  if (millis() - timing.last_maintenance > MAINTENANCE_INTERVAL) {
    timing.last_maintenance = millis();
    
    config.operation_hours = (millis() - system_state.operation_start_time) / 3600000;
    
    // FIX: Only save if changed
    if (millis() - timing.last_config_save > CONFIG_SAVE_INTERVAL) {
      saveConfigurationIfChanged();
    }
    
    Serial.println(F("Maintenance complete"));
  }
}

// ===== SETUP =====
void setup() {
  initializeProductionSystem();
}