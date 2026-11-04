// ZoneTag.h - data format of the RFID road tags, shared by TagWriter and CarFirmware.
// Keep both copies identical.
//
// Supports MIFARE Classic 1K cards/fobs (the ones bundled with MFRC522 kits)
// and NTAG213/215/216 stickers.
//
// Bytes written to block 4 (Classic) or pages 4-5 (NTAG):
//   [0]='R' [1]='Z' [2]=version [3]=TAG_START/TAG_END [4]=zone flags [5]=limit level
#pragma once
#include <MFRC522.h>
#include "Protocol.h"

#define TAG_MAGIC0     'R'
#define TAG_MAGIC1     'Z'
#define TAG_VERSION    1
#define TAG_START      1
#define TAG_END        2
#define TAG_DATA_BLOCK 4

struct ZoneTag {
  uint8_t kind;        // TAG_START or TAG_END
  uint8_t flags;       // ZONE_LIMIT and/or ZONE_NO_HORN
  uint8_t limitLevel;  // speed level cap, used with ZONE_LIMIT on a START tag
};

inline bool isClassicTag(MFRC522::PICC_Type type) {
  return type == MFRC522::PICC_TYPE_MIFARE_MINI ||
         type == MFRC522::PICC_TYPE_MIFARE_1K ||
         type == MFRC522::PICC_TYPE_MIFARE_4K;
}

// Classic cards need a login before each read/write; new cards use the factory key FF..FF.
inline bool authenticateClassic(MFRC522 &rfid) {
  MFRC522::MIFARE_Key key;
  for (byte i = 0; i < 6; i++) key.keyByte[i] = 0xFF;
  return rfid.PCD_Authenticate(MFRC522::PICC_CMD_MF_AUTH_KEY_A, TAG_DATA_BLOCK, &key, &rfid.uid) ==
         MFRC522::STATUS_OK;
}

// Reads the zone data from the card just selected with PICC_ReadCardSerial().
// Call rfid.PICC_HaltA() and rfid.PCD_StopCrypto1() afterwards.
inline bool readZoneTag(MFRC522 &rfid, ZoneTag &out) {
  MFRC522::PICC_Type type = MFRC522::PICC_GetType(rfid.uid.sak);
  if (isClassicTag(type)) {
    if (!authenticateClassic(rfid)) return false;
  } else if (type != MFRC522::PICC_TYPE_MIFARE_UL) {
    return false;
  }

  byte buf[18];
  byte size = sizeof(buf);
  if (rfid.MIFARE_Read(TAG_DATA_BLOCK, buf, &size) != MFRC522::STATUS_OK) return false;
  if (buf[0] != TAG_MAGIC0 || buf[1] != TAG_MAGIC1 || buf[2] != TAG_VERSION) return false;
  if (buf[3] != TAG_START && buf[3] != TAG_END) return false;

  out.kind = buf[3];
  out.flags = buf[4];
  out.limitLevel = buf[5];
  return true;
}

// Writes zone data to the card just selected with PICC_ReadCardSerial().
inline bool writeZoneTag(MFRC522 &rfid, const ZoneTag &tag) {
  byte data[16] = {TAG_MAGIC0, TAG_MAGIC1, TAG_VERSION, tag.kind, tag.flags, tag.limitLevel};
  MFRC522::PICC_Type type = MFRC522::PICC_GetType(rfid.uid.sak);

  if (isClassicTag(type)) {
    if (!authenticateClassic(rfid)) return false;
    return rfid.MIFARE_Write(TAG_DATA_BLOCK, data, 16) == MFRC522::STATUS_OK;
  }
  if (type == MFRC522::PICC_TYPE_MIFARE_UL) {
    // NTAG pages are 4 bytes; the payload fits in two pages
    for (byte page = 0; page < 2; page++) {
      if (rfid.MIFARE_Ultralight_Write(TAG_DATA_BLOCK + page, data + 4 * page, 4) != MFRC522::STATUS_OK) {
        return false;
      }
    }
    return true;
  }
  return false;
}

inline const char *describeZoneTag(const ZoneTag &tag) {
  bool start = tag.kind == TAG_START;
  if ((tag.flags & ZONE_LIMIT) && (tag.flags & ZONE_NO_HORN)) return start ? "SCHOOL+HOSPITAL START" : "SCHOOL+HOSPITAL END";
  if (tag.flags & ZONE_LIMIT)   return start ? "SCHOOL START" : "SCHOOL END";
  if (tag.flags & ZONE_NO_HORN) return start ? "HOSPITAL START" : "HOSPITAL END";
  return "UNKNOWN";
}
