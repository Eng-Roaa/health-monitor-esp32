//Done by Eng.Roaa
#define BLYNK_TEMPLATE_ID "****************"
#define BLYNK_TEMPLATE_NAME "Health system Roaa"
#define BLYNK_AUTH_TOKEN "***********************"

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "MAX30105.h"
#include "heartRate.h"

#include <ThreeWire.h>
#include <RtcDS1302.h>

#include <WiFi.h>
#include <BlynkSimpleEsp32.h>

// ================= WIFI =================
char ssid[] = "**************";
char pass[] = "*********";

// ================= HARDWARE =================
#define MPU_ADDR 0x68
#define BUZZER 19
#define SOS_BUTTON 5

LiquidCrystal_I2C lcd(0x27, 16, 2);
MAX30105 maxSensor;

// ================= RTC =================
ThreeWire myWire(17, 18, 16);
RtcDS1302<ThreeWire> Rtc(myWire);
RtcDateTime now;

// ================= IMU =================
float zFiltered = 0;
float alpha = 0.3;

// ================= SENSOR =================
long irValue, redValue;

// ================= BPM =================
float bpmRaw = 0;
float bpmAvg = 0;
float bpmStable = 0;

const byte N = 5;
float bpmBuffer[N];
byte idx = 0;
long lastBeat = 0;
int beatCount = 0; 

// ================= SpO2 =================
float dcIR = 0, dcRed = 0;
float acIR = 0, acRed = 0;
float spo2 = 0;
float spo2Stable = 0;

// ================= STATE =================
String state = "";
bool fingerDetected = false;
bool fallEventSent = false; 

// ================= TIMERS =================
unsigned long lastLCD = 0;
unsigned long lastRTC = 0;
unsigned long lastIMU = 0;
unsigned long lastBlynkWrite = 0;

// ================= MEDICINE =================
String medName = "Risperdal";
int medHour = 16;
int medMinute = 55;

bool medAlert = false;
bool medSound = false;
unsigned long medStart = 0;
// ================= Sos ======================
bool sosAlert = false;


// ================= IMU READ =================
float readZ() {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);

  float ax = (Wire.read() << 8 | Wire.read()) / 16384.0;
  float ay = (Wire.read() << 8 | Wire.read()) / 16384.0;
  float az = (Wire.read() << 8 | Wire.read()) / 16384.0;

  return az;
}

// ================= BLYNK INPUTS =================
// These capture the values from your app widgets
BLYNK_WRITE(V2) { medName = param.asStr(); }
BLYNK_WRITE(V3) { medHour = param.asInt(); }
BLYNK_WRITE(V4) { medMinute = param.asInt(); }

// 🔥 NEW: Fetch values from the app automatically if ESP32 restarts
BLYNK_CONNECTED() {
  Blynk.syncVirtual(V2, V3, V4); 
}

// ================= SETUP =================
void setup() {

  Serial.begin(115200);

  Wire.end();
  delay(200);
  Wire.begin(21, 22, 100000);

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);
  pinMode(SOS_BUTTON, INPUT_PULLUP);
  // MPU
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0x00);
  Wire.endTransmission();

  // LCD
  lcd.init();
  lcd.backlight();

  // MAX30102
  if (!maxSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    lcd.print("MAX FAIL");
    while (1);
  }

  maxSensor.setup();
  maxSensor.setPulseAmplitudeIR(0x1F);
  maxSensor.setPulseAmplitudeRed(0x0A);

  // RTC
  Rtc.Begin();
  Rtc.SetIsRunning(true);
  
  // Note: Rtc.SetDateTime is usually only run once to set the initial time, 
  // then you comment it out so it doesn't reset to compile time on every reboot.
  // Rtc.SetDateTime(RtcDateTime(__DATE__, __TIME__)); 

  // ================= WIFI + BLYNK =================
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
  }

  Blynk.config(BLYNK_AUTH_TOKEN);
  Blynk.connect();

  lcd.print("SYSTEM READY");
  delay(1500);
  lcd.clear();
}

