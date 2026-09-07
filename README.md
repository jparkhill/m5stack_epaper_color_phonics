# Phonics Cards

A letter-sounds flashcard device for kids, built on the **M5Stack PaperColor**
(SKU C151) — an ESP32-S3 with a 400×600 six-colour Spectra 6 e-paper panel.

Press the top button and the screen shows a picture with a short word. The
letters that make the taught sound are **uppercase and coloured**; the rest of
the word is lowercase. Then the speaker sounds it out:

> **aPPle** — *"P makes the puh sound. puh, puh. ap, ple. apple."*

The status bar across the top shows the date, time, temperature and humidity.

26 letters × 5 words = **130 cards**, all pictures and narration pre-generated
on the host and written to a microSD card.

---

## Layout

```
+--------------------------------------------------+  y=0
|  16:45                              22.4 °C      |
|  Sun 7 Sep 2026                     41% RH       |   status bar (400x64)
+==================================================+  y=64  (blue rule)
|                                                  |
|            +------------------------+            |
|            |                        |            |
|            |     square picture     |            |   image (384x384)
|            |       384 x 384        |            |
|            |                        |            |
|            +------------------------+            |
|                                                  |  y=456
|                   a P P l e                      |   word band (400x98)
|                                                  |
|      TOP: new word  MID: next letter  LOW: again |   footer
+--------------------------------------------------+  y=600
```

Every region is a `ui::Widget` with its own `Rect`, and all geometry lives in
[`main/ui/theme.h`](main/ui/theme.h). Adding an information panel means
writing a widget and giving it a rectangle — see
[`main/ui/screen.cpp`](main/ui/screen.cpp).

## Buttons

The device has four buttons: one on top and three on the side. The **lowest
side button is a hardware power button** wired to the PMIC and is not readable
as a GPIO, so firmware sees exactly three.

| Action | Cost |
|---|---|
| **Either cycle button** — next card: picture + word, then narration | one panel refresh (~16 s) |
| **Hold a cycle button** (0.7 s) — jump to the next letter | one panel refresh |
| **Third button** — replay the current narration | instant, no refresh |

Which GPIO is which physical button is not documented in any M5 source, so
*both* cycle buttons do the same thing and it doesn't matter which is which.
Run `btn` on the console and press each one to identify them.

---

## Power

The device powers itself **off** after **15 minutes** with no user activity
(`kIdleSleepSec` in [`main/app/app.h`](main/app/app.h)). This is a real rail
cut through the M5PM1 PMIC, not a CPU sleep state, so **only the hardware
power button wakes it** — the lowest of the three side buttons.

Because the e-paper panel is bistable, **the last card stays on the screen the
whole time it is off, at zero power**. A sleeping device looks exactly like a
printed flashcard. That is also why sleeping does not draw a "goodbye" screen:
leaving the child's last word up is more useful than spending a 16 s refresh
to replace it. Two descending notes play so it is clear the device chose to
sleep rather than crashed.

Only button presses and console commands count as activity — the idle clock
repaint deliberately does not, or it would keep the device awake forever.

`sleep` on the console triggers the identical path, which is how to test it
without waiting a quarter of an hour. `stat` reports the countdown.

## Refreshing the screen

There is no automatic periodic redraw beyond the clock. The screen changes
only when:

| Trigger | What happens |
|---|---|
| Either cycle button, or `next` | New card + narration, one refresh |
| Hold a cycle button, or `letter` | Next letter, one refresh |
| Third button, or `again` | Narration replays, **no** refresh |
| `repaint` | Recomposes the same card (picks up a new clock/temperature) |
| 5 minutes idle | Clock repaint (`kIdleClockRefreshSec`) |

Each refresh is ~16 s of blocking panel time, so the design spends them
sparingly and gives feedback through the chirp and the LEDs instead.

## Build and flash

Needs ESP-IDF v5.4+ (developed against 5.5.4). The target is pinned in
`sdkconfig.defaults`, so **no `idf.py set-target` step** is required.

```bash
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py monitor` works over the same USB-C cable as flashing, because this
project deliberately does **not** enable TinyUSB. On the ESP32-S3 the internal
USB PHY routes to *either* USB-Serial-JTAG *or* USB-OTG, never both — so any
firmware that claims USB for mass storage loses the serial console.

Dependencies (`m5gfx`, `m5unified`) come from the Espressif component registry
rather than git submodules; the M5GFX git tree carries ~128 MB of CJK font
sources this project never compiles.

## Generating the card assets

```bash
sudo apt-get install -y python3-pil python3-numpy
python3 -m venv --system-site-packages tools/.venv
tools/.venv/bin/pip install -r tools/requirements.txt

tools/.venv/bin/python tools/gen_assets.py --out assets
tools/provision_sd.sh /dev/sdX --format      # DESTRUCTIVE
tools/set_time.py                            # set the clock over serial
```

### Previewing the layout

An e-paper refresh is ~16 s, which makes iterating on typography on the device
miserable. `preview_layout.py` renders the screen on the host instead — it
parses the geometry straight out of `main/ui/theme.h` so it cannot drift from
the firmware, and dithers to the same six inks:

```bash
tools/.venv/bin/python tools/preview_layout.py --card q_question -o /tmp/p.png
```

It is an approximation of glyph metrics, not a pixel-exact simulator — use it
for proportions, sizing and spacing.

The status-bar gradient is generated separately and embedded in the firmware:

```bash
tools/.venv/bin/python tools/gen_chrome.py     # -> main/embedded/statusbar.png
```

