#include "smavista_pulse_cmd_fan.h"
#include "esphome/core/log.h"

namespace esphome {
namespace smavista_pulse_cmd_fan {

static const char *const TAG = "smavista_pulse_cmd_fan";

// Firmware command ids; see the header for how they become wire bytes.
const uint8_t SmavistaPulseCmdFan::LEVEL_CMDS[6] = {0x10, 0x12, 0x1c, 0xa, 0xf, 0xc};
const uint8_t SmavistaPulseCmdFan::CMD_OFF = 0x16;
const uint8_t SmavistaPulseCmdFan::CMD_DIRECTION_FORWARD = 0x4;
const uint8_t SmavistaPulseCmdFan::CMD_DIRECTION_REVERSE = 0x11;
const uint8_t SmavistaPulseCmdFan::CMD_WIND = 0x15;

void SmavistaPulseCmdFan::setup() {
  // Stock initialises its enable output disabled and only raises it around fan
  // activity (see set_enable_pin()). With enable_follow_fan off it is held
  // enabled from boot instead.
  if (this->enable_pin_ != nullptr) {
    this->enable_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->set_enable_level_(!this->enable_follow_fan_);
  }

  // Stock's pin init also drives GPIO20 as an output held LOW. Its purpose is
  // unknown; without this ESPHome would leave the pin floating as an input.
  if (this->aux_pin_ != nullptr) {
    this->aux_pin_->pin_mode(gpio::FLAG_OUTPUT);
    this->aux_pin_->digital_write(false);
  }

  this->pin_->pin_mode(gpio::FLAG_OUTPUT);
  this->write_idle_();

  this->traits_ = fan::FanTraits(false, true, true, 6);

  // The saved state is only read here (no transmission); it is applied below,
  // after the boot-time OFF.
  auto restore = this->restore_state_();

  // Stock's first frame after power-up is an unprompted OFF, sent before any
  // button is pressed. We do the same, then apply any restored state.
  //
  // It is deferred rather than sent inline: transmitting from setup() crashed
  // and rebooted the device, because WiFi and the RTOS scheduler are not yet
  // stable that early in boot.
  this->set_timeout(2000, [this, restore]() mutable {
    this->send_command_(SmavistaPulseCmdFan::CMD_OFF, "boot-init-off");
    if (restore.has_value()) {
      restore->apply(*this);
    }
  });
}

void SmavistaPulseCmdFan::dump_config() {
  LOG_FAN("", "SMAVISTA Pulse Fan", this);
  ESP_LOGCONFIG(TAG, "  Pulse widths: short=%uus long=%uus, frame gap=%uus", (unsigned) this->short_us_,
                (unsigned) this->long_us_, (unsigned) this->gap_us_);
  ESP_LOGCONFIG(TAG, "  Inverted: %s", YESNO(this->inverted_));
  ESP_LOGCONFIG(TAG, "  Repeat: %u", this->repeat_);
  ESP_LOGCONFIG(TAG, "  Stock waveform (idle HIGH + frame trailer): %s, trailer blip=%uus",
                YESNO(this->stock_waveform_), (unsigned) this->blip_us_);
  LOG_PIN("  Pin: ", this->pin_);
  if (this->aux_pin_ != nullptr) {
    LOG_PIN("  Aux pin (held LOW, as stock): ", this->aux_pin_);
  }
  if (this->enable_pin_ != nullptr) {
    ESP_LOGCONFIG(TAG, "  Enable inverted: %s", YESNO(this->enable_inverted_));
    ESP_LOGCONFIG(TAG, "  Enable follows fan: %s (lead %ums before first command, %ums after OFF)",
                  YESNO(this->enable_follow_fan_), (unsigned) this->enable_lead_ms_,
                  (unsigned) this->enable_off_delay_ms_);
    LOG_PIN("  Enable pin: ", this->enable_pin_);
  }
}

void SmavistaPulseCmdFan::control(const fan::FanCall &call) {
  bool state_changed = call.get_state().has_value() && *call.get_state() != this->state;
  bool dir_changed = call.get_direction().has_value() && *call.get_direction() != this->direction;

  if (call.get_state().has_value())
    this->state = *call.get_state();
  if (call.get_speed().has_value())
    this->speed = *call.get_speed();
  if (call.get_direction().has_value())
    this->direction = *call.get_direction();

  // Speed/off first, then direction: when one stock update turns the fan on and
  // sets the direction, the speed command goes out before the direction one.
  if (!this->state) {
    this->wind_active_ = false;
    this->send_command_(SmavistaPulseCmdFan::CMD_OFF, "off");
    // Stock drops its enable ~100ms after the OFF command, not with it.
    this->schedule_disable_();
  } else if (state_changed || call.get_speed().has_value()) {
    // A bare "turn on" re-sends the last speed, as the stock remote does.
    this->send_speed_command_();
  }

  if (dir_changed) {
    // The direction commands are absolute, not a toggle: forward is cmd 0x4,
    // reverse is cmd 0x11.
    if (this->direction == fan::FanDirection::FORWARD) {
      this->send_direction_forward();
    } else {
      this->send_direction_reverse();
    }
  }

  this->publish_state();
}

void SmavistaPulseCmdFan::send_speed_command_() {
  this->wind_active_ = false;
  this->ensure_enabled_();
  // clamp() also covers speed == 0 (never set) by defaulting to level 1.
  int idx = clamp(this->speed, 1, 6) - 1;
  this->send_command_(SmavistaPulseCmdFan::LEVEL_CMDS[idx], "speed");
}

void SmavistaPulseCmdFan::send_wind() {
  if (this->state)
    this->ensure_enabled_();
  this->wind_active_ = true;
  this->send_command_(SmavistaPulseCmdFan::CMD_WIND, "wind");
}

void SmavistaPulseCmdFan::send_wind_off() {
  if (!this->wind_active_) {
    ESP_LOGD(TAG, "wind_off: wind mode not active, re-sending speed anyway");
  }
  this->send_speed_command_();
}

void SmavistaPulseCmdFan::set_enable_level_(bool enabled) {
  if (this->enable_pin_ == nullptr)
    return;
  this->enabled_ = enabled;
  this->enable_pin_->digital_write(this->enable_inverted_ ? !enabled : enabled);
}

// Stock raises its enable ~4-14ms before the first speed command of a turn-on.
// Only waits when the line actually transitions. A pending delayed disable from
// a recent OFF is cancelled so a quick off->on doesn't drop the enable
// mid-command.
void SmavistaPulseCmdFan::ensure_enabled_() {
  if (this->enable_pin_ == nullptr || !this->enable_follow_fan_)
    return;
  this->cancel_timeout("enable_off");
  if (!this->enabled_) {
    this->set_enable_level_(true);
    if (this->enable_lead_ms_ > 0)
      delay(this->enable_lead_ms_);  // NOLINT - deliberate short lead, mirrors stock
  }
}

void SmavistaPulseCmdFan::schedule_disable_() {
  if (this->enable_pin_ == nullptr || !this->enable_follow_fan_)
    return;
  this->set_timeout("enable_off", this->enable_off_delay_ms_, [this]() { this->set_enable_level_(false); });
}

void SmavistaPulseCmdFan::build_frame_(uint8_t cmd, uint8_t seq, uint8_t out[4]) {
  uint32_t word = (0x22222UL << 12) | ((seq & 0x7) << 4) | ((cmd & 0x1f) << 7);

  uint8_t checksum_nibble = 0;
  for (int shift = 0x1c; shift >= 0; shift -= 4) {
    checksum_nibble ^= (word >> shift) & 0xf;
  }

  out[0] = (word >> 20) & 0xff;
  out[1] = (word >> 12) & 0xff;
  out[2] = (word >> 8) & 0xff;
  out[3] = (checksum_nibble ^ 0xa) | (word & 0xff);
}

void SmavistaPulseCmdFan::send_command_(uint8_t cmd, const char *label) {
  uint8_t seq = this->sequence_;
  this->sequence_ = (this->sequence_ + 1) & 0x7;
  this->send_command_with_seq_(cmd, seq, label);
}

void SmavistaPulseCmdFan::send_command_with_seq_(uint8_t cmd, uint8_t seq, const char *label) {
  uint8_t frame[4];
  this->build_frame_(cmd, seq, frame);

  // Each command is sent as a burst of `repeat_` copies of the same frame.
  if (this->stock_waveform_) {
    // The stock waveform:
    //
    //   LOW gap, then per frame:
    //     32 bit pulses | trailer (HIGH short, LOW long) | HIGH blip | LOW gap
    //
    // After the last frame only the trailer is sent, then the line returns to
    // idle (HIGH).
    this->write_level_for_(false, this->gap_us_);
    for (int r = 0; r < this->repeat_; r++) {
      this->send_frame_once_(frame);
      this->write_level_for_(true, this->short_us_);  // trailer reuses the bit-0 widths
      this->write_level_for_(false, this->long_us_);
      if (r + 1 < this->repeat_) {
        this->write_level_for_(true, this->blip_us_);
        this->write_level_for_(false, this->gap_us_);
      }
    }
  } else {
    // Bare 32-bit frames separated by the LOW gap; the line idles LOW.
    for (int r = 0; r < this->repeat_; r++) {
      this->send_frame_once_(frame);
      if (r + 1 < this->repeat_) {
        this->write_level_for_(false, this->gap_us_);
      }
    }
  }
  this->write_idle_();

  ESP_LOGD(TAG, "Sent %s command (cmd=%02X seq=%u, x%u): %02X %02X %02X %02X", label, cmd, seq, this->repeat_,
           frame[0], frame[1], frame[2], frame[3]);
}

void SmavistaPulseCmdFan::send_seq_sweep() {
  // Speed 6 rather than OFF: spinning up from the default-off state is easier
  // to notice than a stop.
  uint8_t cmd = SmavistaPulseCmdFan::LEVEL_CMDS[5];
  this->ensure_enabled_();
  ESP_LOGD(TAG, "Starting seq sweep: speed6 with seq=0..7, 1s apart");
  for (uint8_t seq = 0; seq < 8; seq++) {
    this->send_command_with_seq_(cmd, seq, "seq-sweep-speed6");
    delay(1000);  // NOLINT - deliberate blocking pause between attempts for manual observation
  }
  ESP_LOGD(TAG, "Seq sweep complete");
}

void SmavistaPulseCmdFan::send_frame_once_(const uint8_t frame[4]) {
  for (int byte_idx = 0; byte_idx < 4; byte_idx++) {
    uint8_t data = frame[byte_idx];
    for (int bit_idx = 7; bit_idx >= 0; bit_idx--) {  // MSB first
      this->write_bit_(((data >> bit_idx) & 1) != 0);
    }
  }
}

// bit 0 = short HIGH + long LOW, bit 1 = long HIGH + short LOW. This is not
// UART: there are no start/stop bits, just 32 of these pulses per frame.
//
// Interrupts are deliberately left enabled. Wrapping bits (or whole frames) in
// critical sections breaks micros() timekeeping and stalls frames long enough
// to corrupt them and crash the device; the plain busy-wait is reliable.
void SmavistaPulseCmdFan::write_bit_(bool bit) {
  uint32_t high_us = bit ? this->long_us_ : this->short_us_;
  uint32_t low_us = bit ? this->short_us_ : this->long_us_;
  this->write_level_for_(true, high_us);
  this->write_level_for_(false, low_us);
}

// The stock firmware initialises the TX pin HIGH and leaves it HIGH after every
// burst. (stock_waveform off keeps the older idle-LOW behaviour.)
void SmavistaPulseCmdFan::write_idle_() {
  bool idle_high = this->stock_waveform_;
  this->pin_->digital_write(this->inverted_ ? !idle_high : idle_high);
}

void SmavistaPulseCmdFan::write_level_for_(bool high, uint32_t duration_us) {
  this->pin_->digital_write(this->inverted_ ? !high : high);
  uint32_t t = micros() + duration_us;
  while ((int32_t) (micros() - t) < 0) {
    // busy-wait: durations are short enough (hundreds of us) that yielding
    // would risk missing the deadline on a loaded system
  }
}

}  // namespace smavista_pulse_cmd_fan
}  // namespace esphome
