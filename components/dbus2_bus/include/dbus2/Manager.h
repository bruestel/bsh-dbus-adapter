/*
   D-Bus bus manager: UART, receive task, frame dispatch.

   Owns the hardware half that dbus2_proto deliberately does not: UART setup, a
   dedicated FreeRTOS task that pumps bytes into the parser, and a queue handing
   finished frames to the application task.

   The bus is never driven. The TX pin is handed to the UART driver only so it
   rests at a defined level; nothing here ever writes to it.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#pragma once

#include "dbus2/Protocol.h"

#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include <cstdint>
#include <functional>
#include <vector>

namespace dbus2 {

/* Rates seen on B/S/H/ appliances. Older machines use 9600; the COM1 module is
   documented to probe upwards, so newer ones may be faster. */
inline constexpr uint32_t kBaudCandidates[] = {9600, 19200, 38400};

class Manager {
 public:
  struct Config {
    int rx_pin = -1;
    int tx_pin = -1;  // configured, never written
    uart_port_t uart = UART_NUM_1;
    /* 0 means: probe kBaudCandidates and use whichever sees valid frames. */
    uint32_t baud = 9600;
    int scan_dwell_ms = 2000;
    int task_priority = 18;  // above app/network work, below the WiFi stack
    int rx_queue_len = 16;
  };

  using Observer = std::function<void(const Frame &, int64_t rx_time_us)>;

  /* The byte the addressed node answered a frame with, paired to that frame by
     its receive time. */
  using AckObserver = std::function<void(uint8_t ack, int64_t frame_rx_time_us)>;

  bool begin(const Config &cfg);

  /* Register a passive frame observer. Called from process(), i.e. in the
     application task, never in the bus task. */
  void add_observer(Observer obs) { observers_.push_back(std::move(obs)); }
  void add_ack_observer(AckObserver obs) { ack_observers_.push_back(std::move(obs)); }

  /* Drain the receive queue and dispatch. Call regularly from the app task. */
  void process();

  uint32_t baud() const { return baud_; }
  Stats stats() const;

 private:
  /* Frames and acknowledgements travel the same queue on purpose: they are
     produced by the same task in the order they occurred, and anything that
     pairs them later depends on that order surviving the trip. Two queues
     would not guarantee it. */
  struct RxJob {
    Frame frame;
    int64_t timestamp_us;
    uint8_t ack = 0;
    bool is_ack = false;
  };

  /* Runs before the bus task exists, so it can reconfigure the UART freely. */
  uint32_t detect_baud(int dwell_ms);
  bool configure_uart(uint32_t baud);
  static void task_trampoline(void *arg);
  void run();

  Config cfg_{};
  uint32_t baud_ = 0;
  Protocol proto_;
  QueueHandle_t rx_queue_ = nullptr;
  std::vector<Observer> observers_;
  std::vector<AckObserver> ack_observers_;

  /* Frames older than this are dropped rather than dispatched -- a stale
     appliance state is worse than none. */
  static constexpr int64_t kRxJobMaxAgeUs = 500000;
};

}  // namespace dbus2
