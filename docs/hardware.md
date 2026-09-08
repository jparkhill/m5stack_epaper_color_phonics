# M5Stack PaperColor (SKU C151) — hardware notes

Everything here was read out of M5GFX's `board_M5PaperColor` autodetect block
and M5Unified's pin tables, or found by running the thing. It is written down
because none of it is in a datasheet you can search.

## Silicon

| | |
|---|---|
| MCU | ESP32-S3R8, dual Xtensa LX7 |
| PSRAM | 8 MB **octal** (OPI) — mandatory, see below |
| Flash | 16 MB, quad, 3.3 V (per eFuse) |
| Panel | `Panel_ED2208`, 400×600, E Ink Spectra 6 |

**PSRAM is not optional.** `Panel_ED2208` allocates a 400×600×3 = 703 KB
RGB888 framebuffer in PSRAM and `init()` fails without it. M5GFX logs
`M5PaperColor need OPI-PSRAM enabled` and refuses the board. The required
config is exactly:

```
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_CLK_IO=30
CONFIG_SPIRAM_CS_IO=26
```

## Pin map

### SPI2 — shared between the panel and the microSD card

`bus_shared = true`. Never drive both at once.

| Signal | GPIO |
|---|---|
| MOSI | 13 |
| MISO | 14 |
| SCLK | 15 |
| EPD CS | 44 |
| EPD DC | 43 |
| EPD BUSY | 11 |
| EPD RST | 12 |
| SD CS | 47 |

The card is on **SPI mode**, not the 4-bit SDMMC bus, and the driver
negotiates down through 20 MHz → 10 MHz → 4 MHz. Expect ~1–2 MB/s.

### Internal I2C

| Signal | GPIO |
|---|---|
| SDA | 3 |
| SCL | 2 |

| Device | Address |
|---|---|
| RX8130 RTC | `0x32` |
| SHT40 temp/humidity | `0x44` |
| M5PM1 PMIC | `0x6E` |

Board autodetect keys off the RTC + PMIC responding, so a dead I2C bus
presents as "wrong board detected" rather than "I2C failed".

GPIO **5/4** are the *external* (Grove) I2C pins. The M5 reference firmware
repurposes them as a console UART; this project does not.

### I2S speaker

| Signal | GPIO |
|---|---|
| MCLK | 42 |
| BCLK | 40 |
| WS | 41 |
| DOUT | 38 |

`I2S_NUM_0`, 44100 Hz, stereo, `magnification = 1`. A real I2S DAC — not a
buzzer like the PaperS3. There is also a microphone on `I2S_NUM_1`
(MCLK 42 / BCLK 40 / WS 41 / DIN 39), unused here.

### Buttons and LEDs

Three buttons, all **active low**, read as plain GPIOs:

| M5Unified | GPIO |
|---|---|
| `BtnA` | 9 |
| `BtnB` | 10 |
| `BtnC` | 1 |

Physical top/middle/bottom order is **not documented anywhere**. Use the `btn`
console command.

Two RGB LEDs on **GPIO21**, driven over RMT.

### M5PM1 PMIC GPIOs

These are PMIC pins, not ESP32 pins.

| PMIC GPIO | Function |
|---|---|
| 0 | e-paper rail enable (M5GFX raises it) |
| 1 | microSD card detect, **active low** |
| 2 | RTC wake |
| 3 | microSD rail enable (M5GFX raises it) |
| 4 | card-detect enable — **must be driven high** |

Register map (all bitmasks over `[4:0]`):

| Reg | Purpose |
|---|---|
| `0x10` | direction, 1 = output |
| `0x11` | output level |
| `0x12` | input level (read-only) |
| `0x13` | drive, 1 = open-drain |
| `0x14` | pull-up/down for GPIO0–3, 2 bits each, `01` = pull-up |
| `0x16` | function select GPIO0–3, 2 bits each, `00` = plain GPIO |
| `0x17` | function select GPIO4, `00` = plain GPIO |

