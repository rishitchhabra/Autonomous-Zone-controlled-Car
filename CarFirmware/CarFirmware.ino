// CarFirmware - RFID speed-zone car (ESP32 + L298N + MFRC522 + 16x2 I2C LCD + buzzer).
//
// Gets joystick/button commands from the controller over ESP-NOW, reads zone tags
// from the road and enforces the zone rules itself:
//   SCHOOL zone   -> speed level capped at the tag's limit
//   HOSPITAL zone -> horn disabled
// Sends its status back so the controller LCD shows the same information.

#include <SPI.h>
#include <Wire.h>
#include <MFRC522.h>
#include <LiquidCrystal_I2C.h>
#include <stdarg.h>
#include "Protocol.h"
#include "ZoneTag.h"
#include "EspNowLink.h"

// ---------- Pins ----------
#define PIN_ENA    25   // left side speed (PWM)
#define PIN_IN1    26
#define PIN_IN2    14
#define PIN_ENB    32   // right side speed (PWM)
#define PIN_IN3    33
#define PIN_IN4    13
#define RFID_SCK   18
#define RFID_MISO  19
#define RFID_MOSI  23
#define RFID_SS    5
#define RFID_RST   27
#define LCD_SDA    21
#define LCD_SCL    22
#define PIN_BUZZER 16

// ---------- Settings ----------
#define LCD_ADDR          0x27   // try 0x3F if the screen stays blank
#define START_LEVEL       3      // speed level after power-on
#define PWM_MIN           110    // PWM at level 1 - lowest value that still turns the motors
#define PWM_FREQ          1000
#define PWM_BITS          8
#define SPIN_MIN_PWM      160    // turning on the spot needs extra power
#define TURN_STRENGTH     0.7f   // 0..1, how much the inner side slows while steering
#define JOY_DEADZONE      35     // joystick % ignored around the centre
#define RAMP_STEP         12     // max PWM change per 20 ms tick (soft start / smooth slow-down)
#define LINK_TIMEOUT_MS   300    // stop if no command for this long
#define BUZZER_ACTIVE_LOW false  // true if your buzzer module beeps when the pin is LOW
#define LEFT_REVERSED     false  // set true if the left wheels spin backwards
#define RIGHT_REVERSED    false  // set true if the right wheels spin backwards

MFRC522 rfid(RFID_SS, RFID_RST);
LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

// ---------- Radio (filled by the ESP-NOW callback) ----------
portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
ControlPacket rxControl;
volatile bool controlFresh = false;

// ---------- Car state ----------
QueueHandle_t tagQueue;
ControlPacket ctrl = {};
uint32_t lastPacketMs = 0;
bool linkOk = false;
bool haveCounts = false;
uint8_t lastUpCount = 0, lastDownCount = 0;

uint8_t zoneFlags = 0;
uint8_t limitLevel = MAX_LEVEL;
uint8_t setLevel = START_LEVEL;
bool hornBlocked = false;
int curLeft = 0, curRight = 0;   // current PWM, -255..255

char lcdShown[2][17];

// ---------- PWM (API changed in ESP32 Arduino core 3.x) ----------
void pwmAttach(uint8_t pin, uint8_t channel) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(pin, PWM_FREQ, PWM_BITS);
#else
  ledcSetup(channel, PWM_FREQ, PWM_BITS);
  ledcAttachPin(pin, channel);
#endif
}

void pwmWrite(uint8_t pin, uint8_t channel, uint32_t duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(pin, duty);
#else
  ledcWrite(channel, duty);
#endif
}

// ---------- Radio ----------
void onPacket(const uint8_t *data, int len) {
  if (len != sizeof(ControlPacket)) return;
  const ControlPacket *p = (const ControlPacket *)data;
  if (p->pairId != PAIR_ID || p->type != PKT_CONTROL) return;
  portENTER_CRITICAL(&radioMux);
  memcpy(&rxControl, data, sizeof(ControlPacket));
  controlFresh = true;
  portEXIT_CRITICAL(&radioMux);
}

