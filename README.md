# RFID Speed-Zone Car

An RC car that reads RFID tags in the road:

- **School zone** – the car caps its own speed level.
- **Hospital zone** – the horn is disabled.

A handheld controller drives it: a joystick for direction, buttons for Speed+, Speed− and Horn. Both units show the zone and speed on a 16x2 LCD.

| Folder | Upload to | Purpose |
|---|---|---|
| `TagWriter/` | Car ESP32 (temporarily) | Writes zone data onto the RFID tags |
| `CarFirmware/` | Car ESP32 | Drives the motors, reads tags, enforces zone rules |
| `ControllerFirmware/` | Controller ESP32 | Reads joystick/buttons, shows status |

## 1. Arduino IDE setup

1. **Add the ESP32 boards:** *Tools → Board → Boards Manager*, search **esp32**, install **esp32 by Espressif Systems**. Version 3.x is preferred; 2.x also works.
2. **Install the libraries:** *Sketch → Include Library → Manage Libraries*:
   - **MFRC522** by GithubCommunity
   - **LiquidCrystal I2C** by Frank de Brabander. Ignore any "may be incompatible with ESP32" warning.
3. **Choose the board:** *Tools → Board → esp32 → **ESP32 Dev Module***.

## 2. Write the road tags

1. Upload `TagWriter` to the **car** ESP32.
2. Open *Serial Monitor* at **115200** baud, with line ending set to *Newline*.
3. Type a number, press Enter, then hold a tag on the reader:

   | Key | Writes |
   |---|---|
   | 1 | SCHOOL START (limit = `SCHOOL_LIMIT_LEVEL`, default 4) |
   | 2 | SCHOOL END |
   | 3 | HOSPITAL START |
   | 4 | HOSPITAL END |
   | 5 | Read a tag and show its contents |

4. Label each tag after writing it.

Supported tags: MIFARE Classic 1K cards and fobs (the ones bundled with MFRC522 kits), and NTAG213/215/216 stickers.

## 3. Upload the car and controller

1. Upload `CarFirmware` to the car ESP32.
2. Upload `ControllerFirmware` to the controller ESP32.
3. Power on the controller **with the joystick untouched**. The joystick's centre position is calibrated at start-up.

No MAC addresses are needed. Both boards broadcast, and packets are matched by `PAIR_ID` in `Protocol.h`.

## 4. First test (wheels off the ground)

1. **Controller alone:** the LCD shows `SEARCHING CAR...` and the live joystick values. Pushing forward should give **Y+**, and pushing right should give **X+**. If either is backwards, set `INVERT_Y` / `INVERT_X` to `true` in `ControllerFirmware.ino`.
2. **Turn on the car:** both LCDs should show `ZONE: NORMAL` and `SPEED 3/10`.
3. **Push the joystick forward:** both sides should spin forward. If a side spins backwards, set `LEFT_REVERSED` / `RIGHT_REVERSED` in `CarFirmware.ino`, or swap that side's motor wires.
4. **Speed+ / Speed−:** the level changes between 1 and 10.
5. **Tags:** hold a SCHOOL START tag under the reader. The level should drop to 4 or below, and the controller shows `SET 7  ACTUAL 4`. A SCHOOL END tag restores it.
6. **Hospital:** after a HOSPITAL START tag, pressing Horn makes no sound and the controller flashes `HORN DISABLED!`.
7. **Failsafe:** switch the controller off. Within 0.3 s the car must stop and show `LINK LOST`.

## Settings you may want to change

| Setting | File | Default | What it does |
|---|---|---|---|
| `SCHOOL_LIMIT_LEVEL` | TagWriter | 4 | Speed cap stored on SCHOOL START tags (rewrite the tags after changing it) |
| `START_LEVEL` | Car | 3 | Speed level after power-on |
| `PWM_MIN` | Car | 110 | Motor power at level 1. Raise it if the car doesn't move at low levels. |
| `TURN_STRENGTH` | Car | 0.7 | How sharply it steers while driving |
| `SPIN_MIN_PWM` | Car | 160 | Power used when spinning on the spot |
| `RAMP_STEP` | Car | 12 | Higher = snappier acceleration and braking, lower = smoother |
| `BUZZER_ACTIVE_LOW` | Car | false | Set to true if the buzzer beeps all the time |
| `LCD_ADDR` | Car + Controller | 0x27 | Use 0x3F if the LCD lights up but shows nothing |
| `PAIR_ID` | Protocol.h | 0x5C | Give each car/controller pair a different value |

## Shared files

`Protocol.h`, `ZoneTag.h` and `EspNowLink.h` are copied into each sketch folder, because the Arduino IDE only compiles files inside a sketch's own folder. **If you edit one, copy it to the other folders too.**

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| LCD backlight on, no text | Wrong `LCD_ADDR`, or contrast pot needs turning |
| `RFID NOT FOUND!` on the car LCD | MFRC522 wiring. It must be on **3.3V**, not 5V. |
| Tags not detected while driving | Reader too high (keep it 1–2 cm above the road), metal nearby, or car too fast |
| Car resets or LCD shows garbage when motors start | Motor noise. Solder a 100 nF capacitor across each motor's terminals. |
| `SEARCHING CAR...` never clears | Car not powered, or `PAIR_ID` differs between the two boards |
| Car doesn't move at low speed levels | Raise `PWM_MIN` |
