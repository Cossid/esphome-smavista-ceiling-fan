#pragma once

#include "esphome/core/component.h"
#include "esphome/core/automation.h"
#include "esphome/core/hal.h"
#include "esphome/components/fan/fan.h"

namespace esphome {
namespace smavista_pulse_cmd_fan {

// Controls the SMAVISTA ceiling fan by sending commands to its motor
// controller (a Linko LKS32MC035) over a single GPIO, reproducing the stock
// firmware's transmit waveform. Background and evidence: ANALYSIS.md.
//
// WAVEFORM (ANALYSIS.md section 2.3). Not UART: a pulse-width-per-bit scheme.
//   - The line idles HIGH.
//   - Each bit is one ~1000us slot, a HIGH phase then a LOW phase:
//       bit 0: 250us HIGH, 750us LOW
//       bit 1: 750us HIGH, 250us LOW
//   - A frame is 32 bits, MSB first (4 bytes, MSB-first), no start/stop bits.
//   - A frame is followed by a trailer: HIGH 250us, LOW 750us, HIGH ~100us,
//     then an ~8.1ms LOW gap.
//   - A command is a burst of 6 copies of the frame. After the sixth, only the
//     trailer's HIGH/LOW is sent and the line returns to idle HIGH.
//
// FRAME CONTENT (ANALYSIS.md section 2.1):
//
//   word  = (0x22222 << 12) | (seq << 4) | ((cmd & 0x1F) << 7)
//   byte0 = (word >> 20) & 0xFF   -> always 0x22
//   byte1 = (word >> 12) & 0xFF   -> always 0x22
//   byte2 = (word >> 8)  & 0xFF   =  0x20 | ((cmd >> 1) & 0x0F)
//   byte3 = (checksum_nibble(word) ^ 0xA) | (word & 0xFF)
//
// checksum_nibble XORs all eight nibbles of `word`. `seq` is a rolling 3-bit
// counter incremented per command and shared by all commands. `cmd` is the
// firmware's internal command id: 0x10/0x12/0x1c/0xa/0xf/0xc for speeds 1-6,
// 0x16 off, 0x4 direction forward, 0x11 direction reverse, 0x15 wind. byte2
// carries only cmd's upper bits; cmd's lowest bit lands in byte3's checksum
// nibble.
class SmavistaPulseCmdFan : public Component, public fan::Fan {
 public:
  void set_pin(GPIOPin *pin) { this->pin_ = pin; }
  // Invert every level (idle included), for a signal-inverting stage between
  // this GPIO and the LKS32MC035. This board has none, so leave it false.
  void set_inverted(bool inverted) { this->inverted_ = inverted; }
  // true (default): the full waveform above. false: bare 32-bit frames with the
  // line idling LOW, kept for A/B comparison.
  void set_stock_waveform(bool stock) { this->stock_waveform_ = stock; }
  // Width of the short HIGH blip between a frame's trailer and the LOW gap.
  void set_trailer_blip_us(uint32_t us) { this->blip_us_ = us; }
  // Optional. The stock firmware's pin init also drives GPIO20 as an output
  // held LOW; its purpose is unknown, and this simply matches it.
  void set_aux_pin(GPIOPin *pin) { this->aux_pin_ = pin; }
  // Optional. The stock firmware drives a GPIO22 output (its `lowpower_onoff`)
  // that is high while the fan or light is on, sequenced around fan commands
  // (ANALYSIS.md section 3):
  //   - turning on:  goes high ~4-14ms BEFORE the first speed command
  //   - turning off: the OFF command goes out first, then the pin goes low
  //                  ~100ms AFTER it
  //   - at boot:     an output driven low
  // With enable_follow_fan (default) this is mirrored. With it off, the pin is
  // held enabled from setup() onward. enable_inverted flips the polarity.
  void set_enable_pin(GPIOPin *pin) { this->enable_pin_ = pin; }
  void set_enable_inverted(bool inverted) { this->enable_inverted_ = inverted; }
  void set_enable_follow_fan(bool follow) { this->enable_follow_fan_ = follow; }
  void set_enable_lead_ms(uint32_t ms) { this->enable_lead_ms_ = ms; }
  void set_enable_off_delay_ms(uint32_t ms) { this->enable_off_delay_ms_ = ms; }
  void set_short_pulse_us(uint32_t us) { this->short_us_ = us; }
  void set_long_pulse_us(uint32_t us) { this->long_us_ = us; }
  void set_frame_gap_us(uint32_t us) { this->gap_us_ = us; }
  // Copies of each frame per command (stock: 6).
  void set_repeat(uint8_t repeat) { this->repeat_ = repeat; }

