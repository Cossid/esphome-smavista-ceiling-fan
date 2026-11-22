# SMAVISTA Ceiling Fan (BK7238) — firmware analysis

Source files: `SMAVISTA_Ceiling-Fan-v1.0.2.bin` (2 MB raw flash dump: bootloader,
app and data partitions) and `SMAVISTA_Ceiling-Fan-v1.0.3.bin` (OTA update, app
partition only; its fan-control code is identical to v1.0.2).

## Status summary

- **Working.** The ESPHome component (`components/smavista_pulse_cmd_fan/`,
  configured by `fan-pulse-cmd-config.yaml`) controls the fan: speeds,
  direction, wind and off.
- **Architecture:** the BK7238 does not drive the motor. It sends commands over
  **one GPIO (GPIO14)** to a separate motor-control MCU, a Linko **LKS32MC035**,
  which commutates the 3-phase BLDC motor itself (§1).
- **Protocol:** frame content, checksum and command ids are fully decoded (§2.1,
  §2.2). The **waveform** is the stock firmware's, reproduced from its own
  transmit code: line idles HIGH, and every frame is followed by a trailer
  (§2.3).
- **Two stock behaviours the component now matches** (and that earlier versions
  lacked): the waveform above, and a GPIO22 output (`lowpower_onoff`) that stock
  sequences around fan on/off, plus GPIO20 held LOW (§3). Which of these is
  strictly required has not been isolated (§4).

## 1. Hardware architecture and pin map

### 1.1 Architecture

A second MCU sits on the board: an SOP16 chip marked `LKS 32MC035EL6S8B`, a
**Linko Semiconductor LKS32MC035** — a 32-bit motor-control MCU (Cortex-M0) with
a built-in 3-phase gate driver. Per `LKS32MC03x_3P3N_Datasheet_EN_v3.03.pdf`
(Tables 3-4/3-6), pins 5–10 are `HO1/LO1/HO2/LO2/HO3/LO3`, the gate-driver
outputs feeding external MOSFETs for a **3-phase BLDC motor**. Pin 3 is bonded
internally to P0.7 and P0.9 (alternate functions `UART0_TXD`/`UART0_RXD`), but
the protocol on it is **not UART** (§2.3).

The motor is commutated entirely by the LKS32's own firmware (not part of the
BK7238 image, not dumpable); the BK7238 only sends it high-level commands. The
BK7238's PWM drivers (`pwm_single_channel_setup` @ `0x4b204`,
`pwm_dual_channel_hbridge_setup` @ `0x4b274`) are real but drive the light
channels, not the fan.

### 1.2 Pin map

| BK7238 pin | Status | Source |
|---|---|---|
| **GPIO14** | **Confirmed**: the only data wire to the LKS32 (pin 3), via R26 | GPIO scan + continuity trace; no other signal trace to the LKS32 besides power/ground; literal `0xe` in the firmware TX init |
| **GPIO22** | `lowpower_onoff` output, high while the fan or light is on; board destination not traced | `.data` initial pin id `0x16` + identity pin map (§3) |
| **GPIO20** | Output held LOW by the stock pin init; no other use found; board tracing says it goes to the MOSFET bank | Literal pin 20 at `0xce170` (§3) |
| GPIO6 / GPIO7 | Light PWM channels (PWM0/PWM1) | Continuity trace + fixed BK7238 PWM pin map |
| GPIO16 | SPI pixel strip (MOSI), unverified | Earlier trace |

Notes:
- The module is a non-standard "CHG-Tuya" board (60 MHz crystal, no public pin
  reference); T1-3S-style pinout diagrams don't match it. Chip-level facts
  (which GPIO has which peripheral function) come from Beken's vendor driver
  source (`beken378/driver/gpio/gpio.c`, `gpio_pub.h`) and LibreTiny's BK7238
  docs, and hold regardless.
- GPIO14's only non-GPIO alternate function is SPI SCK; the hardware UARTs are
  fixed to other pin groups, so bit-banging is the only possible mechanism for
  this pin.
- GPIO20's only defined alternate function is `ADC3`, so nothing forces a role
  on it.
- The firmware's logical pin ids equal physical GPIO numbers (the `0x4002fc`
  table is the identity map for all 40 entries, in both builds).
- R26 is a plain 100 Ω series resistor (SMD marking `101`) between GPIO14 and
  LKS32 pin 3 — ESD/short protection, not a divider or filter.

### 1.3 Where other pin assignments live

