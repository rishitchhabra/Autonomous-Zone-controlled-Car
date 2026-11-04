// Protocol.h - radio packets and zone flags shared by all three sketches.
// CarFirmware, ControllerFirmware and TagWriter each hold a copy of this file.
// Keep all copies identical.
#pragma once
#include <stdint.h>
#include <stdio.h>

// Change PAIR_ID if another car running this firmware is nearby,
// so each controller only drives its own car. Car and controller must match.
#define PAIR_ID        0x5C
#define ESPNOW_CHANNEL 1

#define PKT_CONTROL 1
#define PKT_STATUS  2

// Zone flags (stored on the RFID tags and sent in StatusPacket)
#define ZONE_LIMIT   0x01   // speed limit active (school)
#define ZONE_NO_HORN 0x02   // horn disabled (hospital)

// Speed levels selected with Speed+ / Speed-
#define MIN_LEVEL 1
#define MAX_LEVEL 10

static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Controller -> car, every 50 ms
struct __attribute__((packed)) ControlPacket {
  uint8_t pairId;
  uint8_t type;            // PKT_CONTROL
  int8_t  joyX;            // -100 (left) .. 100 (right)
  int8_t  joyY;            // -100 (back) .. 100 (forward)
  uint8_t speedUpCount;    // +1 on every Speed+ press (wraps), so a lost packet loses no press
  uint8_t speedDownCount;  // +1 on every Speed- press
  uint8_t horn;            // 1 while the Horn button is held
};

// Car -> controller, every 100 ms
struct __attribute__((packed)) StatusPacket {
  uint8_t pairId;
  uint8_t type;            // PKT_STATUS
  uint8_t zoneFlags;       // ZONE_LIMIT | ZONE_NO_HORN
  uint8_t limitLevel;      // valid when ZONE_LIMIT is set
  uint8_t setLevel;        // level the driver asked for
  uint8_t actualLevel;     // level the car is really using
  uint8_t hornBlocked;     // 1 while horn is pressed but blocked by a hospital zone
};

// First LCD line, identical on car and controller.
inline void formatZoneLine(char *buf, size_t size, uint8_t flags, uint8_t limitLevel) {
  if ((flags & ZONE_LIMIT) && (flags & ZONE_NO_HORN)) snprintf(buf, size, "SCH+HOSP LIM %u", limitLevel);
  else if (flags & ZONE_LIMIT)                        snprintf(buf, size, "SCHOOL  LIMIT %u", limitLevel);
  else if (flags & ZONE_NO_HORN)                      snprintf(buf, size, "HOSPITAL NO HORN");
  else                                                snprintf(buf, size, "ZONE: NORMAL");
}
