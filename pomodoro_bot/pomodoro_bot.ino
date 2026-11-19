#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <SimpleDHT.h>
#include <RTClib.h>
#include <Servo.h>
#include <SoftwareSerial.h>
#include <DFRobotDFPlayerMini.h>

// LCD
LiquidCrystal_I2C lcd(0x27, 16, 2);

// RTC
RTC_DS3231 rtc;

// Servo
Servo myServo;

// DFPlayer
SoftwareSerial mp3Serial(11, 12); // Arduino RX, TX
DFRobotDFPlayerMini player;
bool audioReady = false;

// Pins
const int trigPin = 9;
const int echoPin = 10;
const int touchPin = 2;
const int pirPin = 4;
const int ledPin = 5;
const int resetBtn = 6;
const int servoPin = 8;
const int micPin = A0;
const int ldrPin = A1;

// DHT
#define DHTPIN 3
SimpleDHT11 dht11(DHTPIN);

// ---------------- AUDIO FILES ----------------
// /mp3/0001.mp3 = beep
// /mp3/0002.mp3 = Too close
// /mp3/0003.mp3 = Startup tune
// /mp3/0004.mp3 = Welcome back
// /mp3/0005.mp3 = Drink water

// ---------------- SERVO ANGLES ----------------
const int SERVO_FOCUS = 90;
const int SERVO_NORMAL = 70;
const int SERVO_SLEEP = 30;
const int SERVO_BREAK = 10;

// ---------------- STATES ----------------
bool isRunning = true;
int displayMode = 0;
// 0 = normal display
// 1 = posture warning
// 2 = humidity display
// 3 = unused now
// 4 = noise warning
// 5 = hydration reminder

// Pomodoro
int workMode = 0;
// 0 = Normal
// 1 = M1
// 2 = M2
// 3 = M3

bool pomodoroRunning = false;
bool isBreakMode = false;

unsigned long pomodoroStartTime = 0;
unsigned long pomodoroDuration = 0;
unsigned long lastPomodoroDisplay = 0;

int pomodoroSessionCount = 0;

unsigned long focusDurations[] = {
  0,
  25UL * 60UL * 1000UL,
  50UL * 60UL * 1000UL,
  15UL * 60UL * 1000UL
};

unsigned long breakDurations[] = {
  0,
  5UL * 60UL * 1000UL,
  10UL * 60UL * 1000UL,
  3UL * 60UL * 1000UL
};

// Sleep
unsigned long pausedStartTime = 0;
bool sleepMode = false;

// Hydration
unsigned long lastHydrationTime = 0;
const unsigned long hydrationInterval = 60UL * 60UL * 1000UL;

// Lighting
bool autoMode = true;
int manualBrightness = 0;

// Timing
unsigned long lastCheck = 0;
unsigned long lastDHTRead = 0;
unsigned long tempDisplayStart = 0;
bool pausedDrawn = false;

// DHT
byte temperature = 0;
byte humidity = 0;

// Touch
bool lastTouch = LOW;
bool longPressHandled = false;
unsigned long touchStart = 0;
unsigned long lastTapTime = 0;
int tapCount = 0;

// PIR / Away Detection
unsigned long lastMotionSeen = 0;
unsigned long awayPromptStart = 0;
bool awayPromptActive = false;
bool autoPausedByAway = false;

const unsigned long noMotionLimit = 30UL * 1000UL;
const unsigned long awayPromptDuration = 10UL * 1000UL;

// Noise
unsigned long lastNoiseCheck = 0;
unsigned long lastNoiseDisplay = 0;
int noiseThreshold = 620;

// Posture audio
unsigned long lastTooCloseAudio = 0;

// RTC
int lastDay = -1;

// ---------------- SETUP ----------------
void setup() {
  lcd.begin();
  lcd.backlight();

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(touchPin, INPUT);
  pinMode(pirPin, INPUT);
  pinMode(ledPin, OUTPUT);
  pinMode(resetBtn, INPUT_PULLUP);
  pinMode(micPin, INPUT);

  myServo.attach(servoPin);
  myServo.write(SERVO_NORMAL);

  Wire.begin();
  rtc.begin();

  mp3Serial.begin(9600);

  if (player.begin(mp3Serial)) {
    audioReady = true;
    player.volume(25); // 0 to 30
  }

  // Set the RTC to the compile time only if it has lost power
  // (otherwise every reset would roll the clock back to the upload time)
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  lcd.print("Starting...");
  playAudio(3); // /mp3/0003.mp3
  delay(2500);
  lcd.clear();

  DateTime now = rtc.now();
  lastDay = now.day();

  lastHydrationTime = millis();
  lastMotionSeen = millis();
}