  void setup() override;
  void dump_config() override;
  fan::FanTraits get_traits() override { return this->traits_; }

  /// Absolute direction commands (cmd 0x4 = forward, cmd 0x11 = reverse). The
  /// fan's own `direction` trait uses these (see control()); they are also
  /// exposed as manual actions.
  void send_direction_forward() {
    this->send_command_(SmavistaPulseCmdFan::CMD_DIRECTION_FORWARD, "direction_forward");
  }
  void send_direction_reverse() {
    this->send_command_(SmavistaPulseCmdFan::CMD_DIRECTION_REVERSE, "direction_reverse");
  }
  /// cmd 0x15, the remote's "wind" (natural wind) button. Exposed as a manual
  /// action; not a fan preset, as the mode's on-fan behaviour isn't characterised.
  void send_wind();
  /// Leave wind mode. Stock has no separate command for this: it re-sends the
  /// current speed command, and so does this.
  void send_wind_off();
  /// Diagnostic: sends speed 6 eight times with seq explicitly cycled 0-7, one
  /// second apart, to see whether any particular sequence value matters.
  void send_seq_sweep();

 protected:
  void control(const fan::FanCall &call) override;
  void send_command_(uint8_t cmd, const char *label);
  void send_speed_command_();
  void set_enable_level_(bool enabled);
  void ensure_enabled_();
  void schedule_disable_();
  void send_command_with_seq_(uint8_t cmd, uint8_t seq, const char *label);
  void build_frame_(uint8_t cmd, uint8_t seq, uint8_t out[4]);
  void send_frame_once_(const uint8_t frame[4]);
  void write_bit_(bool bit);
  void write_level_for_(bool high, uint32_t duration_us);
  void write_idle_();

  GPIOPin *pin_{nullptr};
  bool inverted_{false};
  bool stock_waveform_{true};
  uint32_t blip_us_{100};
  GPIOPin *aux_pin_{nullptr};
  GPIOPin *enable_pin_{nullptr};
  bool enable_inverted_{false};
  bool enable_follow_fan_{true};
  uint32_t enable_lead_ms_{10};
  uint32_t enable_off_delay_ms_{100};
  bool enabled_{false};
  bool wind_active_{false};
  uint32_t short_us_{250};
  uint32_t long_us_{750};
  uint32_t gap_us_{8100};
  uint8_t repeat_{6};
  fan::FanTraits traits_;

  // One rolling counter shared by every command, as the stock firmware does.
  // Starts at 1 after boot: the stock firmware's first frame after power-up is
  // the boot-time OFF with seq=1.
  uint8_t sequence_{1};

  static const uint8_t LEVEL_CMDS[6];
  static const uint8_t CMD_OFF;
  static const uint8_t CMD_DIRECTION_FORWARD;
  static const uint8_t CMD_DIRECTION_REVERSE;
  static const uint8_t CMD_WIND;
};

template<typename... Ts> class DirectionForwardAction : public Action<Ts...> {
 public:
  explicit DirectionForwardAction(SmavistaPulseCmdFan *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_direction_forward(); }

 protected:
  SmavistaPulseCmdFan *parent_;
};

template<typename... Ts> class DirectionReverseAction : public Action<Ts...> {
 public:
  explicit DirectionReverseAction(SmavistaPulseCmdFan *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_direction_reverse(); }

 protected:
  SmavistaPulseCmdFan *parent_;
};

template<typename... Ts> class WindAction : public Action<Ts...> {
 public:
  explicit WindAction(SmavistaPulseCmdFan *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_wind(); }

 protected:
  SmavistaPulseCmdFan *parent_;
};

template<typename... Ts> class WindOffAction : public Action<Ts...> {
 public:
  explicit WindOffAction(SmavistaPulseCmdFan *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_wind_off(); }

 protected:
  SmavistaPulseCmdFan *parent_;
};

template<typename... Ts> class SeqSweepAction : public Action<Ts...> {
 public:
  explicit SeqSweepAction(SmavistaPulseCmdFan *parent) : parent_(parent) {}
  void play(Ts... x) override { this->parent_->send_seq_sweep(); }

 protected:
  SmavistaPulseCmdFan *parent_;
};

}  // namespace smavista_pulse_cmd_fan
}  // namespace esphome
