# Phonics Cards

A letter-sounds flashcard device for kids, built on the **M5Stack PaperColor**
(SKU C151) — an ESP32-S3 with a 400×600 six-colour Spectra 6 e-paper panel.

Press the top button and the screen shows a picture with a short word. The
letters that make the taught sound are **uppercase and coloured**; the rest of
the word is lowercase. Then the speaker sounds it out:

> **aPPle** — *"P makes the puh sound. puh, puh. ap, ple. apple."*

The status bar across the top shows the date, time, temperature and humidity.

26 letters × 10 words = **260 cards**, all pictures and narration
pre-generated on the host and written to a microSD card.

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

Four buttons: one on top, three on the side. The **lowest side button is
`PWR_KEY`**, wired to the PMIC — it powers the device on and off, is not
readable as a GPIO, and is deliberately unlabelled on screen.

| Button | GPIO | Action | Cost |
|---|---|---|---|
| **Upper side** | 10 | Next card — a random word | one refresh (~16 s) |
| **Lower side** | 9 | Repeat the current letter + word | none |
| **Top** (middle of the title bar) | 1 | Next letter **within the current word** | one refresh |
| `PWR_KEY` | — | Power off / wake | — |

No hold or double-press gestures; one press, one action. The on-screen hints
sit next to the buttons they describe — `▲ letter` points up into the title
bar where the top button is, and `◀ word` / `◀ again` sit in a left gutter at
the two side buttons' heights. The arrows are drawn as triangles because the
GFX fonts are ASCII-only and have no arrow glyphs.

### Stepping through a word

The top button moves the taught letter along the current word, so one picture
teaches every letter in it:

```
Apple  ->  aPple  ->  apPle  ->  appLe  ->  applE  ->  (wraps)
```

The highlighted letter is recoloured and its narration changes to match. This
is why the audio is **split into two clips**: one per letter (26 of them,
shared by every word) plus one per word. A clip for every (word, letter) pair
would be ~1,400 recordings; this needs 26 + 260, and the device just plays the
pair back to back.

**All 26 letter clips are embedded in the firmware too**, so this works
identically with no SD card inserted. They were originally left out to save
flash, with built-in cards carrying one combined clip — which made the top
button look broken: the highlight moved but the narration kept naming the
card's *original* letter. 26 clips cost ~4.2 MB of a 15 MB partition, a far
better trade than a button that silently does the wrong thing.

**Stepping does not refresh the panel immediately.** A refresh is ~16 s and
the only visual change is which letter is coloured, so walking through
"apple" would take 80 seconds. Instead the audio plays at once and the
repaint is deferred until `kLetterStepRepaintSec` (4 s) after you stop
stepping — several steps cost one refresh, not one each.

### How cards are chosen

The upper side button picks a **random letter**, then a random word for it —
biased so a word whose taught grapheme is the word's **first letter** is
chosen **60 %** of the time (`kInitialGraphemeBias` in
[`main/content/deck.h`](main/content/deck.h)). Initial sounds are easiest to
hear, so they dominate, but medial and final examples (`bUg`, `boX`, `siX`)
still appear. The same card never comes up twice in a row.

## Power

> **`PWR_KEY` gestures (from the M5PM1 spec):**
>
> | Gesture | Effect |
> |---|---|
> | **Quick press** | Power **on** |
> | **Double press** | Power **off** |
> | **Hold** | Enter ROM **download mode** |
>
> Do *not* hold the button to switch the device on. Holding it drops the chip
> into download mode: it enumerates on USB and esptool talks to it happily,
> but the application never runs, nothing is printed — not even ESP-IDF's own
> bootloader banner — and the screen never updates. It looks like a dead
> board. A quick press recovers it.
>
> If the idle power-off is more trouble than it is worth, `nosleep` on the
> console disables it permanently (saved to NVS); `autosleep` restores it.

After **15 minutes** with no user activity (`kIdleSleepSec` in
[`main/app/app.h`](main/app/app.h)) the device enters **ESP32-S3 deep sleep**.
**Any of the three front buttons wakes it**, and waking is a full chip reset,
so it boots from scratch.

It deliberately does *not* use the PMIC's shutdown for this. That cuts the
peripheral rails without resetting the chip on battery power, which leaves the
board half-powered — shoulder LEDs still cycling, panel and SD dead, and
unrecoverable by PWR_KEY. See [`docs/hardware.md`](docs/hardware.md).

`PWR_KEY` cannot wake from deep sleep because it is a PMIC pin, not an ESP32
GPIO. It still works as a hardware off/on, and `poweroff` on the console does
a true rail cut (falling back to deep sleep if the chip does not reset).