## Powering off

Write `0xA1` to PMIC register `0x0C` — `[7:4]` is a key that must be `0xA`,
`[1:0]` is the command (`01` = shutdown, `10` = reboot, `11` = download mode).
The PMIC wants a settle window of ~120 ms before it will accept the write.

This cuts every rail; the hardware power button is the only way back. Put the
panel into its own sleep state (`M5.Display.sleep()`) first rather than
dropping power mid-scan.

**Do not let an idle timeout fire while a USB host is attached.** The rails
drop, USB-Serial-JTAG disappears mid-session, and the board looks bricked —
`/dev/ttyACM*` simply vanishes and a flash fails with
`could not open port`. Gate it on `usb_serial_jtag_is_connected()`.

The panel is bistable, so the last displayed image persists indefinitely with
no power at all.

## Text-to-speech traps (host side, but they bite hard)

Not hardware, but the same class of problem: **espeak fails silently.** It
never errors — it just says something confidently wrong, and you only find out
by listening to 260 clips. Verify with piper's phonemizer instead:

```python
from piper import PiperVoice
v = PiperVoice.load(".../en_US-libritts-high.onnx")
"".join("".join(x) for x in v.phonemize("fff"))   # -> 'ɛfɛfɛf'
```

Found this way:

| Written | espeak says | Should be |
|---|---|---|
| `ay` | /ˈaɪ/ "eye" | letter A is /eɪ/ — use `eigh` |
| `eff` | /ɛf ɛf ɛf/ | letter F is /ɛf/ — use `ef` |
| `fff` | /ɛf ɛf ɛf/ | the /f/ sound, not the name three times |
| `eh` | /eɪ/ | short-e /ɛ/ |
| `ih` | /aɪ/ | short-i /ɪ/ |
| `ks` | /keɪ ɛs/ | /ks/ |

A bare vowel letter is *always* read as that letter's name, so an isolated
short vowel is unreachable through text. espeak's `[[...]]` markup passes
phonemes through verbatim and is the only reliable route:

```
"eigh makes the [[æ]] sound."  ->  ˈeɪ mˈeɪks ðə æ sˈaʊnd.
```

Isolated plosives are a separate problem: /b/ with no following vowel is
essentially inaudible, so stops use a `-uh` syllable (`[[bʌ]]`, `[[kʌ]]`),
matching what phonics programmes teach anyway.

## Traps

**Card detect is gated.** PMIC GPIO4 must be high before GPIO1 carries a
meaningful level. Miss it and the card always reads absent — while mounting
the card directly still works, which makes it a confusing bug.

**Do not use the `m5stack/m5pm1` component's `begin(m5::I2C_Class*)`.** It is
guarded by `__has_include(<utility/I2C_Class.hpp>)`, which is false while that
component compiles, so the symbol is declared but never emitted and you get an
undefined reference at link time. Its other `begin()` overloads install a
second I2C driver on GPIO2/3 and fight M5Unified for the bus.
`main/hal/hal_power.cpp` writes the registers directly instead.

**M5Unified's `RTC_Class` does not speak RX8130.** It is BM8563/PCF8563 only.
Set `cfg.internal_rtc = false` and drive the RTC yourself: BCD registers
`0x10`–`0x16` (sec, min, hour, week, day, month, year), flags at `0x1E` with
VLF in bit 1. The week register is a **one-hot bitmask**, not a number.

**TinyUSB and the serial console are mutually exclusive.** One internal USB
PHY, routed to either USB-Serial-JTAG or USB-OTG. Enable TinyUSB (for USB mass
storage) and `/dev/ttyACM*` disappears the moment the app starts, so
`idf.py monitor` stops working and reflashing needs the BOOT button. This
project keeps USB-Serial-JTAG.