// ================= MEDICINE =================
void checkMedicine() {

  // Check if current time matches the set hour and minute
  if (now.Hour() == medHour && now.Minute() == medMinute) {
    if (!medAlert) { // Run this only once exactly when the minute matches
      medAlert = true;
      medSound = true;
      medStart = millis();

      // Trigger Blynk Event Notification
      if (WiFi.status() == WL_CONNECTED) {
        Blynk.logEvent("medicine_alert", String("Reminder: It is time to take ") + medName);
      }
    }
  }

  // Turn off the buzzer after exactly 5 seconds (5000 milliseconds)
  if (medSound && millis() - medStart > 5000) {
    medSound = false;
  }

  // Clear the medicine alert from the LCD screen after 5 minutes (300,000 milliseconds)
  if (medAlert && millis() - medStart > 300000) {
    medAlert = false;
  }
}
// =============Sos ========
void checkSOS() {

  static bool lastState = false;

  bool currentState = (digitalRead(SOS_BUTTON) == LOW);

  // تشغيل SOS عند التحويل ON
  if (currentState && !lastState) {

    sosAlert = true;

    Serial.println("SOS ACTIVATED");

    if (WiFi.status() == WL_CONNECTED) {

      Blynk.logEvent("sos_alert", "SOS Button Activated!");
    }
  }

  // إطفاء SOS عند التحويل OFF
  if (!currentState && lastState) {

    sosAlert = false;
    Serial.println("SOS OFF");
  }

  lastState = currentState;
}
// ================= LOOP =================
void loop() {

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }
  checkSOS();
  

  // ================= 1. FAST SENSOR POLLING =================
  irValue = maxSensor.getIR();
  redValue = maxSensor.getRed();

  fingerDetected = (irValue > 10000);

  if (!fingerDetected) {
    lastBeat = millis(); 
    beatCount = 0;
    idx = 0;
    bpmStable = 0;
    spo2Stable = 0;
  } else {
    // ================= BPM =================
    if (checkForBeat(irValue)) {
      long delta = millis() - lastBeat;
      lastBeat = millis();

      bpmRaw = 60.0 / (delta / 1000.0);

      if (bpmRaw > 35 && bpmRaw < 200) {
        bpmBuffer[idx++] = bpmRaw;
        idx %= N;
        if (beatCount < N) beatCount++; 

        float sum = 0;
        for (int i = 0; i < beatCount; i++) sum += bpmBuffer[i];
        bpmAvg = sum / beatCount;

        if (abs(bpmAvg - bpmStable) < 7) {
          bpmStable = bpmStable * 0.8 + bpmAvg * 0.2;
        } else {
          bpmStable = bpmAvg;
        }
      }
    }

    // ================= SpO2 =================
    dcIR = 0.95 * dcIR + 0.05 * irValue;
    dcRed = 0.95 * dcRed + 0.05 * redValue;

    acIR = irValue - dcIR;
    acRed = redValue - dcRed;

    if (dcIR != 0 && dcRed != 0) {
      float R = (acRed / dcRed) / (acIR / dcIR);
      spo2 = 110 - 25 * R;

      if (spo2 > 100) spo2 = 100;
      if (spo2 < 85) spo2 = 85;

      spo2Stable = spo2Stable * 0.8 + spo2 * 0.2;
    }
  }

  // ================= 2. SLOW RTC CHECK =================
  if (millis() - lastRTC > 1000) {
    now = Rtc.GetDateTime();
    checkMedicine();
  
    lastRTC = millis();
  }
  
  // ================= 3. SLOW IMU CHECK =================
  if (millis() - lastIMU > 100) {
    float z = readZ();
    zFiltered = alpha * z + (1 - alpha) * zFiltered;

    float normZ = (zFiltered + 1.2) / 2.4;

    if (normZ < 0.75) {
      state = "FALL";
      
      // Trigger Blynk Event precisely once per fall
      if (!fallEventSent && WiFi.status() == WL_CONNECTED) {
        Blynk.logEvent("fall_alert", "EMERGENCY: A fall was detected!");
        fallEventSent = true; 
      }
    }
    else if (normZ < 0.9) {
      state = "WARNING";
    }
    else {
      state = "OK";
      fallEventSent = false; 
    }

    // ================= BUZZER CONTROL =================
    if (medAlert) {
      if (medSound) digitalWrite(BUZZER, HIGH);
      else digitalWrite(BUZZER, LOW);
    } 
    else if (state == "FALL") {
      digitalWrite(BUZZER, HIGH);
    } 
    else if (state == "WARNING") {
      digitalWrite(BUZZER, HIGH);
      delay(50); // Beeps briefly for warning
      digitalWrite(BUZZER, LOW);
    } 
    else {
      digitalWrite(BUZZER, LOW);
    }

    lastIMU = millis();
  
  }
  Serial.print("SOS = ");
  Serial.println(digitalRead(SOS_BUTTON));
  delay(200);

  // ================= 4. SLOW BLYNK WRITE =================
  if (millis() - lastBlynkWrite > 2000) {
    if (WiFi.status() == WL_CONNECTED) {
      Blynk.virtualWrite(V0, fingerDetected ? bpmStable : 0);
      Blynk.virtualWrite(V1, fingerDetected ? spo2Stable : 0);
    }
    lastBlynkWrite = millis();
  }

  // ================= 5. SLOW LCD UPDATE =================
  if (millis() - lastLCD > 800) {
    lcd.clear();

    if (sosAlert) {

  lcd.setCursor(0,0);
  lcd.print("SOS ALERT");

  lcd.setCursor(0,1);
  lcd.print("HELP NEEDED");}


    else if (medAlert) {
      lcd.print("MED TIME");
      lcd.setCursor(0,1);
      lcd.print(medName);
    }
    else if (state == "FALL") {
      lcd.print("FALL ALERT");
      lcd.setCursor(0,1);
      lcd.print("EMERGENCY");
    }
    else if (state == "WARNING") {
      lcd.print("WARNING");
      lcd.setCursor(0,1);
      lcd.print("Check the patient");
    }
    else if (!fingerDetected) {
      lcd.print("NO FINGER");
      lcd.setCursor(0,1);
      lcd.print(now.Hour());
      lcd.print(":");
      if (now.Minute() < 10) lcd.print("0");
      lcd.print(now.Minute());
    }
    else {
      lcd.print("HR: ");
      lcd.print((int)bpmStable); 
      lcd.setCursor(0,1);
      lcd.print("SpO2: ");
      lcd.print(spo2Stable, 1);
    }

    lastLCD = millis();
  }
}
