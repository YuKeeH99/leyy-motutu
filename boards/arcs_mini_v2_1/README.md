# ARCS Mini v2.1

`arcs_mini_v2_1` is an application-local board definition for the LS2684
QFN76 headless design. It keeps the default Arcs Mini audio enable, power,
USB/ADB, battery and UART0 wiring while removing the LCD/camera and replacing
the expansion-pin assignment.

## Expansion mapping

| Signal | Pad | Function |
| --- | --- | --- |
| Servo 0 | PA04 | PWM0 channel 4, function 12 |
| Servo 1 | PA05 | PWM0 channel 5, function 12 |
| Servo 2 | PA06 | PWM0 channel 6, function 12 |
| Servo 3 | PA07 | PWM0 channel 7, function 12 |
| WS2812 strip | PA08 | GPIOA function 1, uniform GRB output for 1–64 daisy-chained pixels |
| Touch sensor 1 (U10/U12) | PA09 | GPIOA input, function 1 |
| Touch sensor 2 (U7/U9) | PA13 | GPIOA input, function 0 |
| MPU6050 + PN532 I2C0 SDA | PB06 | Shared I2C0 SDA, function 8 |
| MPU6050 + PN532 I2C0 SCL | PB07 | Shared I2C0 SCL, function 8 |
| MPU interrupt | PA12 | GPIOA input, function 0 |

The MPU6050 (`0x68`) and hard-strapped I2C-mode PN532 (`0x24`) share I2C0.
The LISA I2C controller lock serializes their transfers. A dual-controller
device test on 2026-08-18 found both addresses on PB06/PB07 and no devices on
I2C1/PA10/PA11.

PA08 boots in GPIO mode so the timing-based WS2812 driver can own it without the
PWM driver stealing the pad. The actuator service transmits one continuous
`24 × pixel_count` GRB frame for a uniform 1–64-pixel fill; brightness is scaled
before transmission. Manual/action ownership prevents NFC and provisioning
status colours from overwriting an active motion timeline until the host
releases the strip. A service that deliberately uses PWM channel 0 can call
`board_ws2812_pinmux_pwm()` and later restore GPIO mode with
`board_ws2812_pinmux_gpio()`.

PA04–PA07 use a 50 Hz carrier only while enabled. Boot leaves the outputs off;
voice standby uses ordinary PWM disable and GPIO-low, without a pad-force or
persistent sleep output-inhibit latch.
Motion speed is implemented by a 20 ms
four-channel interpolation worker and one combined GPT shadow load, not by
changing the PWM frequency. The host control lease can be set to 200–5000 ms;
lease expiry remaps all four outputs to GPIO-low. Ordinary PWM servos provide
no position feedback, so status values are commanded positions only.

DVP/camera, SDIO/SDMMC, the PA04/PA05 robot UART, and the PA08/PA09 legacy modem
UART remap are disabled by `overlay.conf`. PA13 remains GPIO for the second
TTP223 instead of the default-board DVP data assignment.

## AEC reference population

The firmware supports two mutually exclusive V2.1 audio populations. The
current `overlay.conf` default is **hardware analog feedback**:

- schematic `MIC1` -> SoC `MIC0_P/N` -> interleaved ADC left channel: the real
  microphone;
- speaker feedback -> `R12/R13` -> SoC `MIC1_P/N` -> ADC right channel: the
  hardware AEC reference;
- the ADC right channel is not a second microphone, so raw MIC0/MIC1 TDOA must
  be disabled;
- `LISA_AUDIO_PLAY_ECHO_ENABLE` is DAC echo DMA rather than analog feedback and
  must remain disabled.

The alternative software-reference population uses two real microphones:

- schematic `MIC1` -> SoC `MIC0_P/N` -> interleaved raw channel `MIC0`;
- schematic `MIC2` -> `R35/R36` -> SoC `MIC1_P/N` -> raw channel `MIC1`;
- `R12/R13` must be DNP, otherwise the speaker hard-AEC network is electrically
  paralleled with the second microphone;
- `R35/R36` must remain populated as 0-ohm links.

In software mode the board records both channels at 16 kHz/16-bit with matched
default 30 dB analog gain, copies completed DAC playback PCM through
`LISA_AUDIO_PLAY_SOFT_ECHO` for the ACOMP AEC input, and may run raw MIC0/MIC1
TDOA in parallel. The measured acoustic-port spacing is configured as about
20 mm (`CONFIG_AUDIO_TDOA_MIC_SPACING_TENTH_MM=200`). This produces an
array-axis estimate only; the front/left physical channel mapping must be
confirmed during fixture calibration.

The exact mode switches, build commands, validation rules, and serial logs are
documented in `doc/AEC方案.md`.

Current fixture amplitude cross-check maps the physical ports as follows:

- physical left microphone -> raw `MIC0`;
- physical front microphone -> raw `MIC1`.

The physical MIC1 capsule uses an approximately 10 cm analog lead. The lead
length is not part of the acoustic spacing, but its added pickup and channel
response mismatch must be included in correlation and enclosure calibration.

The current board-specific gates were measured at matched 30 dB analog gain:

- combined mean absolute amplitude: 150;
- peak amplitude: 750;
- absolute normalized correlation: 0.350;
- aggregation: at least 2 accepted 16 ms blocks in each 63-block window;
- calibrated ILD fusion: `abs((MIC0-MIC1)/(MIC0+MIC1)) >= 0.060`.

The observed quiet-fixture correlation did not exceed about 0.251, while
front and left excitation both reached about 0.61. The estimator accepts
either fixed channel polarity and still reports the signed arrival delay.
Six post-flash quiet windows produced zero accepted blocks. Live speech then
produced valid FRONT delays of -0.319 and -0.437 sample and a valid LEFT delay
of +0.801 sample. One reflected front window reported +0.334 sample, so the
published delay now uses a three-valid-window median filter. With two opposing
entries it reports their mean (normally CENTER) until a majority exists; two
consecutive inactive report windows clear the filter before a new sound.
The published side has an additional event-state hysteresis: one contrary
valid report cannot flip LEFT/FRONT, two consecutive contrary reports can;
short CENTER candidates retain the confirmed side, and three consecutive
CENTER candidates confirm CENTER. The same two inactive windows reset both
filters. Logs keep `candidate`, filtered `delay_milli`, and `raw_delay_milli`
separate so the stabilization remains auditable.

Final enclosure tests also showed that a physical LEFT source consistently
made MIC0 stronger (for example 446/299, 461/386 and 415/324), while a FRONT
source consistently made MIC1 stronger. V2.1 therefore fuses this calibrated
inter-channel level difference (ILD) with the time-delay candidate. Strong ILD
or at least eight coherent blocks permits immediate direction confirmation;
otherwise a new/opposite side still needs two reports. The generic default is
zero (ILD disabled), because this threshold is specific to the Mini microphone
placement, gains, enclosure and approximately 10 cm MIC1 analog lead.

Runtime logs emit one bounded `tdoa:` result per approximately one second of
ungated audio. The latest thread-safe result is also available with:

```text
tdoa status
```

Playback suppresses raw TDOA temporarily (`echo_gated=1`) so the robot's own
speaker is not reported as an external source.

## Build

```bash
./build.sh -S apps/arcs-mini -B build-v2_1 -DBOARD=arcs_mini_v2_1
```
