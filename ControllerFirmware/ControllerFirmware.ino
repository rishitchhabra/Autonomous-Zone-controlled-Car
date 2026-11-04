// ControllerFirmware - handheld controller (ESP32 + joystick + 3 buttons + 16x2 I2C LCD).
//
// Sends the joystick position and button presses to the car over ESP-NOW,
// and shows the zone/speed status the car sends back.
//
// Leave the joystick centred while powering on - its centre is calibrated at start-up.

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <stdarg.h>
#include "Protocol.h"
#include "EspNowLink.h"

// ---------- Pins ----------
#define PIN_JOY_X    34
#define PIN_JOY_Y    35
#define PIN_BTN_UP   25   // Speed +
#define PIN_BTN_DOWN 26   // Speed -
#define PIN_BTN_HORN 27
#define LCD_SDA      21
#define LCD_SCL      22

// ---------- Settings ----------
#define LCD_ADDR          0x27   // try 0x3F if the screen stays blank
#define INVERT_X          false  // set true if left/right are swapped (check on the LCD)
#define INVERT_Y          false  // set true if forward/back are swapped
#define DEBOUNCE_MS       30
#define SEND_INTERVAL_MS  50
#define CAR_TIMEOUT_MS    500

LiquidCrystal_I2C lcd(LCD_ADDR, 16, 2);

struct Button {
  uint8_t pin;
  bool pressed;      // debounced state
  bool lastRaw;
  uint32_t changedMs;
};

Button btnUp   = {PIN_BTN_UP, false, false, 0};
Button btnDown = {PIN_BTN_DOWN, false, false, 0};
Button btnHorn = {PIN_BTN_HORN, false, false, 0};

int centerX = 2048, centerY = 2048;
int8_t joyX = 0, joyY = 0;
uint8_t speedUpCount = 0, speedDownCount = 0;

// ---------- Radio (filled by the ESP-NOW callback) ----------
portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
StatusPacket rxStatus;
volatile bool statusFresh = false;

StatusPacket status = {};
uint32_t lastStatusMs = 0;
bool carLinked = false;

char lcdShown[2][17];

// ---------- Inputs ----------
// Returns true once when the button is newly pressed.
bool updateButton(Button &b, uint32_t now) {
  bool raw = digitalRead(b.pin) == LOW;
  if (raw != b.lastRaw) {
    b.lastRaw = raw;
    b.changedMs = now;
  }
  if (now - b.changedMs >= DEBOUNCE_MS && raw != b.pressed) {
    b.pressed = raw;
    return raw;
  }
  return false;
}

int readCenter(uint8_t pin) {
  long sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += analogRead(pin);
    delay(2);
  }
  return sum / 32;
}

// Maps the 12-bit reading to -100..100 around the calibrated centre.
int8_t readAxis(uint8_t pin, int center, bool invert) {
  int raw = analogRead(pin);
  long v = raw >= center ? (long)(raw - center) * 100 / max(1, 4095 - center)
                         : (long)(raw - center) * 100 / max(1, center);
  v = constrain(v, -100, 100);
  return invert ? -v : v;
}

// ---------- Radio ----------
void onPacket(const uint8_t *data, int len) {
  if (len != sizeof(StatusPacket)) return;
  const StatusPacket *p = (const StatusPacket *)data;
  if (p->pairId != PAIR_ID || p->type != PKT_STATUS) return;
  portENTER_CRITICAL(&radioMux);
  memcpy(&rxStatus, data, sizeof(StatusPacket));
  statusFresh = true;
  portEXIT_CRITICAL(&radioMux);
}

void readRadio(uint32_t now) {
  if (statusFresh) {
    portENTER_CRITICAL(&radioMux);
    status = rxStatus;
    statusFresh = false;
    portEXIT_CRITICAL(&radioMux);
    lastStatusMs = now;
    carLinked = true;
  }
  if (carLinked && now - lastStatusMs > CAR_TIMEOUT_MS) carLinked = false;
}

void sendControl() {
  ControlPacket p = {PAIR_ID, PKT_CONTROL, joyX, joyY, speedUpCount, speedDownCount, btnHorn.pressed};
  broadcastPacket(&p, sizeof(p));
}

// ---------- LCD ----------
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

void updateLcd(uint32_t now) {
  if (!carLinked) {
    // Live joystick readout doubles as a check that X/Y directions are right
    showLine(0, "SEARCHING CAR...");
    showLine(1, "X%+4d  Y%+4d", joyX, joyY);
    return;
  }

  char zone[17];
  formatZoneLine(zone, sizeof(zone), status.zoneFlags, status.limitLevel);
  showLine(0, "%s", zone);

  if (status.hornBlocked) {
    bool blinkOn = (now / 300) % 2 == 0;
    showLine(1, "%s", blinkOn ? "HORN DISABLED!" : "");
  } else if (status.actualLevel < status.setLevel) {
    showLine(1, "SET %u  ACTUAL %u", status.setLevel, status.actualLevel);
  } else {
    showLine(1, "SPEED %u/%u", status.actualLevel, MAX_LEVEL);
  }
}

// ---------- Setup / loop ----------
void setup() {
  Serial.begin(115200);

  pinMode(PIN_BTN_UP, INPUT_PULLUP);
  pinMode(PIN_BTN_DOWN, INPUT_PULLUP);
  pinMode(PIN_BTN_HORN, INPUT_PULLUP);
  analogReadResolution(12);

  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  showLine(0, "CAR CONTROLLER");
  showLine(1, "Calibrating...");

  centerX = readCenter(PIN_JOY_X);
  centerY = readCenter(PIN_JOY_Y);
  Serial.printf("Joystick centre X=%d Y=%d\n", centerX, centerY);

  if (!startEspNow(onPacket)) {
    Serial.println("ESP-NOW start failed");
    showLine(1, "RADIO FAILED!");
    delay(2000);
  }
  Serial.println("Controller ready");
}

void loop() {
  static uint32_t lastSendMs = 0, lastLcdMs = 0;
  uint32_t now = millis();

  if (updateButton(btnUp, now)) speedUpCount++;
  if (updateButton(btnDown, now)) speedDownCount++;
  bool hornWasPressed = btnHorn.pressed;
  updateButton(btnHorn, now);

  readRadio(now);

  // Send on schedule, and straight away when the horn changes so it feels instant
  if (now - lastSendMs >= SEND_INTERVAL_MS || btnHorn.pressed != hornWasPressed) {
    lastSendMs = now;
    joyX = readAxis(PIN_JOY_X, centerX, INVERT_X);
    joyY = readAxis(PIN_JOY_Y, centerY, INVERT_Y);
    sendControl();
  }
  if (now - lastLcdMs >= 200) {
    lastLcdMs = now;
    updateLcd(now);
  }
}