Configurable peripherals (light PWM, pixel output, ...) get their pins from a
table **loaded from flash at runtime**: `FUN_00015ea4` → `FUN_00048fc4` reads
4096 bytes at flash `0x1F4000` and validates a magic `0x5459424C` ("TYBL" read
big-endian), two length fields and an additive byte-sum checksum, then
dispatches to a 16-byte-per-entry "simple GPIO/PWM output" table and a
4-byte-per-entry key table. That sector is in LibreTiny's "User Data" partition
(`0x1EC000–0x1F5000`), a manufacturing-provisioned area.

In the release image **that sector is erased (`0xFF`)** and the fallback for an
invalid sector just erases it, which matches the stock log line
`uf_open pd_cfg_kv err 8` (product config missing). The fan control pins are not
in this table: GPIO14, GPIO22 and GPIO20 are all compiled in (§2.3, §3).
Dumping a provisioned device's flash (UART download mode; LibreTiny's
`ltchiptool` guide) would populate the sector, if the light/pixel pins are ever
needed.

## 2. Command protocol

### 2.1 Frame content

The fan dispatcher (`0xce58c`) reads the fan's target value (0–100), looks it up
in a boundary table to pick the `cmd` byte, then builds the checksummed frame
via the packet builder `FUN_cbe7c`. Tables read directly from the firmware:

```
boundaries: [0, 10, 30, 50, 70, 85, 100]
commands:   [0x10, 0x12, 0x1c, 0xa, 0xf, 0xc]
```

i.e. `(0,10]→0x10` (level 1), `(10,30]→0x12` (2), `(30,50]→0x1c` (3),
`(50,70]→0xa` (4), `(70,85]→0xf` (5), `(85,100]→0xc` (6).

A frame is one bit-packed 32-bit word (the "22 22" prefix is not a separable
preamble — it is the top two bytes of the same word):

```
word  = (0x22222 << 12) | (seq << 4) | ((cmd & 0x1F) << 7)
byte0 = (word >> 20) & 0xFF   -> always 0x22
byte1 = (word >> 12) & 0xFF   -> always 0x22
byte2 = (word >> 8)  & 0xFF   =  0x20 | ((cmd >> 1) & 0x0F)
byte3 = (checksum_nibble(word) ^ 0xA) | (word & 0xFF)
```

`checksum_nibble` XORs all eight nibbles of `word`; `seq` is a rolling 3-bit
counter shared by all commands. `cmd`'s bottom bit isn't in byte2 — it flows into
`word` bit 7 and lands in byte3's checksum nibble.

### 2.2 Commands

| Command | `cmd` | Example frame (seq) |
|---|---|---|
| OFF | `0x16` | `22 22 2B 12` (1) |
| Speed 1 | `0x10` | `22 22 28 55` (5) |
| Speed 2 | `0x12` | `22 22 29 67` (6) |
| Speed 3 | `0x1C` | `22 22 2E 71` (7) |
| Speed 4 | `0x0A` | `22 22 25 0D` (0) |
| Speed 5 | `0x0F` | `22 22 27 96` (1) |
| Speed 6 | `0x0C` | `22 22 26 2C` (2) |
| `direction_reverse` | `0x11` | `22 22 28 BB` (3) |
| `direction_forward` | `0x04` | `22 22 22 4E` (4) |
| Wind ("natural wind" button) | `0x15` | `22 22 2A DF` (5) |

The direction commands are **absolute**, not a toggle: the direction handler
(`0xce6b8`) sends `0x04` for direction value 0 (forward) and `0x11` for any
nonzero value (reverse).

### 2.3 Physical layer

The waveform comes from the stock transmit code itself: a timer-driven state
machine at `0xcbd38`, fed by the packet builder (`0xcbe7c`, six copies of the
frame in a 24-byte buffer) and initialised by `0xcbf88`. Its encoding:

- **Not UART** — a pulse-width-per-bit scheme (closer to WS2812 timing).
- Each bit occupies one ~1000 µs slot, HIGH phase then LOW phase: bit `0` =
  250 µs HIGH / 750 µs LOW; bit `1` = 750 µs HIGH / 250 µs LOW.
- Bits are sent **MSB-first**, 32 bits per frame, MSB-first byte order, no
  start/stop bits.
- ~8.1 ms LOW gap between frames; each frame is sent **6 times** back-to-back.
- Each frame is also followed by a trailer pulse sequence (below).

The state machine's timing struct (RAM `0x400f72`) holds these initial values in
`.data`, identical in both firmware builds:

| field | µs | use |
|---|---|---|
| `+2` | 8100 | LOW gap between frames |
| `+4` / `+6` | 750 / 250 | bit `1`: HIGH / LOW |
| `+8` / `+0xa` | 250 / 750 | bit `0`: HIGH / LOW |
| `+0xc` / `+0xe` | 250 / 750 | **frame trailer**: HIGH / LOW |

