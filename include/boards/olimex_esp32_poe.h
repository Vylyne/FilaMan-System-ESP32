#ifndef BOARD_OLIMEX_ESP32_POE_H
#define BOARD_OLIMEX_ESP32_POE_H

#include <Arduino.h>

// =============================================================================
// Olimex ESP32-PoE Board Configuration
// =============================================================================
// 
// Hardware:
//   - ESP32-WROOM-32 (WiFi built-in)
//   - PoE Ethernet interface with PHY chip
//   - Different pin mapping than ESP32-DEV
//
// Pin assignments for HX711 scale and other peripherals may vary.
// This board header provides the base configuration; adjust as needed.
// =============================================================================
// I2C Pins (OLIMEX-ESP32-POE)
#define I2C_SDA 13 // GPIO 13
#define I2C_SCL 14 // GPIO 14

// NFC PN532 pins (OLIMEX-ESP32-POE)
static const uint8_t PN532_IRQ = 32;
static const uint8_t PN532_RESET = 33; 

// HX711 Scale pins (Olimex ESP32-PoE - typically different from ESP32-DEV)
// Note: These may need adjustment based on your specific Olimex board revision
static const uint8_t LOADCELL_DOUT_PIN = 35;  // GPIO 5
static const uint8_t LOADCELL_SCK_PIN = 4;   // GPIO 4

// TTP223 Touch device on OLIMEX ESP32-PoE devices.
static const uint8_t TTP223_PIN = 5; // GPIO 36

// Ethernet Clock Pin Abstraction
// If ETH_CLK_PIN isn't passed by platformio.ini, default to GPIO 0 (WROOM standard)
#ifndef ETH_CLK_PIN
  #define ETH_CLK_PIN 17
#endif

#endif // BOARD_OLIMEX_ESP32_POE_H
