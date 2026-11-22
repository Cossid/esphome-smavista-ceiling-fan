# SMAVISTA Ceiling Fan — ESPHome

Local control of the SMAVISTA ceiling fan (a Tuya-based BK7238 Wi-Fi module
driving a Linko LKS32MC035 motor controller) by replacing its firmware with
ESPHome. A custom component, `smavista_pulse_cmd_fan`, reproduces the stock
firmware's command waveform, so speed, direction and on/off work as a normal Home
Assistant fan. The light's cold/warm-white and RGB outputs are included.

Confirmed working on the my device. The pin assignments below are for my
board; other hardware revisions may differ.  Likely works with the CINOTON
branded ceiling fans as well.

## Features

- **Fan entity:** on/off and 6 speeds.
- **Direction:** forward / reverse, via the fan's `direction` control.
- **Wind:** the remote's "natural wind" command and a way to leave it, as
  actions.
- **Light:** a tunable-white (CWWW) light and an addressable RGB strip with
  effects.

## Quick start

1. Install [ESPHome](https://esphome.io).
2. Create `secrets.yaml` next to the config:

   ```yaml
   wifi_ssid: "your-network"
   wifi_password: "your-password"
   ```

3. Review the pins and strip settings in the example config below.
4. Build and flash:

   ```sh
   esphome run esphome-smavista-ceiling-fan-config.yaml
   ```

   The first flash can be done over a serial connection to the module (see the
   [LibreTiny](https://docs.libretiny.eu) docs for the BK7238), or over the air
   from the stock firmware (next section). Later updates go over the air through
   ESPHome.

## Moving from stock firmware over the air (UnTuyaOS3)

[UnTuyaOS3](https://github.com/tuya-cloudcutter/UnTuyaOS3) can be used to OTA from
the stock firmware to custom ESPHome firmware, with no soldering. It uploads a
firmware image to a TuyaOS 3 device while the device is in its Wi-Fi access-point
(pairing) mode. According to its README:

- It runs on a **Linux** host (tested on Raspberry Pi OS / Debian) with a Wi-Fi
  adapter and `iw`, and needs root. It is run as `sudo ./UnTuyaOS3.sh`.
- The device is put into AP mode first (typically a slow blink). No Tuya account
  or cloud access is involved; the upload happens locally over the device's AP.
- It takes firmware images in Tuya "UG" format, placed under
  `custom-firmware/<PLATFORM>/`. Its bundled default is an ESPHome Kickstart
  image from LibreTiny.
- It works on *some* TuyaOS 3 devices. If the exploit fails the device is left
  unchanged, but flashing is at your own risk and a bad flash can brick the
  device, in which case serial flashing is the only recovery.

The ESPHome build for this config produces a UG-format image,
`image_bk7238_app.ota.ug.bin`, in
`.esphome/build/smavista-ceiling-fan/.pioenvs/smavista-ceiling-fan/`.

This fan's module is a BK7238, which is UnTuyaOS3's **T1** platform, so choose
T1 when it asks for the platform. Follow its README for the current steps.

## Example config

[`esphome-smavista-ceiling-fan-config.yaml`](esphome-smavista-ceiling-fan-config.yaml) is the working
configuration:

```yaml
esphome:
  name: smavista-ceiling-fan
  friendly_name: "Smavista Ceiling Fan"

bk72xx:
  board: generic-bk7238-tuya

logger:
ota:
  - platform: esphome
  - platform: web_server
web_server:
captive_portal:

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password
  ap:

external_components:
  - source: github://cossid/esphome-smavista-ceiling-fan@main
    components: [ smavista_pulse_cmd_fan ]
    refresh: 10 min

# Ceiling fan control. The BK7238 talks to the fan's motor controller
# (LKS32MC035) over one GPIO using the stock firmware's pulse-width waveform;
# see ANALYSIS.md section 2. Speed (1-6), on/off and direction are exposed as a
# normal fan entity.
fan:
  - platform: smavista_pulse_cmd_fan
    id: ceiling_fan
    name: "Ceiling Fan"
    # Command line to the motor controller.
    pin: GPIO14
    # The stock firmware's `lowpower_onoff` output: high while the fan is on,
    # raised ~10ms before the first command and dropped ~100ms after OFF
    # (ANALYSIS.md section 3).
    enable_pin: GPIO22
    # The stock pin init also holds GPIO20 LOW as an output (purpose unknown).
    aux_pin: GPIO20

    # Optional tuning. Everything below is already the default, shown for
    # reference.
    # stock_waveform: true        # false = bare frames, line idles LOW
    # trailer_blip_us: 100        # short HIGH blip after each frame's trailer
    # short_pulse_us: 250
    # long_pulse_us: 750
    # frame_gap_us: 8100
    # repeat: 6                   # copies of each frame per command
    # inverted: false             # only for an inverting stage on the line
    # enable_inverted: false
    # enable_follow_fan: true     # false = hold the enable pin on from boot
    # enable_lead_ms: 10
    # enable_off_delay_ms: 100

# Light: a tunable-white (cold/warm) PWM pair plus an addressable RGB strip.
output:
  - platform: libretiny_pwm
    id: output_warm_white
    pin: GPIO6
  - platform: libretiny_pwm
    id: output_cold_white
    pin: GPIO7

light:
  # Addressable RGB strip, driven over SPI (MOSI on GPIO16).
  - platform: beken_spi_led_strip
    channel_colors: GRB
    pin: GPIO16
    num_leds: 36
    chipset: SK6812
    name: RGB
    effects:
      - addressable_rainbow:
      - addressable_color_wipe:
      - addressable_scan:
      - addressable_twinkle:
      - addressable_random_twinkle:
      - addressable_fireworks:
      - addressable_flicker:
  # Tunable-white light on the two PWM outputs above.
  - platform: cwww
    name: CWWW
    cold_white: output_cold_white
    warm_white: output_warm_white
    cold_white_color_temperature: 6500 K
    warm_white_color_temperature: 2700 K

# Manual actions for Home Assistant or automations.
api:
  actions:
    # Absolute direction commands, the same ones the fan's own `direction`
    # control uses.
    - action: fan_direction_forward
      then:
        - smavista_pulse_cmd_fan.direction_forward: ceiling_fan
    - action: fan_direction_reverse
      then:
        - smavista_pulse_cmd_fan.direction_reverse: ceiling_fan
    # The remote's "wind" (natural wind) button.
    - action: fan_wind
      then:
        - smavista_pulse_cmd_fan.wind: ceiling_fan
    # Leave wind mode. Stock has no separate command: it re-sends the current
    # speed.
    - action: fan_wind_off
      then:
        - smavista_pulse_cmd_fan.wind_off: ceiling_fan
```

If the config file and this copy ever differ, the file is authoritative.

## Pins

| Pin | Use |
|---|---|
| GPIO14 | Command line to the motor controller |
| GPIO22 | Enable output, sequenced around fan on/off (the stock `lowpower_onoff`) |
| GPIO20 | Held LOW as an output, as the stock firmware does |
| GPIO6 / GPIO7 | Warm / cold white PWM |
| GPIO16 | RGB strip data (SPI MOSI) |

If warm and cold come out swapped, swap the two PWM pins. If the strip's colours
are wrong, adjust `channel_colors`.

## Component options

All `smavista_pulse_cmd_fan` options besides `pin` are optional. The defaults
reproduce the stock firmware, so normally only `pin`, `enable_pin` and `aux_pin`
are set. The full list, with defaults, is in the commented block of the example
config. Two worth knowing:

- `stock_waveform: false` sends bare frames with the line idling LOW, the earlier
  model, which is useful only for comparison.
- `enable_follow_fan: false` holds the enable pin on from boot instead of
  following the fan.

## How it works

The fan's motor is driven by a separate controller, which the BK7238 commands over
a single GPIO with a pulse-width protocol (not UART). The component builds each
32-bit frame (command, rolling sequence number, checksum) and sends it exactly as
the stock firmware does, including a trailer after each frame, an idle-high line,
and the extra enable pin.

The analysis behind this — frame format, command table, the full waveform, the pin
map and the reasoning — is in **[ANALYSIS.md](ANALYSIS.md)**.

## Repository layout

| Path | Contents |
|---|---|
| `fan-pulse-cmd-config.yaml` | Example / working ESPHome config |
| `components/smavista_pulse_cmd_fan/` | The custom fan component |
| `ANALYSIS.md` | Firmware analysis and protocol write-up |
| `tools/txsim.py` | Emulates the stock transmit routine to print its waveform (needs the stock v1.0.2 firmware image, which is not included) |
| `LKS32MC03x_3P3N_Datasheet_EN_v3.03.pdf` | Datasheet for the motor controller |