// ---------------- LOOP ----------------
void loop() {
  handleResetButton();
  handleRTC();
  handleTouch();
  handleMotionMonitor();
  handleHydrationReminder();

  if (pomodoroRunning) {
    handlePomodoro();
    handleDHT();
    handleLighting();
    handleTemporaryDisplays();
    return;
  }

  if (!isRunning) {
    handlePausedIdle();
    return;
  }

  myServo.write(SERVO_NORMAL);

  handleNoise();
  handlePosture();
  handleDHT();
  handleLighting();
  handleClock();
  handleTemporaryDisplays();
}

// ---------------- AUDIO ----------------
void playAudio(int fileNumber) {
  if (audioReady) {
    player.playMp3Folder(fileNumber);
  }
}

void playBeepOnce() {
  playAudio(1); // /mp3/0001.mp3
}

void playBeepTwice() {
  playAudio(1);
  delay(1600);
  playAudio(1);
}

// ---------------- RESET ----------------
void handleResetButton() {
  if (digitalRead(resetBtn) == LOW) {
    lcd.clear();
    lcd.print("System");
    lcd.setCursor(0, 1);
    lcd.print("Resetting...");
    delay(2000);

    void(* resetFunc) (void) = 0;
    resetFunc();
  }
}

// ---------------- RTC ----------------
void handleRTC() {
  DateTime now = rtc.now();

  if (now.day() != lastDay) {
    pomodoroSessionCount = 0;
    lastDay = now.day();
  }
}

// ---------------- MOTION / AWAY DETECTION ----------------
void handleMotionMonitor() {
  bool motionDetected = digitalRead(pirPin) == HIGH;

  if (motionDetected) {
    lastMotionSeen = millis();

    if (autoPausedByAway && !pomodoroRunning) {
      autoPausedByAway = false;
      awayPromptActive = false;

      isRunning = true;
      sleepMode = false;
      pausedDrawn = false;
      displayMode = 0;

      myServo.write(SERVO_NORMAL);

      lcd.clear();
      lcd.print("Welcome Back");
      playAudio(4); // /mp3/0004.mp3
      delay(2000);
      lcd.clear();
    }

    return;
  }

  if (!isRunning || pomodoroRunning) return;

  if (millis() - lastMotionSeen >= noMotionLimit) {
    isRunning = false;
    autoPausedByAway = true;
    awayPromptActive = true;
    awayPromptStart = millis();

    sleepMode = false;
    pausedDrawn = true;
    pausedStartTime = millis();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Are you away?");
    lcd.setCursor(0, 1);
    lcd.print("No motion");
  }
}

// ---------------- HYDRATION ----------------
void handleHydrationReminder() {
  if (!isRunning && !pomodoroRunning) return;
  if (displayMode != 0) return;

  if (millis() - lastHydrationTime >= hydrationInterval) {
    lastHydrationTime = millis();

    displayMode = 5;
    tempDisplayStart = millis();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Drink Water");
    lcd.setCursor(0, 1);
    lcd.print("Hydration Time");

    playAudio(5); // /mp3/0005.mp3
  }
}

// ---------------- TOUCH ----------------
void handleTouch() {
  bool current = digitalRead(touchPin);

  if (current && !lastTouch) {
    touchStart = millis();
    longPressHandled = false;
  }

  if (current && !longPressHandled && millis() - touchStart > 1200) {
    longPressHandled = true;
    tapCount = 0;
    handleLongPress();
  }

  if (!current && lastTouch) {
    unsigned long duration = millis() - touchStart;

    if (!longPressHandled && duration < 800) {
      tapCount++;
      lastTapTime = millis();
    }
  }

  if (tapCount > 0 && millis() - lastTapTime > 400) {
    handleTap(tapCount);
    tapCount = 0;
  }

  lastTouch = current;
}

void handleLongPress() {
  if (pomodoroRunning) return;
  if (isRunning) return;

  workMode++;

  if (workMode > 3) {
    workMode = 0;
  }

  pomodoroSessionCount = 0;
  isBreakMode = false;
  sleepMode = false;
  pausedStartTime = millis();
  pausedDrawn = false;

  myServo.write(SERVO_NORMAL);
}

