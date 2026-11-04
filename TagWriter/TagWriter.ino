// TagWriter - writes zone data onto the RFID road tags.
//
// Upload this to the CAR's ESP32 (it uses the same MFRC522 wiring),
// open Serial Monitor at 115200 baud, pick a tag type, then hold a tag on the reader.
// Afterwards, upload CarFirmware again.

#include <SPI.h>
#include <MFRC522.h>
#include "ZoneTag.h"

#define RFID_SCK  18
#define RFID_MISO 19
#define RFID_MOSI 23
#define RFID_SS   5
#define RFID_RST  27

// Highest speed level allowed inside a school zone (1-10)
#define SCHOOL_LIMIT_LEVEL 4

MFRC522 rfid(RFID_SS, RFID_RST);

struct Choice {
  const char *name;
  ZoneTag tag;
};

const Choice CHOICES[] = {
  {"SCHOOL START",   {TAG_START, ZONE_LIMIT,   SCHOOL_LIMIT_LEVEL}},
  {"SCHOOL END",     {TAG_END,   ZONE_LIMIT,   0}},
  {"HOSPITAL START", {TAG_START, ZONE_NO_HORN, 0}},
  {"HOSPITAL END",   {TAG_END,   ZONE_NO_HORN, 0}},
};
const int NUM_CHOICES = sizeof(CHOICES) / sizeof(CHOICES[0]);
const int READ_ONLY = NUM_CHOICES;  // menu entry after the write choices

int pending = -1;  // menu entry waiting for a tag, -1 = none

void printMenu() {
  Serial.println();
  Serial.println("==== Zone tag writer ====");
  for (int i = 0; i < NUM_CHOICES; i++) {
    Serial.printf("  %d = write %s\n", i + 1, CHOICES[i].name);
  }
  Serial.printf("  %d = read a tag\n", READ_ONLY + 1);
  Serial.println("Type a number and press Enter:");
}

void printUid() {
  Serial.print("Tag UID:");
  for (byte i = 0; i < rfid.uid.size; i++) Serial.printf(" %02X", rfid.uid.uidByte[i]);
  Serial.printf("  (%s)\n", rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak)));
}

void setup() {
  Serial.begin(115200);
  SPI.begin(RFID_SCK, RFID_MISO, RFID_MOSI, RFID_SS);
  rfid.PCD_Init();
  delay(50);

  byte version = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (version == 0x00 || version == 0xFF) {
    Serial.println("MFRC522 not found - check wiring (3.3V, SDA=5, SCK=18, MOSI=23, MISO=19, RST=27).");
  } else {
    Serial.printf("MFRC522 found, firmware 0x%02X\n", version);
  }
  printMenu();
}

void loop() {
  if (Serial.available()) {
    int choice = Serial.parseInt() - 1;
    while (Serial.available()) Serial.read();  // drop the rest of the line
    if (choice >= 0 && choice <= READ_ONLY) {
      pending = choice;
      Serial.printf("> %s - hold a tag on the reader...\n",
                    choice == READ_ONLY ? "READ" : CHOICES[choice].name);
    } else {
      printMenu();
    }
  }

  if (pending < 0) return;
  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  printUid();
  ZoneTag tag;

  if (pending == READ_ONLY) {
    if (readZoneTag(rfid, tag)) {
      Serial.printf("Zone tag: %s", describeZoneTag(tag));
      if (tag.kind == TAG_START && (tag.flags & ZONE_LIMIT)) Serial.printf(", limit level %u", tag.limitLevel);
      Serial.println();
    } else {
      Serial.println("Not a zone tag (blank, unsupported type, or read failed).");
    }
  } else {
    const Choice &c = CHOICES[pending];
    bool ok = writeZoneTag(rfid, c.tag);
    // read back to verify
    ok = ok && readZoneTag(rfid, tag) && tag.kind == c.tag.kind && tag.flags == c.tag.flags &&
         tag.limitLevel == c.tag.limitLevel;
    Serial.println(ok ? "Written and verified. Label this tag!" : "FAILED - hold the tag still and try again.");
  }

  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  pending = -1;
  printMenu();
}