`tools/txsim.py` runs that state machine in a small Thumb interpreter (no
emulator dependency): `python -I tools/txsim.py old 22222b12` (`old` or `new`
selects the firmware build; add `json` to dump the segments). For frame
`22 22 2B 12` it reproduces the 32 data bits exactly (`0x22222b12`), and the full
stock waveform is:

```
LOW 8100, then six times:
   32 bit pulses (H250/L750 for a 0, H750/L250 for a 1)
   trailer:  HIGH 250, LOW 750          (an extra "0"-shaped pulse)
   HIGH 100                             (short blip, hard-coded 100 µs)
   LOW 8100                             (gap; not after the sixth frame)
after the sixth frame: the trailer's H250/L750, then the line is left HIGH
```

That is 67 segments between gaps (a bare frame has 63) and a 408-edge, ~247 ms
burst. Also:

- **The line idles HIGH.** The TX init configures GPIO14 as an output with
  initial level HIGH (`{4,1,1}`), and a burst ends with the line HIGH.
- The first burst after boot waits **250 ms** (flag at `0x400f70`); later bursts
  wait 100 µs before starting.
- The LOW 8100 gap is therefore a falling edge from an idle-HIGH line, with a
  distinct HIGH blip just before it.

### 2.4 Observed stock behaviour

- **Verification:** all 42 distinct `cmd/seq → data` pairs in a long stock debug
  log match the formula in §2.1 byte for byte. The seq counter increments by
  exactly 1 per command, shared across all command types, with no resets
  (0→7→0…).
- **Boot:** the first frame after power-up is an unprompted **OFF (`0x16`) with
  `seq=1`**, using the ordinary protocol, before any button is pressed. It goes
  out ~544 ms after boot by the firmware's own log timestamps.
- **No handshake or init frame:** the transmit mechanism has exactly one caller
  chain in the firmware, and no differently formatted frame appears in the log.
- **Turn-on + direction in one update:** the speed command goes first, then the
  direction command.
- **Wind** (DP61): value 1 sends `0x15`; value 0 sends no special command, it
  re-sends the current speed command. What wind does on the fan (pattern,
  timing) is not characterised; `fun_fan_mode` itself just forces the fan off and
  reschedules tasks, and the actual mode lives in a deferred callback that wasn't
  decompiled.
- **Speed mapping** in the log matches §2.1 (e.g. 10→`0x10`, 15/17/21→`0x12`,
  38–47→`0x1C`, 51/54→`0x0A`).

## 3. The `lowpower_onoff` GPIO (GPIO22) and GPIO20

The stock log prints `lowpower_onoff: N` alongside `rgbcw_onoff` and `fan_onoff`.
It is a **real GPIO output**: the light and fan on/off functions (`0xcd3f4`,
`0xcd44c`) compute `lowpower = fan_on OR light_on` in a RAM struct at
`0x401214`, and on change call `0x4b108(u16 [0x401214], flag)` — a GPIO write
that maps a logical pin id (valid ≤ 39) through the table at `0x4002fc` to a
physical GPIO and drives it high/low.

**The pin is GPIO22.** The struct lives in `.data`; its initial bytes (read from
the startup copy source) are `16 00 | 00 | 00 00 00 00 01`, i.e. pin id `0x0016`
= **22**, with the fan/light/lowpower state bytes all 0. The `0x4002fc` table is
also in `.data` and is the identity map, so logical 22 = GPIO22. Both builds
agree.

Timing seen in the stock log:

- **Turn-on:** the pin goes high ~4–14 ms **before** the first speed command.
- **Turn-off:** the OFF command goes out first; `fan_onoff: 0` /
  `lowpower_onoff: 0` follow ~**100 ms** later (all 8 offs in the log), matching
  the 100 ms deferred task that the stock on/off actuator schedules.
- **Boot:** high at ~584 ms, only because the light defaults to on (so the first
  OFF at ~544 ms goes out with the pin still low).

