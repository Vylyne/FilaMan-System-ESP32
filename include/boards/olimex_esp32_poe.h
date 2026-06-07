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
// =============================================================================

// I2C Pins
static const uint8_t I2C_SDA = 13; // GPIO 13
static const uint8_t I2C_SCL = 14; // GPIO 14

// NFC PN532 pins 
static const uint8_t PN532_IRQ = 32; // GPIO 32
static const uint8_t PN532_RESET = 33; // GPIO 33

// HX711 Scale pins
static const uint8_t LOADCELL_DOUT_PIN = 35;  // GPIO 35
static const uint8_t LOADCELL_SCK_PIN = 4;   // GPIO 4

// TTP223 (Touch module) pins
static const uint8_t TTP223_PIN = 5; // GPIO 5

// Ethernet Pin Abstraction
// If ETH_CLK_PIN isn't passed by platformio.ini, default to GPIO 0 (WROOM standard)
#ifndef ETH_CLK_PIN
  #define ETH_CLK_PIN 17  // GPIO 17 for WROOM, override with 0 for WROVER
#endif

// LAN8720 via RMII
#define ETH_PHY_TYPE    ETH_PHY_LAN8720
#define ETH_PHY_ADDR    0
#define ETH_PHY_MDC     23
#define ETH_PHY_MDIO    18
#define ETH_PHY_POWER   12   // verify against your specific board revision

// Derive CLK mode at compile time from CLK pin
#if ETH_CLK_PIN == 0
  #define ETH_CLK_MODE  ETH_CLOCK_GPIO0_OUT
#else
  #define ETH_CLK_MODE  ETH_CLOCK_GPIO17_OUT
#endif

#endif // BOARD_OLIMEX_ESP32_POE_H
