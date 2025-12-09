/*
* ========================================
* SMART WATERING SYSTEM - ESP32 v3.0
* Latihan IoT IRMAL
* ========================================
* PIN ASSIGNMENT:
* - I2C (OLED & RTC): SDA=21, SCL=22
* - Rotary Encoder: CLK=25, DT=26, SW=27
* - Soil Sensor: Pin 34 (ADC1_CH6)
* - Relay (AKTIF LOW): Pin 32
* - LED: Pin 33
* - Push Button: Pin 14
* ========================================
*/
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <RTClib.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
// ========================================
// PIN DEFINITIONS
// ========================================
#define SDA_PIN 21
#define SCL_PIN 22
#define ENCODER_CLK 25
#define ENCODER_DT 26
#define ENCODER_SW 27
#define SOIL_SENSOR_PIN 34
#define RELAY_PIN 32
#define LED_PIN 33
#define BUTTON_PIN 14
// ========================================
// OLED SETUP
// ========================================
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire,
OLED_RESET);
// ========================================
// RTC SETUP
// ========================================
RTC_DS3231 rtc;
// ========================================
// WEB SERVER SETUP
// ========================================
const char* ssid = "latihaniotirmal";
const char* password = "pemudahijrah";
WebServer server(80);
// ========================================
// PREFERENCES
// ========================================
Preferences preferences;
// ========================================
// SYSTEM VARIABLES
// ========================================
enum Mode { MODE_MANUAL, MODE_AUTO, MODE_JADWAL };
Mode currentMode = MODE_AUTO;
bool pumpState = false;
unsigned long pumpStartTime = 0;
const unsigned long MAX_PUMP_DURATION = 5 * 60 * 1000;
int soilMoisture = 0;
const int SOIL_DRY_THRESHOLD = 60;
const int SOIL_WET_THRESHOLD = 30;
int dryThreshold = SOIL_DRY_THRESHOLD;
int wetThreshold = SOIL_WET_THRESHOLD;
const int SOIL_WET_RAW = 1300;
const int SOIL_DRY_RAW = 3200;
int schedule1Hour = 6;
int schedule1Minute = 0;
int schedule2Hour = 17;
int schedule2Minute = 0;
const unsigned long SCHEDULE_DURATION = 5 * 60 * 1000;
bool schedule1Done = false;
bool schedule2Done = false;
int lastDay = -1;
// Rotary Encoder
int encoderPos = 0;
int lastEncoderPos = 0;
volatile int encoderCounter = 0;
int lastCLK = HIGH;
bool buttonPressed = false;
unsigned long lastButtonTime = 0;
const unsigned long DEBOUNCE_DELAY = 200;
enum MenuState {
MENU_HOME,
MENU_SELECT_MODE,
MENU_MANUAL_MAIN,
MENU_MANUAL_SET_MODE,
MENU_MANUAL_SET_PAR,
MENU_AUTO_MAIN,
MENU_AUTO_SET_MODE,
MENU_AUTO_SET_PAR,
MENU_AUTO_SET_BASAH,
MENU_AUTO_SET_KERING,
MENU_AUTO_ADJUST_BASAH,
MENU_AUTO_ADJUST_KERING,
MENU_JADWAL_MAIN,
MENU_JADWAL_SET_MODE,
MENU_JADWAL_SET_PAR,
MENU_JADWAL_SET_J1,
MENU_JADWAL_SET_J2,
MENU_JADWAL_ADJUST_J1_JAM,
MENU_JADWAL_ADJUST_J1_MENIT,
MENU_JADWAL_ADJUST_J2_JAM,
MENU_JADWAL_ADJUST_J2_MENIT
};
MenuState menuState = MENU_HOME;
int menuSelection = 0;
int tempValue = 0;
bool lastButtonState = HIGH;
unsigned long lastManualButtonTime = 0;
unsigned long lastLoopTime = 0;
const unsigned long WATCHDOG_TIMEOUT = 10000;
bool rtcError = false;
// ========================================
// FUNCTION DECLARATIONS
// ========================================
void checkRTCStatus();
void readSoilMoisture();
void readEncoder();
void handleEncoderRotation();
void handleEncoderButton();
void readManualButton();
void runManualMode();
void runAutoMode();
void runJadwalMode();
void controlPump(bool state);
void checkPumpDuration();
void updateDisplay();
void saveSettings();
void handleRoot();
void handleStatus();
void handleControl();
void handleSettings();
// ========================================
// SETUP
// ========================================
void setup() {
    Serial.begin(115200);
    
    // *** DISABLE BROWNOUT DETECTOR ***
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    
    Serial.println("\n=== Latihan IoT IRMAL v3.0 ===");
    Serial.println("Brownout detector disabled");
    
    pinMode(RELAY_PIN, OUTPUT);
    pinMode(LED_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(ENCODER_CLK, INPUT_PULLUP);
    pinMode(ENCODER_DT, INPUT_PULLUP);
    pinMode(ENCODER_SW, INPUT_PULLUP);
    pinMode(SOIL_SENSOR_PIN, INPUT);
    
    // *** PASTIKAN RELAY OFF DULU ***
    digitalWrite(RELAY_PIN, HIGH);
    digitalWrite(LED_PIN, LOW);
    delay(100);
    
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(100000);  // *** Set I2C ke 100kHz ***
    
    if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println(F("OLED gagal!"));
        for(;;);
    }
    
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Latihan IoT");
    display.println("IRMAL");
    display.println();
    display.println("Smart Watering");
    display.println("v3.0");
    display.println("Initializing...");
    display.display();
    delay(2000);
    
    if (!rtc.begin()) {
        Serial.println("RTC tidak ditemukan!");
        rtcError = true;
        display.clearDisplay();
        display.setCursor(0, 0);
        display.println("RTC ERROR!");
        display.println();
        display.println("Mode JADWAL");
        display.println("tidak tersedia");
        display.display();
        delay(3000);
    } else {
        if (rtc.lostPower()) {
            Serial.println("RTC lost power, setting time...");
            rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
        }
    }
    
    preferences.begin("watering", false);
    currentMode = (Mode)preferences.getInt("mode", MODE_AUTO);
    dryThreshold = preferences.getInt("dryThresh", SOIL_DRY_THRESHOLD);
    wetThreshold = preferences.getInt("wetThresh", SOIL_WET_THRESHOLD);
    schedule1Hour = preferences.getInt("sch1Hour", 6);
    schedule1Minute = preferences.getInt("sch1Min", 0);
    schedule2Hour = preferences.getInt("sch2Hour", 17);
    schedule2Minute = preferences.getInt("sch2Min", 0);
    
    if (dryThreshold <= wetThreshold) {
        dryThreshold = wetThreshold + 10;
        saveSettings();
    }
    
    if (rtcError && currentMode == MODE_JADWAL) {
        currentMode = MODE_AUTO;
        saveSettings();
    }
    
    Serial.println("Setting up WiFi AP...");
    WiFi.softAP(ssid, password);
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WiFi AP Ready");
    display.print("SSID: ");
    display.println(ssid);
    display.print("IP: ");
    display.println(IP);
    display.display();
    delay(3000);
    
    server.on("/", handleRoot);
    server.on("/status", handleStatus);
    server.on("/control", handleControl);
    server.on("/settings", handleSettings);
    server.begin();
    
    Serial.println("Web server started");
    Serial.println("Setup complete!");
}
// ========================================
// MAIN LOOP
// ========================================
void loop() {
    lastLoopTime = millis();
    
    checkRTCStatus();
    server.handleClient();
    
    // *** BACA SENSOR dengan interval 1 detik ***
    static unsigned long lastSensorRead = 0;
    if (millis() - lastSensorRead > 1000) {
        readSoilMoisture();
        lastSensorRead = millis();
    }
    
    readEncoder();
    readManualButton();
    
    switch(currentMode) {
        case MODE_MANUAL:
            runManualMode();
            break;
        case MODE_AUTO:
            runAutoMode();
            break;
        case MODE_JADWAL:
            runJadwalMode();
            break;
    }
    
    checkPumpDuration();
    
    // *** UPDATE DISPLAY dengan interval 250ms ***
    static unsigned long lastDisplayUpdate = 0;
    if (millis() - lastDisplayUpdate > 250) {
        updateDisplay();
        lastDisplayUpdate = millis();
    }
    
    digitalWrite(LED_PIN, pumpState ? HIGH : LOW);
    
    delay(50);
}
// ========================================
// HTML PAGE (PROGMEM)
// ========================================
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Latihan IoT IRMAL</title>
<style>
* { margin: 0; padding: 0; box-sizing: border-box; }
body {
    font-family: Arial, sans-serif;
    background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
    min-height: 100vh;
    padding: 20px;
    display: flex;
    flex-direction: column;
}
.container {
    max-width: 500px;
    margin: 0 auto;
    background: white;
    border-radius: 15px;
    padding: 25px;
    box-shadow: 0 10px 30px rgba(0,0,0,0.3);
    flex: 1;
}
h1 {
    text-align: center;
    color: #667eea;
    margin-bottom: 5px;
    font-size: 24px;
}
.subtitle {
    text-align: center;
    color: #764ba2;
    margin-bottom: 20px;
    font-size: 16px;
    font-weight: normal;
}
.status-card {
    background: #f8f9fa;
    padding: 15px;
    border-radius: 10px;
    margin-bottom: 20px;
}
.status-item {
    display: flex;
    justify-content: space-between;
    padding: 8px 0;
    border-bottom: 1px solid #dee2e6;
}
.status-item:last-child { border-bottom: none; }
.status-label { font-weight: bold; color: #495057; }
.status-value { color: #212529; }
.section-title {
    font-size: 18px;
    font-weight: bold;
    color: #495057;
    margin: 20px 0 15px 0;
    padding-bottom: 8px;
    border-bottom: 2px solid #667eea;
    text-align: center;
}
.mode-selector-row {
    display: flex;
    flex-direction: column;
    gap: 10px;
    margin-bottom: 20px;
}
.dropdown-wrapper {
    width: 100%;
}
.btn-activate {
    width: 100%;
}
select {
    width: 100%;
    padding: 15px;
    border: 2px solid #667eea;
    border-radius: 8px;
    font-size: 16px;
    font-weight: bold;
    color: #495057;
    background: white;
    cursor: pointer;
    transition: all 0.3s;
}
select:hover {
    border-color: #5568d3;
    background: #f8f9fa;
}
select:focus {
    outline: none;
    border-color: #667eea;
    box-shadow: 0 0 0 3px rgba(102, 126, 234, 0.1);
}
.btn-activate {
    padding: 15px 25px;
    background: #667eea;
    color: white;
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: bold;
    cursor: pointer;
    white-space: nowrap;
    transition: all 0.3s;
}
.btn-activate:hover { 
    background: #5568d3;
    transform: scale(1.02);
}
.control-section {
    margin-bottom: 20px;
    padding: 15px;
    background: #e9ecef;
    border-radius: 10px;
    display: none;
}
.control-section.active {
    display: block;
}
.control-section h3 {
    margin-bottom: 12px;
    color: #495057;
    font-size: 16px;
    text-align: center;
}
.pump-control {
    display: flex;
    gap: 10px;
}
.pump-btn {
    flex: 1;
    padding: 15px;
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: bold;
    cursor: pointer;
    transition: all 0.3s;
    line-height: 1.4;
}
.pump-on { background: #28a745; color: white; }
.pump-off { background: #dc3545; color: white; }
.pump-btn:hover { transform: scale(1.05); }
.slider-group {
    margin: 15px 0;
}
.slider-group label {
    display: block;
    margin-bottom: 5px;
    font-weight: bold;
    color: #495057;
}
input[type="range"] {
    width: 100%;
    margin: 10px 0;
}
input[type="time"], input[type="number"] {
    width: 100%;
    padding: 10px;
    border: 2px solid #ced4da;
    border-radius: 5px;
    font-size: 16px;
}
.btn-save {
    width: 100%;
    padding: 15px;
    background: #28a745;
    color: white;
    border: none;
    border-radius: 8px;
    font-size: 16px;
    font-weight: bold;
    cursor: pointer;
    margin-top: 10px;
}
.btn-save:hover { background: #218838; }
.indicator {
    display: inline-block;
    width: 12px;
    height: 12px;
    border-radius: 50%;
    margin-right: 5px;
}
.indicator-on { background: #28a745; }
.indicator-off { background: #dc3545; }
.warning {
    background: #fff3cd;
    border: 2px solid #ffc107;
    padding: 10px;
    border-radius: 8px;
    margin-bottom: 15px;
    color: #856404;
    text-align: center;
    font-weight: bold;
}
.time-input-group {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 10px;
    margin: 10px 0;
}
.time-input-wrapper {
    display: flex;
    flex-direction: column;
}
.time-input-wrapper label {
    font-size: 12px;
    color: #6c757d;
    margin-bottom: 5px;
}
.footer {
    text-align: center;
    padding: 15px 0;
    margin-top: 20px;
    border-top: 2px solid #dee2e6;
    color: #6c757d;
    font-size: 14px;
}
.footer strong {
    color: #667eea;
    font-weight: bold;
}
</style>
</head>
<body>
<div class="container">
<h1>🌱 Latihan IoT IRMAL</h1>
<div class="subtitle">Sistem Penyiraman Tanaman Otomatis</div>

<div id="rtcWarning" class="warning" style="display:none;">
⚠ RTC ERROR - Mode Jadwal Tidak Tersedia
</div>

<!-- STATUS CARD -->
<div class="status-card">
<div class="status-item">
    <span class="status-label">Kelembaban Tanah</span>
    <span class="status-value" id="moisture">--%</span>
</div>
<div class="status-item">
    <span class="status-label">Status Pompa</span>
    <span class="status-value">
        <span class="indicator" id="pumpIndicator"></span>
        <span id="pumpStatus">--</span>
    </span>
</div>
<div class="status-item">
    <span class="status-label">Mode Aktif</span>
    <span class="status-value" id="currentMode">--</span>
</div>
<div class="status-item">
    <span class="status-label">Waktu</span>
    <span class="status-value" id="time">--:--:--</span>
</div>
</div>

<!-- DROPDOWN MODE + TOMBOL AKTIFKAN -->
<div class="section-title">Pilih Mode Operasi</div>
<div class="mode-selector-row">
<div class="dropdown-wrapper">
    <select id="modeDropdown" onchange="onModeDropdownChange()">
        <option value="0">MANUAL</option>
        <option value="1">AUTO</option>
        <option value="2">JADWAL</option>
    </select>
</div>
<button class="btn-activate" onclick="activateSelectedMode()">AKTIFKAN MODE</button>
</div>

<!-- FORM MANUAL -->
<div id="manualControl" class="control-section">
<h3>⚙️ Pengaturan Manual</h3>
<div class="pump-control">
    <button class="pump-btn pump-on" onclick="pumpControl(true)">POMPA<br>ON</button>
    <button class="pump-btn pump-off" onclick="pumpControl(false)">POMPA<br>OFF</button>
</div>
</div>

<!-- FORM AUTO -->
<div id="autoControl" class="control-section">
<h3>⚙️ Pengaturan Auto</h3>
<div class="slider-group">
    <label>Batas Kering: <span id="dryValue">60</span>%</label>
    <input type="range" id="dryThreshold" min="0" max="100" value="60"
           oninput="document.getElementById('dryValue').textContent=this.value">
</div>
<div class="slider-group">
    <label>Batas Basah: <span id="wetValue">30</span>%</label>
    <input type="range" id="wetThreshold" min="0" max="100" value="30"
           oninput="document.getElementById('wetValue').textContent=this.value">
</div>
<button class="btn-save" onclick="saveAutoSettings()">SIMPAN PERUBAHAN</button>
</div>

<!-- FORM JADWAL -->
<div id="jadwalControl" class="control-section">
<h3>⚙️ Pengaturan Jadwal</h3>
<div class="slider-group">
    <label>⏰ Waktu Penyiraman 1</label>
    <div class="time-input-group">
        <div class="time-input-wrapper">
            <label>Jam</label>
            <input type="number" id="sch1Hour" min="0" max="23" value="6">
        </div>
        <div class="time-input-wrapper">
            <label>Menit</label>
            <input type="number" id="sch1Min" min="0" max="59" value="0">
        </div>
    </div>
</div>
<div class="slider-group">
    <label>⏰ Waktu Penyiraman 2</label>
    <div class="time-input-group">
        <div class="time-input-wrapper">
            <label>Jam</label>
            <input type="number" id="sch2Hour" min="0" max="23" value="17">
        </div>
        <div class="time-input-wrapper">
            <label>Menit</label>
            <input type="number" id="sch2Min" min="0" max="59" value="0">
        </div>
    </div>
</div>
<p style="color: #6c757d; font-size: 14px; margin-top: 10px;">
* Durasi penyiraman: 5 menit per sesi
</p>
<button class="btn-save" onclick="saveJadwalSettings()">SIMPAN PERUBAHAN</button>
</div>
<!-- FOOTER -->
<div class="footer">
Developed by:<br>
<strong>Pemuda Hijrah</strong><br>
2025
</div>
</div>
<script>
let currentModeValue = 1; // Mode yang AKTIF saat ini
let selectedModeValue = 1; // Mode yang DIPILIH di dropdown
let isUpdating = false;
setInterval(updateStatus, 2000);
updateStatus();
// ==========================================
// UPDATE STATUS DARI SERVER
// ==========================================
function updateStatus() {
    if (isUpdating) return;
    isUpdating = true;
    fetch('/status')
    .then(response => response.json())
    .then(data => {
        document.getElementById('moisture').textContent = data.moisture + '%';
        const pumpOn = (data.pumpState === true);
        document.getElementById('pumpStatus').textContent = pumpOn ? 'ON' : 'OFF';
        document.getElementById('pumpIndicator').className =
            'indicator ' + (pumpOn ? 'indicator-on' : 'indicator-off');
        const modes = ['MANUAL', 'AUTO', 'JADWAL'];
        document.getElementById('currentMode').textContent = modes[data.mode];
        currentModeValue = data.mode;
        document.getElementById('time').textContent = data.time;
        if (data.rtcError === true) {
            document.getElementById('rtcWarning').style.display = 'block';
        } else {
            document.getElementById('rtcWarning').style.display = 'none';
        }
        const activeElement = document.activeElement;
        const isEditingSchedule = (
            activeElement.id === 'sch1Hour' || 
            activeElement.id === 'sch1Min' ||
            activeElement.id === 'sch2Hour' || 
            activeElement.id === 'sch2Min'
        );
        if (!isEditingSchedule) {
            if (data.dryThreshold !== undefined) {
                document.getElementById('dryThreshold').value = data.dryThreshold;
                document.getElementById('dryValue').textContent = data.dryThreshold;
            }
            if (data.wetThreshold !== undefined) {
                document.getElementById('wetThreshold').value = data.wetThreshold;
                document.getElementById('wetValue').textContent = data.wetThreshold;
            }
            if (data.schedule1Hour !== undefined && data.schedule1Min !== undefined) {
                document.getElementById('sch1Hour').value = data.schedule1Hour;
                document.getElementById('sch1Min').value = data.schedule1Min;
            }
            if (data.schedule2Hour !== undefined && data.schedule2Min !== undefined) {
                document.getElementById('sch2Hour').value = data.schedule2Hour;
                document.getElementById('sch2Min').value = data.schedule2Min;
            }
        }
        isUpdating = false;
    })
    .catch(err => {
        console.error('Error fetching status:', err);
        isUpdating = false;
    });
}
// ==========================================
// DROPDOWN BERUBAH - TAMPILKAN FORM
// ==========================================
function onModeDropdownChange() {
    selectedModeValue = parseInt(document.getElementById('modeDropdown').value);
    displayFormForMode(selectedModeValue);
}
function displayFormForMode(mode) {
    // Sembunyikan semua form
    document.getElementById('manualControl').classList.remove('active');
    document.getElementById('autoControl').classList.remove('active');
    document.getElementById('jadwalControl').classList.remove('active');
    if (mode === 0) {
        document.getElementById('manualControl').classList.add('active');
    } else if (mode === 1) {
        document.getElementById('autoControl').classList.add('active');
    } else if (mode === 2) {
        document.getElementById('jadwalControl').classList.add('active');
    }
}
// ==========================================
// AKTIFKAN MODE YANG DIPILIH DI DROPDOWN
// ==========================================
function activateSelectedMode() {
    const modes = ['MANUAL', 'AUTO', 'JADWAL'];   
    if (selectedModeValue === currentModeValue) {
        alert('Mode ' + modes[selectedModeValue] + ' sudah aktif!');
        return;
    }
    if (confirm('Aktifkan mode ' + modes[selectedModeValue] + '?')) {
        fetch('/control?action=mode&value=' + selectedModeValue)
        .then(response => response.json())
        .then(data => {
            if (data.success) {
                alert('Mode ' + modes[selectedModeValue] + ' berhasil diaktifkan!');
                currentModeValue = selectedModeValue;
                updateStatus();
            } else if (data.error) {
                alert(data.error);
            }
        })
        .catch(err => {
            console.error('Error:', err);
            alert('Gagal mengaktifkan mode!');
        });
    }
}
// ==========================================
// KONTROL POMPA (MODE MANUAL)
// ==========================================
function pumpControl(state) {
    fetch('/control?action=pump&value=' + (state ? '1' : '0'))
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            updateStatus();
        } else if (data.error) {
            alert(data.error);
        }
    });
}
// ==========================================
// SIMPAN PENGATURAN AUTO
// ==========================================
function saveAutoSettings() {
    const dry = document.getElementById('dryThreshold').value;
    const wet = document.getElementById('wetThreshold').value;   
    if (parseInt(dry) <= parseInt(wet)) {
        alert('Batas KERING (' + dry + ') harus lebih besar dari batas BASAH (' + wet + ')!');
        return;
    }
    fetch('/settings?type=auto&dry=' + dry + '&wet=' + wet)
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            alert('Pengaturan AUTO berhasil disimpan!');
            updateStatus();
        } else if (data.error) {
            alert(data.error);
        }
    });
}
// ==========================================
// SIMPAN PENGATURAN JADWAL
// ==========================================
function saveJadwalSettings() {
    const h1 = parseInt(document.getElementById('sch1Hour').value);
    const m1 = parseInt(document.getElementById('sch1Min').value);
    const h2 = parseInt(document.getElementById('sch2Hour').value);
    const m2 = parseInt(document.getElementById('sch2Min').value);   
    if (isNaN(h1) || isNaN(m1) || isNaN(h2) || isNaN(m2)) {
        alert('Mohon isi semua field dengan angka yang valid!');
        return;
    }
    if (h1 < 0 || h1 > 23 || h2 < 0 || h2 > 23) {
        alert('Jam harus antara 0-23!');
        return;
    }
    if (m1 < 0 || m1 > 59 || m2 < 0 || m2 > 59) {
        alert('Menit harus antara 0-59!');
        return;
    }
    fetch('/settings?type=jadwal&h1=' + h1 + '&m1=' + m1 + '&h2=' + h2 + '&m2=' + m2)
    .then(response => response.json())
    .then(data => {
        if (data.success) {
            alert('Pengaturan JADWAL berhasil disimpan!');
            setTimeout(updateStatus, 500);
        } else if (data.error) {
            alert(data.error);
        }
    });
}
// ==========================================
// INISIALISASI SAAT HALAMAN DIMUAT
// ==========================================
window.onload = function() {
    // Set dropdown ke mode AUTO (default)
    document.getElementById('modeDropdown').value = '1';
    selectedModeValue = 1;
    displayFormForMode(1);
};
</script>
</body>
</html>
)rawliteral";
// ========================================
// CHECK RTC STATUS
// ========================================
void checkRTCStatus() {
if (!rtcError && rtc.lostPower()) {
Serial.println("WARNING: RTC lost power!");
rtcError = true;
if (currentMode == MODE_JADWAL) {
currentMode = MODE_AUTO;
controlPump(false);
saveSettings();
Serial.println("Switched to AUTO mode due to RTC error");
}
}
}
// ========================================
// BACA SOIL SENSOR
// ========================================
void readSoilMoisture() {
int rawValue = analogRead(SOIL_SENSOR_PIN);
soilMoisture = map(rawValue, SOIL_WET_RAW, SOIL_DRY_RAW, 0, 100);
soilMoisture = constrain(soilMoisture, 0, 100);
}
// ========================================
// BACA ROTARY ENCODER
// ========================================
void readEncoder() {
int currentCLK = digitalRead(ENCODER_CLK);
if (currentCLK != lastCLK && currentCLK == LOW) {
if (digitalRead(ENCODER_DT) == HIGH) {
encoderPos++;
} else {
encoderPos--;
}
handleEncoderRotation();
}
lastCLK = currentCLK;
if (digitalRead(ENCODER_SW) == LOW && !buttonPressed) {
if (millis() - lastButtonTime > DEBOUNCE_DELAY) {
buttonPressed = true;
lastButtonTime = millis();
handleEncoderButton();
}
} else if (digitalRead(ENCODER_SW) == HIGH) {
buttonPressed = false;
}
}
// ========================================
// HANDLE ENCODER ROTATION
// ========================================
void handleEncoderRotation() {
int maxOptions = 4; // 4 pilihan termasuk KEMBALI
switch(menuState) {
case MENU_SELECT_MODE:
case MENU_MANUAL_MAIN:
case MENU_AUTO_MAIN:
case MENU_JADWAL_MAIN:
case MENU_MANUAL_SET_MODE:
case MENU_MANUAL_SET_PAR:
case MENU_AUTO_SET_MODE:
case MENU_AUTO_SET_PAR:
case MENU_AUTO_SET_BASAH:
case MENU_AUTO_SET_KERING:
case MENU_JADWAL_SET_MODE:
case MENU_JADWAL_SET_PAR:
case MENU_JADWAL_SET_J1:
case MENU_JADWAL_SET_J2:
menuSelection = (encoderPos % maxOptions + maxOptions) %
maxOptions;
break;
case MENU_AUTO_ADJUST_BASAH:
case MENU_AUTO_ADJUST_KERING:
tempValue = constrain(encoderPos, 0, 100);
break;
case MENU_JADWAL_ADJUST_J1_JAM:
case MENU_JADWAL_ADJUST_J2_JAM:
tempValue = (encoderPos + 24) % 24;
if (tempValue < 0) tempValue += 24;
break;
case MENU_JADWAL_ADJUST_J1_MENIT:
case MENU_JADWAL_ADJUST_J2_MENIT:
tempValue = (encoderPos + 60) % 60;
if (tempValue < 0) tempValue += 60;
break;
}
}
// ========================================
// BACA TOMBOL MANUAL
// ========================================
void readManualButton() {
bool buttonState = digitalRead(BUTTON_PIN);
if (buttonState == LOW && lastButtonState == HIGH) {
if (millis() - lastManualButtonTime > DEBOUNCE_DELAY) {
currentMode = MODE_MANUAL;
pumpState = !pumpState;
controlPump(pumpState);
saveSettings();
lastManualButtonTime = millis();
Serial.print("Button: Pompa ");
Serial.println(pumpState ? "ON" : "OFF");
}
}
lastButtonState = buttonState;
}
// ========================================
// MODE MANUAL
// ========================================
void runManualMode() {
// Manual mode - no automatic control
}
// ========================================
// MODE AUTO
// ========================================
void runAutoMode() {
if (soilMoisture >= dryThreshold && !pumpState) {
controlPump(true);
Serial.println("AUTO: Tanah kering, pompa ON");
} else if (soilMoisture <= wetThreshold && pumpState) {
controlPump(false);
Serial.println("AUTO: Tanah basah, pompa OFF");
}
}
// ========================================
// MODE JADWAL
// ========================================
void runJadwalMode() {
    if (rtcError) {
        currentMode = MODE_AUTO;
        saveSettings();
        return;
    }   
    // *** CACHE RTC READING - Baca hanya setiap 500ms ***
    static unsigned long lastRTCRead = 0;
    static DateTime cachedTime;
    if (millis() - lastRTCRead > 500) {
        cachedTime = rtc.now();
        lastRTCRead = millis();
    }
    DateTime now = cachedTime;
    // Reset flag harian
    if (now.day() != lastDay) {
        schedule1Done = false;
        schedule2Done = false;
        lastDay = now.day();
        Serial.println("JADWAL: Reset daily flags");
    }
    if (now.hour() == schedule1Hour && now.minute() == schedule1Minute && !schedule1Done) {
        Serial.println("JADWAL: Sesi 1 akan dimulai...");
        delay(50);
        controlPump(true);
        schedule1Done = true;
        delay(50);
        Serial.println("JADWAL: Sesi 1 mulai");
    }
    if (now.hour() == schedule2Hour && now.minute() == schedule2Minute && !schedule2Done) {
        Serial.println("JADWAL: Sesi 2 akan dimulai...");
        delay(50);   
        controlPump(true);
        schedule2Done = true;
        delay(50);  // Stabilisasi
        Serial.println("JADWAL: Sesi 2 mulai");
    }
    if (pumpState && (millis() - pumpStartTime >= SCHEDULE_DURATION)) {
        delay(50);  // Stabilisasi
        controlPump(false);
        Serial.println("JADWAL: Selesai, pompa OFF");
    }
}
// ========================================
// KONTROL POMPA
// ========================================
void controlPump(bool state) {
pumpState = state;
digitalWrite(RELAY_PIN, state ? LOW : HIGH);
if (state) {
pumpStartTime = millis();
}
Serial.print("Pompa: ");
Serial.println(state ? "ON" : "OFF");
}
// ========================================
// CHECK PUMP DURATION
// ========================================
void checkPumpDuration() {
if (pumpState && (millis() - pumpStartTime >= MAX_PUMP_DURATION))
{
controlPump(false);
Serial.println("SAFETY: Max duration reached, pompa OFF");
}
}
// ========================================
// SAVE SETTINGS
// ========================================
void saveSettings() {
if (dryThreshold <= wetThreshold) {
dryThreshold = wetThreshold + 10;
if (dryThreshold > 100) {
dryThreshold = 100;
wetThreshold = 90;
}
}
preferences.putInt("mode", currentMode);
preferences.putInt("dryThresh", dryThreshold);
preferences.putInt("wetThresh", wetThreshold);
preferences.putInt("sch1Hour", schedule1Hour);
preferences.putInt("sch1Min", schedule1Minute);
preferences.putInt("sch2Hour", schedule2Hour);
preferences.putInt("sch2Min", schedule2Minute);
Serial.println("Settings saved");
}
// ========================================
// HANDLE ENCODER BUTTON (PART 1/2)
// (Kode ini dipertahankan dari versi Anda)
// ========================================
void handleEncoderButton() {
switch(menuState) {
case MENU_HOME:
menuState = MENU_SELECT_MODE;
menuSelection = 0;
encoderPos = 0;
break;
case MENU_SELECT_MODE:
if (menuSelection == 0) {
menuState = MENU_MANUAL_MAIN;
} else if (menuSelection == 1) {
menuState = MENU_AUTO_MAIN;
} else if (menuSelection == 2) {
menuState = MENU_JADWAL_MAIN;
} else if (menuSelection == 3) {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
// ============ MANUAL ============
case MENU_MANUAL_MAIN:
if (menuSelection == 0) {
menuState = MENU_MANUAL_SET_MODE;
} else if (menuSelection == 1) {
menuState = MENU_MANUAL_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_SELECT_MODE;
} else if (menuSelection == 3) {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_MANUAL_SET_MODE:
if (menuSelection == 0) {
currentMode = MODE_MANUAL;
saveSettings();
menuState = MENU_HOME;
Serial.println("Mode MANUAL diaktifkan");
} else if (menuSelection == 1) {
menuState = MENU_MANUAL_MAIN;
} else {
menuState = MENU_MANUAL_MAIN;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_MANUAL_SET_PAR:
if (menuSelection == 0) {
controlPump(true);
menuState = MENU_HOME;
Serial.println("Manual: Pompa ON");
} else if (menuSelection == 1) {
controlPump(false);
menuState = MENU_HOME;
Serial.println("Manual: Pompa OFF");
} else if (menuSelection == 2) {
menuState = MENU_MANUAL_MAIN;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
// ============ AUTO ============
case MENU_AUTO_MAIN:
if (menuSelection == 0) {
menuState = MENU_AUTO_SET_MODE;
} else if (menuSelection == 1) {
menuState = MENU_AUTO_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_SELECT_MODE;
} else if (menuSelection == 3) {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_AUTO_SET_MODE:
if (menuSelection == 0) {
currentMode = MODE_AUTO;
saveSettings();
menuState = MENU_HOME;
Serial.println("Mode AUTO diaktifkan");
} else if (menuSelection == 1) {
menuState = MENU_AUTO_MAIN;
} else {
menuState = MENU_AUTO_MAIN;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_AUTO_SET_PAR:
if (menuSelection == 0) {
menuState = MENU_AUTO_SET_BASAH;
} else if (menuSelection == 1) {
menuState = MENU_AUTO_SET_KERING;
} else if (menuSelection == 2) {
menuState = MENU_AUTO_MAIN;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_AUTO_SET_BASAH:
if (menuSelection == 0) {
tempValue = wetThreshold;
encoderPos = tempValue;
menuState = MENU_AUTO_ADJUST_BASAH;
} else if (menuSelection == 1) {
menuState = MENU_AUTO_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_AUTO_SET_PAR;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
break;
case MENU_AUTO_SET_KERING:
if (menuSelection == 0) {
tempValue = dryThreshold;
encoderPos = tempValue;
menuState = MENU_AUTO_ADJUST_KERING;
} else if (menuSelection == 1) {
menuState = MENU_AUTO_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_AUTO_SET_PAR;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
break;
case MENU_AUTO_ADJUST_BASAH:
wetThreshold = tempValue;
if (wetThreshold >= dryThreshold) {
wetThreshold = dryThreshold - 10;
if (wetThreshold < 0) wetThreshold = 0;
}
saveSettings();
menuState = MENU_AUTO_SET_PAR;
menuSelection = 0;
encoderPos = 0;
Serial.print("Batas basah diubah: ");
Serial.print(wetThreshold);
Serial.println("%");
break;
case MENU_AUTO_ADJUST_KERING:
dryThreshold = tempValue;
if (dryThreshold <= wetThreshold) {
dryThreshold = wetThreshold + 10;
if (dryThreshold > 100) dryThreshold = 100;
}
saveSettings();
menuState = MENU_AUTO_SET_PAR;
menuSelection = 0;
encoderPos = 0;
Serial.print("Batas kering diubah: ");
Serial.print(dryThreshold);
Serial.println("%");
break;
// ============ JADWAL ============
case MENU_JADWAL_MAIN:
if (menuSelection == 0) {
menuState = MENU_JADWAL_SET_MODE;
} else if (menuSelection == 1) {
menuState = MENU_JADWAL_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_SELECT_MODE;
} else if (menuSelection == 3) {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_JADWAL_SET_MODE:
if (menuSelection == 0) {
if (rtcError) {
Serial.println("ERROR: RTC tidak tersedia!");
menuState = MENU_JADWAL_MAIN;
} else {
currentMode = MODE_JADWAL;
saveSettings();
menuState = MENU_HOME;
Serial.println("Mode JADWAL diaktifkan");
}
} else if (menuSelection == 1) {
menuState = MENU_JADWAL_MAIN;
} else {
menuState = MENU_JADWAL_MAIN;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_JADWAL_SET_PAR:
if (menuSelection == 0) {
menuState = MENU_JADWAL_SET_J1;
} else if (menuSelection == 1) {
menuState = MENU_JADWAL_SET_J2;
} else if (menuSelection == 2) {
menuState = MENU_JADWAL_MAIN;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
encoderPos = 0;
break;
case MENU_JADWAL_SET_J1:
if (menuSelection == 0) {
tempValue = schedule1Hour;
encoderPos = tempValue;
menuState = MENU_JADWAL_ADJUST_J1_JAM;
} else if (menuSelection == 1) {
menuState = MENU_JADWAL_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_JADWAL_SET_PAR;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
break;
case MENU_JADWAL_SET_J2:
if (menuSelection == 0) {
tempValue = schedule2Hour;
encoderPos = tempValue;
menuState = MENU_JADWAL_ADJUST_J2_JAM;
} else if (menuSelection == 1) {
menuState = MENU_JADWAL_SET_PAR;
} else if (menuSelection == 2) {
menuState = MENU_JADWAL_SET_PAR;
} else {
menuState = MENU_HOME;
}
menuSelection = 0;
break;
case MENU_JADWAL_ADJUST_J1_JAM:
schedule1Hour = tempValue;
tempValue = schedule1Minute;
encoderPos = tempValue;
menuState = MENU_JADWAL_ADJUST_J1_MENIT;
Serial.print("Jadwal 1 - Jam: ");
Serial.println(schedule1Hour);
break;
case MENU_JADWAL_ADJUST_J1_MENIT:
schedule1Minute = tempValue;
saveSettings();
menuState = MENU_JADWAL_SET_PAR;
menuSelection = 0;
encoderPos = 0;
Serial.print("Jadwal 1 disimpan: ");
Serial.print(schedule1Hour);
Serial.print(":");
Serial.println(schedule1Minute);
break;
case MENU_JADWAL_ADJUST_J2_JAM:
schedule2Hour = tempValue;
tempValue = schedule2Minute;
encoderPos = tempValue;
menuState = MENU_JADWAL_ADJUST_J2_MENIT;
Serial.print("Jadwal 2 - Jam: ");
Serial.println(schedule2Hour);
break;
case MENU_JADWAL_ADJUST_J2_MENIT:
schedule2Minute = tempValue;
saveSettings();
menuState = MENU_JADWAL_SET_PAR;
menuSelection = 0;
encoderPos = 0;
Serial.print("Jadwal 2 disimpan: ");
Serial.print(schedule2Hour);
Serial.print(":");
Serial.println(schedule2Minute);
break;
}
}
// ========================================
// UPDATE DISPLAY
// ========================================
void updateDisplay() {
    static unsigned long lastRelayChange = 0;
    static bool lastPumpState = false;
    if (pumpState != lastPumpState) {
        lastRelayChange = millis();
        lastPumpState = pumpState;
    }
    if (millis() - lastRelayChange < 200) {
        return;
    }
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0, 0);
    if (menuState == MENU_HOME) {
        display.println("Latihan IoT IRMAL");
        display.println("---");
        if (rtcError) {
            display.println("RTC ERROR!");
        } else {
            static DateTime lastValidTime;
            DateTime now = rtc.now();
            if (now.year() >= 2020 && now.year() <= 2099) {
                lastValidTime = now;
            } else {
                now = lastValidTime;
            }
            display.print(now.hour() < 10 ? "0" : "");
            display.print(now.hour());
            display.print(":");
            display.print(now.minute() < 10 ? "0" : "");
            display.print(now.minute());
            display.print(":");
            display.print(now.second() < 10 ? "0" : "");
            display.println(now.second());
            display.print(now.day() < 10 ? "0" : "");
            display.print(now.day());
            display.print("/");
            display.print(now.month() < 10 ? "0" : "");
            display.print(now.month());
            display.print("/");
            display.println(now.year());
        }
        display.println("---");
        display.print("Mode: ");
        if (currentMode == MODE_MANUAL) display.println("MANUAL");
        else if (currentMode == MODE_AUTO) display.println("AUTO");
        else display.println("JADWAL");
        display.print("Sensor: ");
        display.print(soilMoisture);
        display.print("% ");
        if (soilMoisture <= wetThreshold) display.println("(Basah)");
        else if (soilMoisture >= dryThreshold) display.println("Kering");
        else display.println("(OK)");
        display.print("Pompa: ");
        display.println(pumpState ? "ON" : "OFF");
    } else if (menuState == MENU_SELECT_MODE) {
        display.println("== PILIH MODE ==");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("MANUAL");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("AUTO");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("JADWAL");
        display.print(menuSelection == 3 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_MANUAL_MAIN) {
        display.println("== MANUAL ==");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("SET MODE");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("SET PAR");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_MANUAL_SET_MODE) {
        display.println("== SET MODE ==");
        display.println("Aktifkan MANUAL?");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("Y");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("N");
    } else if (menuState == MENU_MANUAL_SET_PAR) {
        display.println("== SET PAR ==");
        display.println("Kontrol Pompa:");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("ON");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("OFF");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_AUTO_MAIN) {
        display.println("== AUTO ==");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("SET MODE");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("SET PAR");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_AUTO_SET_MODE) {
        display.println("== SET MODE ==");
        display.println("Aktifkan AUTO?");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("Y");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("N");
    } else if (menuState == MENU_AUTO_SET_PAR) {
        display.println("== SET PAR ==");
        display.print("Basah:");
        display.print(wetThreshold);
        display.print("% Kering:");
        display.print(dryThreshold);
        display.println("%");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("BASAH");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("KERING");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_AUTO_SET_BASAH) {
        display.println("== BASAH ==");
        display.print("Nilai: ");
        display.print(wetThreshold);
        display.println("%");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("ATUR NILAI");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("(Reserved)");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_AUTO_SET_KERING) {
        display.println("== KERING ==");
        display.print("Nilai: ");
        display.print(dryThreshold);
        display.println("%");
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("ATUR NILAI");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("(Reserved)");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_AUTO_ADJUST_BASAH) {
        display.setTextSize(2);
        display.println("BASAH");
        display.println();
        display.print(" ");
        display.print(tempValue);
        display.println("%");
        display.setTextSize(1);
        display.println();
        display.println("Putar: ubah nilai");
        display.println("Tekan: simpan");
    } else if (menuState == MENU_AUTO_ADJUST_KERING) {
        display.setTextSize(2);
        display.println("KERING");
        display.println();
        display.print(" ");
        display.print(tempValue);
        display.println("%");
        display.setTextSize(1);
        display.println();
        display.println("Putar: ubah nilai");
        display.println("Tekan: simpan");
    } else if (menuState == MENU_JADWAL_MAIN) {
        display.println("== JADWAL ==");
        if (rtcError) {
            display.println();
            display.println("! RTC ERROR !");
        }
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("SET MODE");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("SET PAR");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_JADWAL_SET_MODE) {
        display.println("== SET MODE ==");
        if (rtcError) {
            display.println("RTC ERROR!");
            display.println("Tidak bisa aktif");
        } else {
            display.println("Aktifkan JADWAL?");
        }
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("Y");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("N");
    } else if (menuState == MENU_JADWAL_SET_PAR) {
        display.println("== SET PAR ==");
        display.print("J1:");
        display.print(schedule1Hour < 10 ? "0" : "");
        display.print(schedule1Hour);
        display.print(":");
        display.print(schedule1Minute < 10 ? "0" : "");
        display.print(schedule1Minute);
        display.print(" J2:");
        display.print(schedule2Hour < 10 ? "0" : "");
        display.print(schedule2Hour);
        display.print(":");
        display.print(schedule2Minute < 10 ? "0" : "");
        display.println(schedule2Minute);
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("JADWAL 1");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("JADWAL 2");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_JADWAL_SET_J1) {
        display.println("== JADWAL 1 ==");
        display.print("Waktu: ");
        display.print(schedule1Hour < 10 ? "0" : "");
        display.print(schedule1Hour);
        display.print(":");
        display.print(schedule1Minute < 10 ? "0" : "");
        display.println(schedule1Minute);
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("ATUR WAKTU");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("(Reserved)");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_JADWAL_SET_J2) {
        display.println("== JADWAL 2 ==");
        display.print("Waktu: ");
        display.print(schedule2Hour < 10 ? "0" : "");
        display.print(schedule2Hour);
        display.print(":");
        display.print(schedule2Minute < 10 ? "0" : "");
        display.println(schedule2Minute);
        display.println();
        display.print(menuSelection == 0 ? "> " : "  ");
        display.println("ATUR WAKTU");
        display.print(menuSelection == 1 ? "> " : "  ");
        display.println("(Reserved)");
        display.print(menuSelection == 2 ? "> " : "  ");
        display.println("KEMBALI");
    } else if (menuState == MENU_JADWAL_ADJUST_J1_JAM || menuState == MENU_JADWAL_ADJUST_J2_JAM) {
        display.setTextSize(2);
        display.println("JAM");
        display.println();
        display.print(" ");
        display.print(tempValue < 10 ? "0" : "");
        display.println(tempValue);
        display.setTextSize(1);
        display.println();
        display.println("Putar: ubah jam");
        display.println("Tekan: lanjut");
    } else if (menuState == MENU_JADWAL_ADJUST_J1_MENIT || menuState == MENU_JADWAL_ADJUST_J2_MENIT) {
        display.setTextSize(2);
        display.println("MENIT");
        display.println();
        display.print(" ");
        display.print(tempValue < 10 ? "0" : "");
        display.println(tempValue);
        display.setTextSize(1);
        display.println();
        display.println("Putar: ubah menit");
        display.println("Tekan: simpan");
    }   
    display.display();
}
// ========================================
// WEB SERVER HANDLERS (KODE PERBAIKAN)
// ========================================
void handleRoot() {
    // Kirim halaman HTML utama
    server.send_P(200, "text/html", index_html);
}
void handleStatus() {
    // Format waktu RTC
    String timeStr;
    if (rtcError) {
        timeStr = "--:--:--";
    } else {
        DateTime now = rtc.now();
        char buf[20];
        // Format waktu HH:MM:SS
        sprintf(buf, "%02d:%02d:%02d", now.hour(), now.minute(), now.second());
        timeStr = buf;
    }
    String json = "{";
    json += "\"moisture\":" + String(soilMoisture) + ",";
    json += "\"pumpState\":" + String(pumpState ? "true" : "false") + ",";
    json += "\"mode\":" + String((int)currentMode) + ","; // Menggunakan (int)currentMode untuk memastikan dikirim sebagai angka
    json += "\"time\":\"" + timeStr + "\","; // Menggunakan timeStr yang sudah diformat
    json += "\"rtcError\":" + String(rtcError ? "true" : "false") + ",";
    json += "\"dryThreshold\":" + String(dryThreshold) + ",";
    json += "\"wetThreshold\":" + String(wetThreshold) + ",";
    json += "\"schedule1Hour\":" + String(schedule1Hour) + ",";
    json += "\"schedule1Min\":" + String(schedule1Minute) + ",";
    json += "\"schedule2Hour\":" + String(schedule2Hour) + ",";
    json += "\"schedule2Min\":" + String(schedule2Minute);
    json += "}";
    server.send(200, "application/json", json);
}
void handleControl() {
    String action = server.arg("action");
    String value = server.arg("value");
    bool success = false;
    String error = "";
    if (action == "mode") {
        int mode = value.toInt();
        if (mode >= 0 && mode <= 2) {
            if (mode == MODE_JADWAL && rtcError) {
                error = "RTC ERROR - Mode JADWAL tidak tersedia";
            } else {
                currentMode = (Mode)mode;
                if (mode != MODE_MANUAL && pumpState) {
                    controlPump(false);
                }
                saveSettings();
                success = true;
                Serial.print("Web: Mode changed to ");
                Serial.println(mode);
            }
        } else {
            error = "Mode tidak valid";
        }
    } else if (action == "pump") {
        bool state = (value == "1");
        // Perbaikan: Izinkan kontrol pompa hanya di Mode MANUAL
        if (currentMode == MODE_MANUAL) {
            controlPump(state);
            saveSettings();
            success = true;
            Serial.print("Web: Pump ");
            Serial.println(state ? "ON" : "OFF");
        } else {
            error = "Pompa hanya bisa dikontrol di Mode MANUAL";
        }
    }
    String json = "{\"success\":" + String(success ? "true" : "false");
    if (error.length() > 0) {
        json += ",\"error\":\"" + error + "\"";
    }
    json += "}";
    server.send(200, "application/json", json);
}
void handleSettings() {
    String type = server.arg("type");
    bool success = false;
    String error = "";
    if (type == "auto") {
        int dry = server.arg("dry").toInt();
        int wet = server.arg("wet").toInt();
        if (dry >= 0 && dry <= 100 && wet >= 0 && wet <= 100) {
            if (dry > wet) {
                dryThreshold = dry;
                wetThreshold = wet;
                saveSettings();
                success = true;
                Serial.print("Web: Auto thresholds updated - Dry: ");
                Serial.print(dry);
                Serial.print("%, Wet: ");
                Serial.print(wet);
                Serial.println("%");
            } else {
                error = "Batas KERING harus lebih besar dari BASAH";
            }
        } else {
             error = "Nilai batas tidak valid (0-100)";
        }
    } else if (type == "jadwal") {
        if (rtcError) {
            error = "RTC ERROR - Tidak bisa set jadwal";
        } else {
            int h1 = server.arg("h1").toInt();
            int m1 = server.arg("m1").toInt();
            int h2 = server.arg("h2").toInt();
            int m2 = server.arg("m2").toInt();   
            if (h1 >= 0 && h1 <= 23 && m1 >= 0 && m1 <= 59 &&
                h2 >= 0 && h2 <= 23 && m2 >= 0 && m2 <= 59) {
                schedule1Hour = h1;
                schedule1Minute = m1;
                schedule2Hour = h2;
                schedule2Minute = m2;
                schedule1Done = false; 
                schedule2Done = false;
                saveSettings();
                success = true;
                Serial.print("Web: Schedule updated - 1: ");
                Serial.print(h1);
                Serial.print(":");
                Serial.print(m1);
                Serial.print(", 2: ");
                Serial.print(h2);
                Serial.print(":");
                Serial.println(m2);
            } else {
                error = "Nilai jam/menit tidak valid";
            }
        }
    } else {
         error = "Tipe pengaturan tidak dikenal";
    }
    String json = "{\"success\":" + String(success ? "true" : "false");
    if (error.length() > 0) {
        json += ",\"error\":\"" + error + "\"";
    }
    json += "}";
    server.send(200, "application/json", json);
}