**Opening the serial port can reboot the chip into download mode.** On
USB-Serial-JTAG, DTR/RTS are wired to EN/GPIO0. A host tool that asserts them
(esptool's `--before default_reset`, and `idf.py monitor`'s reset-on-open) can
leave the chip sitting at `boot:0x23 (DOWNLOAD(USB/UART0))` printing "waiting
for download". If a freshly-flashed board looks dead, check the boot mode in
the ROM banner before suspecting the firmware. `tools/set_time.py` sets
`dsrdtr=False` / `rtscts=False` for this reason.

**LovyanGFX auto-refreshes the panel after EVERY drawing call.** This one is
worth reading twice. `Panel_FrameBufferBase::init()` contains:

```cpp
#if defined ( LGFX_USE_CACHE_WRITEBACK_ADDR )
    // auto_display is used to automate the cache write-back in display()
    _auto_display = true;
#endif
```

That macro is always defined on an ESP32-S3 with a PSRAM framebuffer, so
`endWrite()` calls `display()` — a full physical Spectra 6 refresh — after
every `fillRect`, every `drawString`, everything. Measured cost was a constant
**16.73 s per drawing call**, whether it touched 1,200 pixels or 240,000:

```
PSRAM memset 703 KB      :     38.8 ms (17.7 MB/s)   <- memory is fine
fillScreen (no guard)    :  16775.7 ms
fillRect 100x100         :  16728.9 ms   <- same cost as a full screen
drawString 9 chars       :  16729.1 ms   <- same cost again
```

A constant per-call cost that ignores pixel count is the tell. The symptom on
the bench is a screen that appears to refresh in an endless cycle and never
finishes composing a frame. The fix is one line after `M5.begin()`:

```cpp
M5.Display.setAutoDisplay(false);
```

then compose the whole frame and call `display()` exactly once. Safe here
because `Panel_ED2208::_exec_transfer()` reads the framebuffer with the CPU,
not DMA, so the cache write-back `_auto_display` exists to automate is not
needed. After the fix a full frame composes in **157 ms**.

**Optimisation level is load-bearing, in both directions.**

* Everything must be built at `-O2`. LovyanGFX's framebuffer writes are
  per-pixel templates that collapse without inlining; at `-Og` a single
  `fillScreen()` took ~17 s of genuine CPU work.
* M5Unified must be built at `-Og`. See the next entry.

The root `CMakeLists.txt` applies `-Og` to `__idf_m5stack__m5unified` only.

**M5Unified's speaker task cannot be given enough stack.** `Speaker_Class`
creates `spk_task` with `1280 + dma_buf_len * 4` bytes of stack, and then
inside that task does:

```cpp
int32_t* sound_buf32 = (int32_t*)alloca(dma_buf_len * sizeof(int32_t));
```

The `alloca` grows exactly as fast as the stack, so headroom for the rest of
the frame is a fixed ~1280 bytes at any `dma_buf_len` — tuning it cannot help.
At `-O2` the frame exceeds that and the board panics on boot with
`***ERROR*** A stack overflow in task spk_task has been detected`. There is no
exposed stack-size knob.

This project therefore sets `cfg.internal_spk = false` and drives the codec
and I2S itself (`main/hal/hal_audio.cpp`), with synchronous writes from the
caller's task. That removes the task, the alloca and the whole failure mode.

## Audio path

The speaker is behind an **ES8311 codec on the internal I2C bus at 0x18** —
this is the "unknown" device that shows up in the boot I2C scan — plus two
enable GPIOs:

| Signal | GPIO |
|---|---|
| codec enable | 45 |
| speaker/PA enable | 46 |