void readRadio(uint32_t now) {
  if (controlFresh) {
    portENTER_CRITICAL(&radioMux);
    ctrl = rxControl;
    controlFresh = false;
    portEXIT_CRITICAL(&radioMux);

    lastPacketMs = now;
    linkOk = true;

    // Button presses arrive as counters; apply only the new presses
    if (!haveCounts) {
      lastUpCount = ctrl.speedUpCount;
      lastDownCount = ctrl.speedDownCount;
      haveCounts = true;
    }
    uint8_t ups = ctrl.speedUpCount - lastUpCount;
    uint8_t downs = ctrl.speedDownCount - lastDownCount;
    lastUpCount = ctrl.speedUpCount;
    lastDownCount = ctrl.speedDownCount;
    setLevel = constrain((int)setLevel + ups - downs, MIN_LEVEL, MAX_LEVEL);
  }

  if (linkOk && now - lastPacketMs > LINK_TIMEOUT_MS) {
    linkOk = false;
    haveCounts = false;   // controller may have restarted and reset its counters
    ctrl = {};
  }
}

// ---------- RFID ----------
// Polling the reader blocks for ~25 ms when no tag is present,
// so it runs in its own task and hands tags to loop() through a queue.
void rfidTask(void *) {
  for (;;) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      ZoneTag tag;
      if (readZoneTag(rfid, tag)) xQueueSend(tagQueue, &tag, 0);
      // A halted tag stays silent until it leaves the field, so each tag is read once
      rfid.PICC_HaltA();
      rfid.PCD_StopCrypto1();
    }
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

void applyZoneTags() {
  ZoneTag tag;
  while (xQueueReceive(tagQueue, &tag, 0) == pdTRUE) {
    if (tag.kind == TAG_START) {
      zoneFlags |= tag.flags;
      if (tag.flags & ZONE_LIMIT) limitLevel = constrain(tag.limitLevel, MIN_LEVEL, MAX_LEVEL);
    } else {
      zoneFlags &= ~tag.flags;
    }
  }
}

uint8_t actualLevel() {
  return (zoneFlags & ZONE_LIMIT) ? min(setLevel, limitLevel) : setLevel;
}

// ---------- Motors ----------
int levelToPwm(uint8_t level) {
  return PWM_MIN + (level - MIN_LEVEL) * (255 - PWM_MIN) / (MAX_LEVEL - MIN_LEVEL);
}

int rampToward(int current, int target) {
  if (target > current) return min(current + RAMP_STEP, target);
  if (target < current) return max(current - RAMP_STEP, target);
  return current;
}

void driveSide(uint8_t enPin, uint8_t channel, uint8_t inA, uint8_t inB, int value) {
  digitalWrite(inA, value > 0);
  digitalWrite(inB, value < 0);
  pwmWrite(enPin, channel, abs(value));
}

void updateDrive() {
  int targetLeft = 0, targetRight = 0;

  if (linkOk) {
    int x = abs(ctrl.joyX) > JOY_DEADZONE ? ctrl.joyX : 0;
    int y = abs(ctrl.joyY) > JOY_DEADZONE ? ctrl.joyY : 0;
    int base = levelToPwm(actualLevel());

    if (y != 0) {
      // Driving forward/back: slow the inner side to steer
      int dir = y > 0 ? 1 : -1;
      int inner = base * (1.0f - TURN_STRENGTH * abs(x) / 100.0f);
      targetLeft = dir * (x < 0 ? inner : base);
      targetRight = dir * (x > 0 ? inner : base);
    } else if (x != 0) {
      // Joystick only sideways: spin on the spot
      int spin = max(base, SPIN_MIN_PWM);
      targetLeft = x > 0 ? spin : -spin;
      targetRight = -targetLeft;
    }
  }

  curLeft = rampToward(curLeft, targetLeft);
  curRight = rampToward(curRight, targetRight);
  driveSide(PIN_ENA, 0, PIN_IN1, PIN_IN2, LEFT_REVERSED ? -curLeft : curLeft);
  driveSide(PIN_ENB, 1, PIN_IN3, PIN_IN4, RIGHT_REVERSED ? -curRight : curRight);
}