**GPIO20.** The init routine at `0xce160` calls the GPIO init helper (`0x4b0b0`)
twice with the same config `{4, 1, 0}` (push-pull output, initial level LOW):
first for the lowpower pin (GPIO22), then for hard-coded `0x14` (**20**). Pin 20
is therefore also an output driven LOW at boot; no other write to it was found
(writes through a variable pin id can't be excluded). Board tracing says GPIO20
goes to the MOSFET bank; what the stock firmware does with it is unknown.

Where GPIO22 goes on the board has not been traced.

## 4. The ESPHome component

`components/smavista_pulse_cmd_fan/` + `fan-pulse-cmd-config.yaml`. The shipped
configuration — the full stock waveform (`stock_waveform: true`), GPIO14 as the
data pin, `enable_pin: GPIO22` and `aux_pin: GPIO20` — is the one confirmed
working on the fan.

### Behaviour

- **Waveform:** exactly §2.3. The component's send sequence was checked
  segment-for-segment against the emulated stock waveform for several frames
  (407 segments each, identical). `stock_waveform: false` restores the earlier
  bare-frame, idle-LOW model for comparison.
- **`enable_pin` (GPIO22):** follows the stock sequencing by default
  (`enable_follow_fan`): initialised disabled, raised `enable_lead_ms` (10)
  before the first speed/wind command, dropped `enable_off_delay_ms` (100) after
  OFF; a quick off→on cancels the pending drop. `enable_follow_fan: false` holds
  it enabled from boot. `enable_inverted` selects polarity.
- **`aux_pin` (GPIO20):** an output held LOW from boot, as the stock pin init
  does.
- **Sequence counter** starts at 1, matching stock's boot-time OFF.
- **Command order:** speed (or OFF) before direction on a combined update.
- **Wind:** `wind` sends `0x15`; `wind_off` re-sends the current speed, since
  stock has no wind-off command.
- **Actions** (exposed in the example YAML's `api.actions`):
  `direction_forward`, `direction_reverse`, `wind`, `wind_off`. The component
  also provides a `seq_sweep` diagnostic action, which the example omits.

### What isn't isolated

The updates that made the fan respond — stock waveform, the GPIO22 enable and
GPIO20 held LOW — went in together, and earlier tests used the old waveform and
the wrong enable pin (GPIO20), so it is not known which of them the LKS32 or
the output stage strictly needs. The component can be bisected on the fan: set
`stock_waveform: false`, remove `enable_pin`, or remove `aux_pin`, one at a time.
Two caveats for that test: GPIO22's board destination is untraced, and an
earlier run of GPIO20 driven as an enable (in either polarity, held static) did
nothing, but that was with the old waveform.

### Differences from stock that remain

- The boot-time OFF is sent 2 s after boot, not ~0.5 s (see below).
- The first burst after boot doesn't wait 250 ms (the 2 s delay already covers
  it).
- Stock's `lowpower` pin is `fan OR light`; there is no light here, so the pin
  tracks the fan only.

### Implementation notes

- **Do not wrap bit transmission in interrupt-disable.** Disabling interrupts for
  a whole frame (~32 ms) breaks `micros()` (LibreTiny's BK72xx `micros()` goes
  stale if interrupts are off for more than ~2 ms), and wrapping each bit
  separately (192 critical sections per 6x send) created multi-millisecond
  stalls that fragmented frames and crashed the device. The plain busy-wait is
  reliable.
- **Do not transmit from `setup()`.** Calling `send_command_()` inline there
  reliably crashed and rebooted the device (WiFi/RTOS not yet stable); the boot
  OFF is deferred via `set_timeout(2000, ...)` and chained with the
  restore-state apply.

## 5. Dead ends and lessons

Things that were investigated and are **not** the answer, kept so they aren't
re-tried:

- **PWM H-bridge on BK7238 pins.** The dual-channel complementary PWM driver
  (`pwm_dual_channel_hbridge_setup`) is real but drives the light; the fan is
  commutated by the LKS32.
- **UART.** The link is not UART — no baud rate, no start/stop bits; GPIO14's
  only alternate function is SPI SCK. Searching for baud-rate literals and UART
  register use found nothing, for that reason.
- **A separate init/handshake frame.** None exists; the very first frame after
  boot is an ordinary OFF.
- **Power or a missing signal wire.** The LKS32's rails were fine (AVDD ≈ 4.7 V,
  VCC ≈ 22 V; its internal LDO needs no enable pin), and GPIO14→pin 3 is the
  only signal trace between the BK7238 module and the LKS32.
- **Sequence-value gating.** The stock log shows a free-running counter and no
  resync mechanism.

Lessons from how the answer was found:

- Correct frame bytes and checksums don't prove the waveform is correct: the
  idle level and the per-frame trailer were missed until the *complete* edge
  sequence was compared against what the firmware itself generates. Nothing in
  the data bits or checksums had suggested a gap.
- Call-graph tracing through generic RTOS plumbing failed repeatedly. Scanning
  for **call sites of the GPIO write helper** found the real transmit routine,
  and reading `.data` initial values (startup copy source) gave the pin ids and
  timing constants directly — no execution needed.
- An early lead dismissed a literal-`0xe` call as a log helper; it was the real
  GPIO14 TX init. Treat "retracted" static-analysis leads with suspicion once
  better tooling exists.
- Static pin guesses from a mismatched pinout diagram were wrong; the firmware's
  own `.data` and the board continuity trace were reliable.