Bring-up: drive both high, wait ~20 ms, then write these registers (sequence
taken from M5Unified's `_speaker_enabled_cb_papercolor`):

| Reg | Value | Meaning |
|---|---|---|
| `0x00` | `0x80` | RESET / CSM power on |
| `0x01` | `0xB5` | clock manager: derive codec clock from BCLK |
| `0x02` | `0x18` | clock manager: `MULT_PRE = 3` |
| `0x0D` | `0x01` | power up analog |
| `0x12` | `0x00` | power up DAC |
| `0x13` | `0x10` | enable output to headphone driver |
| `0x32` | `0xCF` | DAC volume (+16 dB) |
| `0x37` | `0x08` | bypass DAC equaliser |

**The codec must be driven at one fixed sample rate.** Reg `0x01 = 0xB5` makes
the ES8311 derive its internal clock from BCLK, and `0x02 = 0x18`
(`MULT_PRE = 3`) is tuned for that. Reconfiguring the I2S clock per clip moves
the codec's PLL, and at 22.05 kHz it produces **no audible output at all** —
while `i2s_channel_write()` keeps returning `ESP_OK`, so from the firmware's
side everything looks perfect. A silent speaker and a working one are
indistinguishable without listening, which is what the `audio` console
command exists for (it reads the codec registers back and plays a loud tone).

M5Unified drives this board at a fixed 44100 stereo, so this project does the
same and resamples clips in software instead (linear interpolation; the
22.05 kHz narration is an exact 2× ratio).

**Do not enable/disable the I2S channel per clip.** Each start/stop produces
an audible transient at the head of the clip. Bring the channel up once at
init and leave it up — with `auto_clear = true` an idle channel emits zeros,
so it is silent. A short silence pre-roll before each clip covers the
remaining settling time.

## Panel behaviour

The driver keeps an RGB888 framebuffer in PSRAM and quantises to 4 bpp at
transmit time. Only six of the sixteen codes are real inks:

| Ink | Code | RGB |
|---|---|---|
| black | `0x0` | `0, 0, 0` |
| white | `0x1` | `255, 255, 255` |
| yellow | `0x2` | `255, 243, 56` |
| red | `0x3` | `191, 0, 0` |
| blue | `0x5` | `100, 64, 255` |
| green | `0x6` | `67, 138, 28` |

`0x4` and `0x7` are unused. **There is no grey**, so grey subjects dither to
blue-and-white.

**`setEpdMode()` is not a speed control.** It is the obvious place to look for
a "less aggressive, faster" refresh, and it does nothing of the sort:
`_epd_mode` is read in exactly one place, `_exec_transfer()`, to pick a dither
algorithm. `_turn_on_display()` — POWER_ON, boost setup, DISPLAY_REFRESH,
POWER_OFF, each with a busy-wait — is identical in every mode. Measured
refresh time is ~16.1 s regardless of mode.

There is no partial-update or fast-waveform path exposed by this driver, so
~16 s per update is a hard floor. The only lever is to stop waiting idly
through it: compose in PSRAM, start audio on the other core, then refresh.

`setEpdMode()` selects the dither used at transmit time:

| Mode | Dither |
|---|---|
| `epd_fastest` | none — straight nearest-colour |
| `epd_fast` | Bayer, paired |
| `epd_text` | RGB pair |
| `epd_quality` | RGB pair, stronger |

Card images are pre-dithered on the host to the exact six RGB values above and
presented with `epd_fastest`, so the on-device lookup is an identity mapping.
Using a dithering mode on already-dithered art produces visible mush.

**There is no partial refresh.** `display(x, y, w, h)` accumulates a dirty
rectangle but `_exec_transfer()` sends every row regardless, then runs
POWER_ON → DISPLAY_REFRESH → POWER_OFF with busy-waits (driver timeout 20 s,
real refresh ~10 s). Consequences for the UI:

- Draw everything, then present once.
- Don't repaint for a clock tick.
- Give the user non-visual feedback (chirp + LED) the instant they press.

The busy-wait loop yields via `vTaskDelay`, so the idle tasks still run and
the task watchdog is not tripped — but `CONFIG_ESP_TASK_WDT_TIMEOUT_S=30` is
set anyway for headroom.