// ---------- Horn ----------
void updateHorn() {
  bool pressed = linkOk && ctrl.horn;
  hornBlocked = pressed && (zoneFlags & ZONE_NO_HORN);
  bool on = pressed && !hornBlocked;
  digitalWrite(PIN_BUZZER, on != BUZZER_ACTIVE_LOW);
}

// ---------- Status + LCD ----------
void sendStatus() {
  StatusPacket s = {PAIR_ID, PKT_STATUS, zoneFlags, limitLevel, setLevel, actualLevel(), hornBlocked};
  broadcastPacket(&s, sizeof(s));
}

// Prints a line padded to 16 chars; skips the I2C write if nothing changed (avoids flicker).
void showLine(uint8_t row, const char *fmt, ...) {
  char text[17], padded[17];
  va_list args;
  va_start(args, fmt);
  vsnprintf(text, sizeof(text), fmt, args);
  va_end(args);
  snprintf(padded, sizeof(padded), "%-16s", text);
  if (strcmp(padded, lcdShown[row]) == 0) return;
  strcpy(lcdShown[row], padded);
  lcd.setCursor(0, row);
  lcd.print(padded);
}

void updateLcd() {
  if (!linkOk && lastPacketMs == 0) {
    showLine(0, "WAITING FOR");
    showLine(1, "CONTROLLER...");
    return;
  }
  if (!linkOk) {
    showLine(0, "!! LINK  LOST !!");
    showLine(1, "  CAR STOPPED");
    return;
  }
  char zone[17];
  formatZoneLine(zone, sizeof(zone), zoneFlags, limitLevel);
  showLine(0, "%s", zone);
  if (hornBlocked) showLine(1, "HORN DISABLED!");
  else             showLine(1, "SPEED %u/%u", actualLevel(), MAX_LEVEL);
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_IN1, OUTPUT);
  pinMode(PIN_IN2, OUTPUT);
  pinMode(PIN_IN3, OUTPUT);
  pinMode(PIN_IN4, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, BUZZER_ACTIVE_LOW);   // buzzer off
  pwmAttach(PIN_ENA, 0);
  pwmAttach(PIN_ENB, 1);
  driveSide(PIN_ENA, 0, PIN_IN1, PIN_IN2, 0);
  driveSide(PIN_ENB, 1, PIN_IN3, PIN_IN4, 0);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  showLine(0, "RFID ZONE CAR");
  showLine(1, "Starting...");

  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("MFRC522 not found - check wiring");
    showLine(1, "RFID NOT FOUND!");
    delay(2000);
  }

  if (!startEspNow(onPacket)) {
    Serial.println("ESP-NOW start failed");
    showLine(1, "RADIO FAILED!");
    delay(2000);
  }

  tagQueue = xQueueCreate(8, sizeof(ZoneTag));
  xTaskCreatePinnedToCore(rfidTask, "rfid", 4096, nullptr, 1, nullptr, 0);

  Serial.println("Car ready");
}

void loop() {
  static uint32_t lastDriveMs = 0, lastStatusMs = 0, lastLcdMs = 0;
  uint32_t now = millis();

  readRadio(now);
  applyZoneTags();

  if (now - lastDriveMs >= 20) {
    lastDriveMs = now;
    updateDrive();
    updateHorn();
  }
  if (now - lastStatusMs >= 100) {
    lastStatusMs = now;
    sendStatus();
  }
  if (now - lastLcdMs >= 250) {
    lastLcdMs = now;
    updateLcd();
  }
}