void handleTap(int count) {
  if (count == 1) {
    if (pomodoroRunning) {
      stopPomodoro();
      return;
    }

    if (workMode == 0) {
      isRunning = !isRunning;
      lcd.clear();

      if (!isRunning) {
        autoPausedByAway = false;
        awayPromptActive = false;

        pausedStartTime = millis();
        sleepMode = false;
        pausedDrawn = false;
        myServo.write(SERVO_NORMAL);
      }

      else {
        sleepMode = false;
        pausedDrawn = false;
        myServo.write(SERVO_NORMAL);

        lcd.print("Resuming...");
        playAudio(4); // /mp3/0004.mp3
        delay(2000);
        lcd.clear();

        lastMotionSeen = millis();
      }
    }

    else {
      startPomodoro();
    }
  }

  else if (count == 2) {
    autoMode = !autoMode;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(autoMode ? "Auto Light" : "Manual Light");

    delay(800);
    lcd.clear();

    if (!isRunning) pausedDrawn = false;
  }

  else if (count == 3) {
    if (!autoMode) {
      if (manualBrightness == 0) manualBrightness = 80;
      else if (manualBrightness == 80) manualBrightness = 180;
      else manualBrightness = 0;

      lcd.clear();
      lcd.print("Brightness:");
      lcd.setCursor(0, 1);
      lcd.print(manualBrightness);
      delay(800);
      lcd.clear();

      if (!isRunning) pausedDrawn = false;
    }
  }

  else if (count == 4) {
    showHumidity();

    if (!isRunning) {
      delay(2000);
      lcd.clear();
      pausedDrawn = false;
    }

    else {
      displayMode = 2;
      tempDisplayStart = millis();
    }
  }
}

// ---------------- PAUSED / IDLE ----------------
void handlePausedIdle() {
  if (awayPromptActive) {
    if (millis() - awayPromptStart < awayPromptDuration) {
      return;
    }

    awayPromptActive = false;
    pausedDrawn = false;
  }

  if (!sleepMode) {
    myServo.write(SERVO_NORMAL);
  }

  if (!pausedDrawn) {
    lcd.clear();

    if (workMode == 0) {
      lcd.setCursor(0, 0);
      lcd.print("System Paused");
      lcd.setCursor(0, 1);

      if (autoPausedByAway) {
        lcd.print("Away Mode");
      } else {
        lcd.print("Normal Mode");
      }
    }

    else {
      lcd.setCursor(0, 0);
      lcd.print("M");
      lcd.print(workMode);
      lcd.print(" Selected");

      lcd.setCursor(0, 1);

      if (workMode == 1) lcd.print("25F / 5B");
      else if (workMode == 2) lcd.print("50F / 10B");
      else if (workMode == 3) lcd.print("15F / 3B");
    }

    pausedDrawn = true;
  }

  if (!sleepMode && millis() - pausedStartTime >= 10000) {
    sleepMode = true;

    lcd.clear();
    lcd.print("Going to sleep");
    myServo.write(SERVO_SLEEP);

    delay(1500);
    lcd.clear();
    pausedDrawn = false;
  }
}

// ---------------- POMODORO ----------------
void startPomodoro() {
  pomodoroRunning = true;
  isRunning = true;
  isBreakMode = false;

  autoPausedByAway = false;
  awayPromptActive = false;

  pomodoroStartTime = millis();
  pomodoroDuration = focusDurations[workMode];
  lastPomodoroDisplay = 0;

  sleepMode = false;
  pausedDrawn = false;
  displayMode = 0;

  myServo.write(SERVO_FOCUS);
  lcd.clear();

  playBeepOnce();
}

void stopPomodoro() {
  pomodoroRunning = false;
  isRunning = false;
  isBreakMode = false;

  autoPausedByAway = false;
  awayPromptActive = false;

  pausedStartTime = millis();
  sleepMode = false;
  pausedDrawn = false;

  myServo.write(SERVO_NORMAL);
  lcd.clear();
}

void handlePomodoro() {
  if (isBreakMode) {
    myServo.write(SERVO_BREAK);
  }

  else {
    myServo.write(SERVO_FOCUS);
  }

  unsigned long elapsed = millis() - pomodoroStartTime;

  if (elapsed >= pomodoroDuration) {
    if (!isBreakMode) {
      isBreakMode = true;
      pomodoroStartTime = millis();
      pomodoroDuration = breakDurations[workMode];
      lastPomodoroDisplay = 0;

      lcd.clear();
      playBeepTwice();
    }

    else {
      pomodoroSessionCount++;

      pomodoroRunning = false;
      isRunning = false;
      isBreakMode = false;

      autoPausedByAway = false;
      awayPromptActive = false;

      pausedStartTime = millis();
      sleepMode = false;
      pausedDrawn = false;

      myServo.write(SERVO_NORMAL);
      lcd.clear();
      return;
    }
  }

  if (displayMode == 5) return;

  if (millis() - lastPomodoroDisplay >= 1000 || lastPomodoroDisplay == 0) {
    lastPomodoroDisplay = millis();
    showPomodoroTimer();
  }
}