Because the e-paper panel is bistable, **the last card stays on the screen the
whole time it is off, at zero power**. A sleeping device looks exactly like a
printed flashcard. That is also why sleeping does not draw a "goodbye" screen:
leaving the child's last word up is more useful than spending a 16 s refresh
to replace it. Two descending notes play so it is clear the device chose to
sleep rather than crashed.

Only button presses and console commands count as activity — the idle clock
repaint deliberately does not, or it would keep the device awake forever.

**The timeout is deferred while a USB host is attached**
(`kSleepWhileUsbConnected`). Otherwise the device cuts power in the middle of
a `monitor` session or a flash, the port vanishes, and the board looks
bricked until someone presses `PWR_KEY`. On battery the timeout applies
normally, which is the case that matters.

`sleep` on the console triggers the identical path, which is how to test it
without waiting a quarter of an hour. `stat` reports the countdown.

## Refreshing the screen

There is no automatic periodic redraw beyond the clock. The screen changes
only when:

| Trigger | What happens |
|---|---|
| Upper side button, or `next` | Random card + narration, one refresh |
| Top button | Next letter within the word, one refresh |
| `letter` | Jump to the next letter of the alphabet, one refresh |
| `again` | Narration replays, **no** refresh |
| `step` | Step the taught letter (same as the top button) |
| `repaint` | Recomposes the same card (picks up a new clock/temperature) |
| 5 minutes idle | Clock repaint (`kIdleClockRefreshSec`) |

### Why a refresh takes ~16 s, and what is done about it

It cannot be made shorter. `epd_mode` looks like a speed control but is not —
in `Panel_ED2208` it only selects the dither algorithm, while the part that
costs the time (`_turn_on_display()`: POWER_ON → DISPLAY_REFRESH →
POWER_OFF) is a fixed command sequence with no mode dependency. The ~16 s is
the panel physically cycling pigment through its colour passes, and this
driver exposes no partial or fast waveform.

So instead the wait is *filled*. The frame is composed in PSRAM (~157 ms),
then the narration is started on an audio task pinned to **CPU 1**, and only
then is the refresh pushed on CPU 0. The child hears the word immediately
while the picture develops, rather than sitting through 16 s of silence and
then hearing it. The LEDs sweep throughout for the same reason.

Because the SD card shares SPI2 with the panel, a clip is read into PSRAM
*before* the refresh begins — never streamed off the card during it. That is
why `hal_audio` splits `preloadWavFile()` from `playPreloadedAsync()`.

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

The narration names the letter rather than sounding it: *"see makes the kuh
sound"*. Getting that right needed two tables, both **verified against
Piper's own phonemizer rather than by ear**, because the failures are silent —
the audio sounds confident and says the wrong thing:

| Written | espeak actually says | |
|---|---|---|
| `ay` | /ˈaɪ/ — "eye" | wrong name for **A**; use `eigh` |
| `eff` | /ɛf ɛf ɛf/ — "eff eff eff" | wrong name for **F**; use `ef` |
| `fff` | /ɛf ɛf ɛf/ | says the *name* three times, not the /f/ sound |
| `eh` | /eɪ/ — "ay" | not short-e |
| `ih` | /aɪ/ — "eye" | not short-i |
| `ks` | /keɪ ɛs/ — "kay-ess" | not /ks/ |

A bare vowel letter is *always* read as that letter's name, so an isolated
short vowel cannot be written as text at all. The fix is espeak's inline
phoneme markup, which passes IPA through verbatim: `SOUNDS` in
`phonics_data.py` holds `"A": "[[æ]]"`, `"F": "[[f]]"`, `"X": "[[ks]]"` and so
on. Stops keep a `-uh` syllable (`[[bʌ]]`, `[[kʌ]]`) because a plosive with no
following vowel is essentially inaudible — which is also what phonics
programmes teach.

Z is `zee` to match the American voice; change that one entry for `zed`.

That markup has one more trap: handed a whole multi-sentence narration
containing `[[...]]`, piper renders only the first marked sentence and
**silently drops the rest** — an output *shorter than either half alone* is
the tell. So `Narrator.synth` splits into sentences and synthesises them
individually, concatenating the PCM. On one narration that took the result
from 2.14 s to 8.07 s.

The word clip **spells the word by letter name** rather than sounding out
syllables: `apple` → *"eigh, pee, pee, ell, ee. apple."* Syllable
segmentation was tried first and came out garbled — a TTS model is trained on
running speech, not on deliberately fragmented words.

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
| `batt` | Battery voltage, power source, LVP threshold, PMIC sleep config |
| `verify` | Stat every card asset and report what is missing (slow) |
| `nosleep` / `autosleep` | Disable / re-enable the idle auto power-off (persisted) |
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