`gen_assets.py` caches aggressively — re-running only fetches what is missing.
Use `--only ABC` to work on a few letters, `--force-images` / `--force-audio`
to redo work deliberately.

**Pictures** come from DuckDuckGo image search, padded to square, resized to
384×384, and dithered with Floyd-Steinberg onto the panel's six inks. Doing
this on the host matters: the panel driver quantises with a nearest-colour
lookup at transmit time, so images made of those exact six RGB values map 1:1
and the firmware can present them with no on-device dithering. Dither twice
and you get mud.

**Narration** is [Piper](https://github.com/rhasspy/piper) neural TTS
(`en_US-libritts-high`) at 22.05 kHz 16-bit mono, rendered at
`--length-scale 2.3` — roughly half the voice's natural pace. That is
deliberate: at a normal speaking rate the short vowel sounds run together,
which is exactly the distinction the device is trying to teach.

That voice has 904 speakers, so the speaker id is pinned; `--audition` renders
one line across several speakers so you can pick by ear:

```bash
tools/.venv/bin/python tools/gen_assets.py --audition --out /tmp/aud
for f in /tmp/aud/audition/*.wav; do echo $f; aplay -q $f; done
tools/.venv/bin/python tools/gen_assets.py --out assets --speaker 42 --force-audio
```

> **Image licensing:** these are arbitrary third-party images from a web
> search. Fine for a device on your own desk; do not ship them. Swapping in
> Openverse or your own art means changing one function in `gen_assets.py`.

The card must be **FAT32** — exFAT is compiled out (`FF_FS_EXFAT=0`).

## Content

Words and narration live in [`tools/phonics_data.py`](tools/phonics_data.py),
one table. To change a word, edit that table and re-run the generator.

Word choice is deliberate: the target letter is **not** always word-initial.
For short vowels a medial letter teaches the sound far better — `bUg` beats a
stretch like "unicycle", which is actually a /juː/ sound. `X` is medial and
final (`boX`, `siX`) because that is where children meet its /ks/ sound.

---

## Debug console

The whole point of the serial console is that a debug cycle should not need a
reflash. Type `help` for the list.

| Command | Does |
|---|---|
| `stat` | Uptime, deck, sensors, RTC, SD, heap |
| `next` / `letter` / `again` | Same as the three buttons |
| `card P 2` | Show a specific card |
| `deck` | Per-letter card counts |
| `time 2026-09-07 20:15:00` | Set the RTC |
| `btn` | Watch raw button GPIOs for 8 s |
| `sd` / `sd remount` | Card info; re-mount after a swap |
| `i2c` | Scan the internal I2C bus |
| `env` | Read the SHT40 now |
| `vol 200` / `beep` | Speaker |
| `audio` | Codec register read-back + a loud 2 s test tone |
| `repaint` | Force a full repaint |
| `reboot` | Restart |

Boot is staged and traced, so a cold start is diagnosable from one paste:

```
+============================================================+
|  PHONICS CARDS  --  M5Stack PaperColor (C151)              |
+============================================================+
I (301) boot: app          : phonics_cards v0.1.0
I (302) boot: reset reason : POWERON (cold start / battery insert)
I (303) boot: PSRAM        : 8192 KB (octal) -- required by the panel driver
I (410) boot:   [ OK ] display             412.3 ms  400x600 Spectra 6
I (455) boot:   [ OK ] microSD             120.7 ms  30436 MB, 20000 kHz
I (992) boot:   [ OK ] deck                537.1 ms  130 cards, 26 letters
+------------------------------------------------------------+
|  VERDICT: PASS -- all stages healthy                       |
+------------------------------------------------------------+
```

A failing stage **degrades rather than aborts** — no SD card puts a
diagnostic on the screen and still gives you a console.

## Known constraints

These are properties of the hardware, not bugs:

- **~16 s per screen update.** Spectra 6 has no partial refresh; every update
  re-transmits the whole framebuffer and runs a full POWER_ON → REFRESH →
  POWER_OFF cycle. Hence one refresh per action, a chirp plus an animated LED
  for instant feedback, and a replay button that never touches the panel.
  Composing the frame itself takes only ~157 ms; the rest is the panel.
- **The idle clock is up to 5 minutes stale** (`kIdleClockRefreshSec` in
  `main/app/app.h`). Repainting every minute would mean a ~10 s refresh every
  minute, and e-paper panels have a finite cycle count. Any button press
  repaints immediately.
- **Six colours, no greys.** Grey subjects dither to blue-and-white — the
  anchor card is a good example. The generator over-saturates before
  quantising to keep hues apart.
- **SD and the panel share SPI2.** Assets are read into PSRAM *before* a
  refresh starts, never during.
- **No network.** The clock is set over serial, by design.
- **The shoulder LEDs animate on CPU1.** A rainbow sweep runs in its own task
  pinned to core 1 specifically so it keeps moving during the multi-second
  panel refresh that blocks core 0.

### Editing sdkconfig.defaults

ESP-IDF only reads `sdkconfig.defaults` when `sdkconfig` does not yet exist.
After changing defaults you must delete the generated file or the change
silently does nothing:

```bash
rm -f sdkconfig && idf.py build
```

## Docs

- [`docs/hardware.md`](docs/hardware.md) — pin map, panel behaviour, and the
  traps found the hard way.

## Licence

MIT (see `LICENSE`). M5GFX and M5Unified are MIT, © M5Stack.
