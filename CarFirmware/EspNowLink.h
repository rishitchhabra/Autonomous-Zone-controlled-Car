// EspNowLink.h - ESP-NOW setup shared by CarFirmware and ControllerFirmware.
// Keep both copies identical.
//
// Both boards broadcast on ESPNOW_CHANNEL, so no MAC addresses need to be typed in.
// Packets carry PAIR_ID so a board ignores packets from other pairs.
#pragma once
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_arduino_version.h>
#include "Protocol.h"

typedef void (*PacketHandler)(const uint8_t *data, int len);
static PacketHandler packetHandler = nullptr;

// The receive callback signature changed in ESP32 Arduino core 3.x
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static void espNowReceive(const esp_now_recv_info_t *, const uint8_t *data, int len) {
#else
static void espNowReceive(const uint8_t *, const uint8_t *data, int len) {
#endif
  if (packetHandler) packetHandler(data, len);
}

inline bool startEspNow(PacketHandler handler) {
  packetHandler = handler;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

  if (esp_now_init() != ESP_OK) return false;
  esp_now_register_recv_cb(espNowReceive);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

inline void broadcastPacket(const void *data, size_t len) {
  esp_now_send(BROADCAST_ADDR, (const uint8_t *)data, len);
}
