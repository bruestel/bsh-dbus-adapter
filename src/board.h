/*
   Board pin mapping, resolved from Kconfig.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#pragma once

#include "sdkconfig.h"
#include <cstdint>
#include <driver/gpio.h>

namespace board {

inline constexpr int PIN_RX = CONFIG_BSH_PIN_RX;

/* Handed to the UART driver but never written to.

   Verified against the Bouni BSH-Board schematic:

       +3.3V -- R14 (10k) -- net DBUS-TX --+-- U1 74LVC1G07 (input)
                                           +-- ESP32-C6 GPIO5

   U1 is a non-inverting buffer with an OPEN-DRAIN output, and R14 pulls the
   net high. So the bus is released whenever this pin is high -- including
   while it floats during reset and boot. The board is fail-safe: firmware
   doing nothing can never disturb the bus.

   Handing the pin to the UART is therefore a convention, not a safety
   requirement; it matches the ESPHome configurations that have run on this
   board for years. What matters is that nothing ever writes to it. */
inline constexpr int PIN_TX = CONFIG_BSH_PIN_TX;
inline constexpr int PIN_LED_STATUS = CONFIG_BSH_PIN_LED_STATUS;
inline constexpr int PIN_LED_ACTIVITY = CONFIG_BSH_PIN_LED_ACTIVITY;

#ifdef CONFIG_BSH_LED_ACTIVE_LOW
inline constexpr bool LED_ACTIVE_LOW = true;
#else
inline constexpr bool LED_ACTIVE_LOW = false;
#endif

/* Which adapter this image was built for. Reported over HTTP so a device can
   say what it is rather than leaving it to be inferred from its pins, and so
   the profile export can write a configuration for the right hardware. */
#if defined(CONFIG_BSH_BOARD_BOUNI_C6)
inline constexpr const char *NAME = "bouni-c6";
#elif defined(CONFIG_BSH_BOARD_KIU_C3)
inline constexpr const char *NAME = "kiu-c3";
#else
inline constexpr const char *NAME = "custom";
#endif

inline constexpr int DEFAULT_BAUD = CONFIG_BSH_BUS_DEFAULT_BAUD;
inline constexpr int PIN_BOOT = CONFIG_BSH_PIN_BOOT;

static_assert(PIN_RX >= 0, "CONFIG_BSH_PIN_RX is unset -- pick a board in menuconfig");
static_assert(PIN_TX >= 0, "CONFIG_BSH_PIN_TX is unset -- pick a board in menuconfig");
static_assert(PIN_RX != PIN_TX, "RX and TX must be different pins");
/* A button sharing a pin with the bus is not a configuration mistake that shows
   up as a wrong reading: whichever driver is initialised last owns the pin, so
   one of the two silently stops working. Worse on a board where the bus TX line
   is the one being shorted to ground by a button press. */
static_assert(PIN_BOOT < 0 || (PIN_BOOT != PIN_RX && PIN_BOOT != PIN_TX),
              "CONFIG_BSH_PIN_BOOT collides with the bus pins");

inline constexpr bool has_led(int pin) { return pin >= 0; }

/* The Bouni board drives its LEDs active low; keep that detail in one place. */
inline uint32_t led_level(bool on) { return static_cast<uint32_t>(LED_ACTIVE_LOW ? !on : on); }

inline void led_init(int pin) {
  if (!has_led(pin))
    return;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
  gpio_set_level(static_cast<gpio_num_t>(pin), led_level(false));
}

inline void button_init(int pin) {
  if (pin < 0)
    return;
  gpio_config_t cfg = {
      .pin_bit_mask = 1ULL << pin,
      .mode = GPIO_MODE_INPUT,
      /* The button shorts to ground, so the pin needs pulling up to read high
         when nobody is touching it. */
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  gpio_config(&cfg);
}

inline bool button_pressed(int pin) {
  return pin >= 0 && gpio_get_level(static_cast<gpio_num_t>(pin)) == 0;
}

inline void led_set(int pin, bool on) {
  if (has_led(pin))
    gpio_set_level(static_cast<gpio_num_t>(pin), led_level(on));
}

}  // namespace board