void showPomodoroTimer() {
  unsigned long elapsed = millis() - pomodoroStartTime;
  unsigned long remaining;

  if (elapsed >= pomodoroDuration) remaining = 0;
  else remaining = pomodoroDuration - elapsed;

  int totalSeconds = remaining / 1000;
  int minutes = totalSeconds / 60;
  int seconds = totalSeconds % 60;

  lcd.setCursor(0, 0);

  if (isBreakMode) {
    lcd.print("Focus in: ");
  }

  else {
    lcd.print("Break in: ");
  }

  if (minutes < 10) lcd.print("0");
  lcd.print(minutes);
  lcd.print(":");

  if (seconds < 10) lcd.print("0");
  lcd.print(seconds);

  lcd.print(" ");

  lcd.setCursor(0, 1);
  lcd.print("M");
  lcd.print(workMode);
  lcd.print(" session:");
  lcd.print(pomodoroSessionCount);
  lcd.print("   ");
}

// ---------------- CLOCK + TEMP + HUMIDITY ----------------
void handleClock() {
  if (displayMode != 0) return;
  if (workMode != 0) return;

  static int lastSec = -1;

  DateTime now = rtc.now();

  if (now.second() == lastSec) return;
  lastSec = now.second();

  lcd.setCursor(0, 0);
  lcd.print("Time ");

  if (now.hour() < 10) lcd.print("0");
  lcd.print(now.hour());
  lcd.print(":");

  if (now.minute() < 10) lcd.print("0");
  lcd.print(now.minute());
  lcd.print(":");

  if (now.second() < 10) lcd.print("0");
  lcd.print(now.second());

  lcd.print(" ");

  lcd.setCursor(0, 1);
  lcd.print("T:");
  lcd.print(temperature);
  lcd.print("C ");

  lcd.print("H:");
  lcd.print(humidity);
  lcd.print("%   ");
}

// ---------------- LIGHTING ----------------
void handleLighting() {
  if (autoMode) {
    int light = analogRead(ldrPin);

    if (light < 150) {
      analogWrite(ledPin, 0);
      return;
    }

    int brightness = map(light, 150, 900, 0, 255);
    brightness = constrain(brightness, 0, 255);
    brightness = brightness / 3;

    analogWrite(ledPin, brightness);
  }

  else {
    analogWrite(ledPin, manualBrightness);
  }
}

// ---------------- NOISE ----------------
void handleNoise() {
  if (displayMode == 2 || displayMode == 3 || displayMode == 5) return;

  if (millis() - lastNoiseCheck < 100) return;
  lastNoiseCheck = millis();

  int noiseValue = analogRead(micPin);

  if (noiseValue > noiseThreshold && millis() - lastNoiseDisplay > 2500) {
    lastNoiseDisplay = millis();

    displayMode = 4;
    tempDisplayStart = millis();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Too Much Noise!");
    lcd.setCursor(0, 1);
    lcd.print("Noise:");
    lcd.print(noiseValue);
    lcd.print("   ");
  }
}

// ---------------- POSTURE ----------------
void handlePosture() {
  if (displayMode == 2 || displayMode == 3 || displayMode == 4 || displayMode == 5) return;

  if (millis() - lastCheck < 300) return;
  lastCheck = millis();

  int dist = getDistance();

  if (dist > 0 && dist < 30) {
    if (displayMode != 1 && millis() - lastTooCloseAudio > 5000) {
      playAudio(2); // /mp3/0002.mp3
      lastTooCloseAudio = millis();
    }

    displayMode = 1;

    lcd.clear();
    lcd.print("Too Close!");
    lcd.setCursor(0, 1);
    lcd.print("Dist:");
    lcd.print(dist);
    lcd.print("cm");
  }

  else if (displayMode == 1) {
    displayMode = 0;
    lcd.clear();
  }
}

// ---------------- DHT ----------------
void handleDHT() {
  if (millis() - lastDHTRead < 2000) return;
  lastDHTRead = millis();

  byte t, h;

  if (dht11.read(&t, &h, NULL) == SimpleDHTErrSuccess) {
    temperature = t;
    humidity = h;
  }
}

// ---------------- HUMIDITY ----------------
void showHumidity() {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Humidity: ");
  lcd.print(humidity);
  lcd.print("%");

  lcd.setCursor(0, 1);

  if (humidity > 70) lcd.print("Too Humid");
  else if (humidity < 30) lcd.print("Too Dry");
  else lcd.print("Normal");
}

// ---------------- TEMP DISPLAY RESET ----------------
void handleTemporaryDisplays() {
  if ((displayMode == 2 || displayMode == 4 || displayMode == 5) &&
      millis() - tempDisplayStart > 3000) {
    displayMode = 0;
    lcd.clear();
  }
}

// ---------------- ULTRASONIC ----------------
int getDistance() {
  digitalWrite(trigPin, LOW);
  delayMicroseconds(2);

  digitalWrite(trigPin, HIGH);
  delayMicroseconds(10);
  digitalWrite(trigPin, LOW);

  long duration = pulseIn(echoPin, HIGH, 30000);

  if (duration == 0) return -1;

  return duration * 0.034 / 2;
}
