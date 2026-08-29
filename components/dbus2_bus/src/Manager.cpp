/*
   D-Bus bus manager.

   (C) 2026 Jonas Brüstel
   Derived from Open-DBus2, (C) 2024-2026 Hajo Noerenberg
   https://github.com/hn/bsh-home-appliances

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License version 3.0 as
   published by the Free Software Foundation.
*/

#include "dbus2/Manager.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/task.h>

#include <algorithm>
#include <new>

namespace dbus2 {

static const char *const TAG = "dbus2";

bool Manager::configure_uart(uint32_t baud) {
  uart_config_t cfg = {};
  cfg.baud_rate = static_cast<int>(baud);
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  esp_err_t err = uart_param_config(cfg_.uart, &cfg);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_param_config(%" PRIu32 ") failed: %s", baud, esp_err_to_name(err));
    return false;
  }
  baud_ = baud;
  proto_.set_baud(baud);
  return true;
}

uint32_t Manager::detect_baud(int dwell_ms) {
  /* Runs before the bus task is started, so we can drive the UART directly
     without any locking. Each candidate gets the same listening window; the one
     that yields the most CRC-valid frames wins. A wrong rate produces framing
     garbage that essentially never passes CRC, so this is a sharp signal. */
  uint32_t best_baud = 0;
  uint32_t best_frames = 0;

  for (uint32_t cand : kBaudCandidates) {
    if (!configure_uart(cand))
      continue;
    uart_flush_input(cfg_.uart);
    proto_.reset();
    proto_.reset_stats();

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(dwell_ms) * 1000;
    uint8_t b;
    while (esp_timer_get_time() < deadline) {
      int64_t now = esp_timer_get_time();
      proto_.tick(now);
      if (uart_read_bytes(cfg_.uart, &b, 1, pdMS_TO_TICKS(1)) > 0)
        proto_.feed(b, esp_timer_get_time());
    }

    const Stats &s = proto_.stats();
    ESP_LOGI(TAG, "baud %6" PRIu32 ": %" PRIu32 " frames, %" PRIu32 " crc errors, %" PRIu32 " bytes", cand,
             s.frames_ok, s.crc_errors, s.bytes);

    if (s.frames_ok > best_frames) {
      best_frames = s.frames_ok;
      best_baud = cand;
    }
  }

  if (best_frames == 0) {
    ESP_LOGW(TAG, "No valid frames at any candidate rate. Is the bus connected and the appliance powered?");
    return 0;
  }
  ESP_LOGI(TAG, "Detected %" PRIu32 " baud (%" PRIu32 " frames)", best_baud, best_frames);
  return best_baud;
}

bool Manager::begin(const Config &cfg) {
  cfg_ = cfg;

  if (cfg_.rx_pin < 0) {
    ESP_LOGE(TAG, "No RX pin configured");
    return false;
  }

  rx_queue_ = xQueueCreate(cfg_.rx_queue_len, sizeof(RxJob *));
  if (!rx_queue_) {
    ESP_LOGE(TAG, "xQueueCreate failed");
    return false;
  }

  if (!configure_uart(cfg_.baud ? cfg_.baud : kBaudCandidates[0]))
    return false;

  /* TX is handed to the driver so the pin rests at a defined level. On the
     Bouni board the line driver is an open-drain 74LVC1G07 with a pull-up, so
     the bus stays released either way -- but nothing here ever writes to it. */
  esp_err_t err = uart_set_pin(cfg_.uart, cfg_.tx_pin, cfg_.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
    return false;
  }

  /* No TX ring buffer: we never transmit. */
  err = uart_driver_install(cfg_.uart, 256, 0, 0, nullptr, 0);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
    return false;
  }
  uart_set_rx_full_threshold(cfg_.uart, 1);

  if (cfg_.baud == 0) {
    uint32_t detected = detect_baud(cfg_.scan_dwell_ms);
    if (!configure_uart(detected ? detected : kBaudCandidates[0]))
      return false;
  }

  proto_.set_frame_handler([this](const Frame &f, int64_t ts) {
    auto *job = new (std::nothrow) RxJob{f, ts};
    if (!job)
      return false;
    if (xQueueSend(rx_queue_, &job, 0) != pdPASS) {
      delete job;
      return false;
    }
    return true;
  });

  /* Queued rather than dispatched here: this runs in the bus task, and an
     acknowledgement must reach the observers after the frame it answers, which
     only holds if both take the same road. */
  proto_.set_ack_handler([this](uint8_t ack, int64_t frame_ts) {
    auto *job = new (std::nothrow) RxJob{};
    if (!job)
      return;
    job->timestamp_us = frame_ts;
    job->ack = ack;
    job->is_ack = true;
    if (xQueueSend(rx_queue_, &job, 0) != pdPASS)
      delete job;
  });

  uart_flush_input(cfg_.uart);
  proto_.reset();
  proto_.reset_stats();

  if (xTaskCreate(task_trampoline, "dbus2_bus", 4096, this, cfg_.task_priority, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "xTaskCreate failed");
    return false;
  }

  ESP_LOGI(TAG, "Listening on UART%d, rx=%d tx=%d (never driven), %" PRIu32 " baud, resync %lld us",
           static_cast<int>(cfg_.uart), cfg_.rx_pin, cfg_.tx_pin, baud_,
           static_cast<long long>(proto_.resync_timeout_us()));
  return true;
}

void Manager::task_trampoline(void *arg) { static_cast<Manager *>(arg)->run(); }

void Manager::run() {
  uint8_t b;
  for (;;) {
    /* Blocking for one byte gives the scheduler a natural yield point. The
       timestamp is taken right after, so it approximates the end of the byte
       on the wire -- which is what the resync logic reasons about. */
    int got = uart_read_bytes(cfg_.uart, &b, 1, pdMS_TO_TICKS(1));
    int64_t now = esp_timer_get_time();

    /* Resync first: a byte arriving after a long gap starts a new frame, so the
       leftovers of the previous one must be discarded before feeding it. */
    proto_.tick(now);
    if (got > 0)
      proto_.feed(b, now);
  }
}

Stats Manager::stats() const { return proto_.stats(); }

void Manager::process() {
  RxJob *job;
  const int64_t now = esp_timer_get_time();
  while (xQueueReceive(rx_queue_, &job, 0) == pdPASS) {
    if (job->is_ack) {
      /* Not aged out: an acknowledgement only means anything beside its frame,
         and that frame went through this queue a moment earlier. */
      for (auto &obs : ack_observers_)
        obs(job->ack, job->timestamp_us);
    } else if ((now - job->timestamp_us) <= kRxJobMaxAgeUs) {
      for (auto &obs : observers_)
        obs(job->frame, job->timestamp_us);
    } else {
      ESP_LOGD(TAG, "Stale frame for 0x%02x discarded (age %lld us)", job->frame.dest,
               static_cast<long long>(now - job->timestamp_us));
    }
    delete job;
  }
}

}  // namespace dbus2
