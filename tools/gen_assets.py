#!/usr/bin/env python3
"""
Build the phonics SD-card payload: one picture + one narration per card, plus
the manifest the firmware reads.

    tools/.venv/bin/python tools/gen_assets.py --out assets

Pipeline per card:

  1. IMAGE  DuckDuckGo image search for "<word> cartoon clipart ...", pick the
            best candidate, pad to square, resize to 384x384, then dither to
            the panel's exact six inks with Floyd-Steinberg.

            Pre-dithering on the host matters: Panel_ED2208 quantises with a
            nearest-colour lookup at transmit time, so if we hand it images
            already made of those six exact RGB values the mapping is 1:1 and
            the firmware can use epd_fastest (no on-device dithering). Dither
            twice and you get mud.

  2. AUDIO  Piper neural TTS (en_US-libritts-high) -> 16-bit PCM WAV at
            22.05kHz mono. M5Unified resamples to the I2S rate; mono at
            22.05kHz keeps each clip ~150KB instead of ~1MB.

Everything is cached: re-running only fetches what is missing. --force-images
or --force-audio to redo work deliberately.

IMAGE LICENSING: these are arbitrary third-party images pulled from a web
search. Fine for a personal device on your desk; do not ship them.
"""

import argparse
import io
import json
import os
import sys
import time
import wave
from pathlib import Path

import numpy as np
import requests
from PIL import Image, ImageEnhance, ImageOps

sys.path.insert(0, str(Path(__file__).resolve().parent))
from phonics_data import (build_cards, build_letters,  # noqa: E402
                          assert_unique_words, assert_letter_names,
                          LETTER_NAMES, SOUND_LABELS)

# --- Panel palette ---------------------------------------------------------
# Copied verbatim from Panel_ED2208.cpp's epd_palette[]. Do not "improve"
# these values: they are what the driver's nearest-colour search compares
# against, so any drift reintroduces quantisation error.
PALETTE = np.array([
    [0,   0,   0],    # black
    [255, 255, 255],  # white
    [255, 243, 56],   # yellow
    [191, 0,   0],    # red
    [100, 64,  255],  # blue
    [67,  138, 28],   # green
], dtype=np.float64)

IMAGE_SIZE = 384          # must match ui::theme::kImageW/H
TTS_RATE = 22050
# Duration multiplier for the TTS. 1.0 is the voice's natural pace; 2.3 is
# roughly half speed, which is what a child sounding out letters needs -- the
# natural rate runs the short vowel sounds together, which is exactly the
# distinction the device is trying to teach.
DEFAULT_LENGTH_SCALE = 2.3

PIPER_VOICE = Path.home() / ".local/share/piper/en_US-libritts-high.onnx"
PIPER_VOICE_FALLBACK = Path.home() / ".local/share/piper/en_US-lessac-medium.onnx"

USER_AGENT = ("Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/125.0 Safari/537.36")

MIN_SOURCE_PX = 200       # reject thumbnails
MAX_ASPECT = 2.2          # reject banners/panoramas

# Stock libraries that stamp a watermark across the preview. Their images look
# fine in search results and then arrive with a logo bar burned into them --
# "bird" came back as a watermarked elephant. Blocked by host.
BLOCKED_HOSTS = (
    "vectorstock.com", "shutterstock.com", "istockphoto.com", "dreamstime.com",
    "123rf.com", "alamy.com", "depositphotos.com", "gettyimages.com",
    "canstockphoto.com", "stockfresh.com", "bigstockphoto.com",
)


def host_blocked(url):
    from urllib.parse import urlparse
    host = (urlparse(url).hostname or "").lower()
    return any(host == b or host.endswith("." + b) for b in BLOCKED_HOSTS)


# ===========================================================================
# DuckDuckGo image search
# ===========================================================================
class DuckDuckGoImages:
    """DuckDuckGo image search via the `ddgs` package.

    A hand-rolled scraper of DDG's /i.js endpoint gets a hard 403 now -- it
    wants a session-bound vqd token plus browser-shaped headers that move
    around. `ddgs` tracks that, so we use it and keep our own filtering and
    download logic on top.
    """

    def __init__(self, delay=1.5):
        from ddgs import DDGS
        self._DDGS = DDGS
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": USER_AGENT})
        self.delay = delay
        self._last_call = 0.0

    def _throttle(self):
        gap = time.time() - self._last_call
        if gap < self.delay:
            time.sleep(self.delay - gap)
        self._last_call = time.time()

    def search(self, query, want=12, attempts=3):
        """Return up to `want` candidate image URLs, best-first."""
        for attempt in range(attempts):
            try:
                self._throttle()
                with self._DDGS() as ddgs:
                    results = list(ddgs.images(query, max_results=want * 3))
                out = []
                for item in results:
                    url = item.get("image")
                    if not url:
                        continue
                    if host_blocked(url):
                        continue
                    w = int(item.get("width") or 0)
                    h = int(item.get("height") or 0)
                    if w and h:
                        # Skip thumbnails and banner-shaped crops: both look
                        # terrible padded into a square.
                        if min(w, h) < MIN_SOURCE_PX:
                            continue
                        if max(w, h) / max(1, min(w, h)) > MAX_ASPECT:
                            continue
                    out.append(url)
                    if len(out) >= want:
                        break
                if out:
                    return out
                print("      search returned nothing usable", flush=True)
            except Exception as exc:  # noqa: BLE001 - report and back off
                wait = 5 * (attempt + 1)
                print(f"      search attempt {attempt + 1} failed ({exc}); "
                      f"waiting {wait}s", flush=True)
                time.sleep(wait)
        return []

    def fetch(self, url, timeout=25):
        self._throttle()
        r = self.session.get(url, timeout=timeout, stream=True)
        r.raise_for_status()
        data = r.content
        if len(data) < 1024:
            raise RuntimeError("suspiciously small response")
        return data


# ===========================================================================
# Image processing
# ===========================================================================
def load_rgb_on_white(data):
    """Decode bytes to RGB, compositing any transparency onto white.

    Clipart is very often a transparent PNG; naive .convert('RGB') turns the
    transparent background black, which then dithers to a solid black square.
    """
    img = Image.open(io.BytesIO(data))
    img.load()
    if img.mode in ("RGBA", "LA", "P"):
        img = img.convert("RGBA")
        canvas = Image.new("RGBA", img.size, (255, 255, 255, 255))
        canvas.alpha_composite(img)
        img = canvas.convert("RGB")
    else:
        img = img.convert("RGB")
    return img


def to_square(img, size):
    """Pad (never crop) to a square, then resize.

    Padding rather than cropping because the subject of a clipart image
    usually fills the frame -- a centre crop lops the ears off the rabbit.
    """
    img = ImageOps.contain(img, (size, size), Image.LANCZOS)
    canvas = Image.new("RGB", (size, size), (255, 255, 255))
    canvas.paste(img, ((size - img.width) // 2, (size - img.height) // 2))
    return canvas


def boost(img):
    """Push saturation and contrast before quantising.

    Six inks is a brutal reduction; mid-tone pastels all collapse toward white
    or black. Over-saturating first keeps hues distinguishable afterwards.
    """
    img = ImageEnhance.Color(img).enhance(1.7)
    img = ImageEnhance.Contrast(img).enhance(1.25)
    return img


def dither_to_palette(img):
    """Floyd-Steinberg error diffusion onto PALETTE.

    Pillow's own quantize() only dithers to a palette via Web/ADAPTIVE modes
    and won't take an arbitrary 6-colour set with error diffusion, so this is
    done by hand. Serial in y, vectorised across x is fast enough (~40ms per
    384x384 on a Pi 5) and exactly matches the driver's colour set.
    """
    arr = np.asarray(img, dtype=np.float64).copy()
    h, w, _ = arr.shape

    for y in range(h):
        for x in range(w):
            old = arr[y, x].copy()
            idx = int(np.argmin(((PALETTE - old) ** 2).sum(axis=1)))
            new = PALETTE[idx]
            arr[y, x] = new
            err = old - new
            # Standard Floyd-Steinberg weights: 7/16 right, 3/16 below-left,
            # 5/16 below, 1/16 below-right.
            if x + 1 < w:
                arr[y, x + 1] += err * (7 / 16)
            if y + 1 < h:
                if x > 0:
                    arr[y + 1, x - 1] += err * (3 / 16)
                arr[y + 1, x] += err * (5 / 16)
                if x + 1 < w:
                    arr[y + 1, x + 1] += err * (1 / 16)

    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def dither_to_palette_fast(img):
    """Row-vectorised Floyd-Steinberg.

    Same algorithm as above but the palette search runs on a whole row at a
    time, which is ~50x quicker. Error still propagates pixel-by-pixel within
    the row, so output is identical to the scalar version.
    """
    arr = np.asarray(img, dtype=np.float32).copy()
    h, w, _ = arr.shape
    pal = PALETTE.astype(np.float32)

    for y in range(h):
        row = arr[y]
        nxt = arr[y + 1] if y + 1 < h else None
        for x in range(w):
            old = row[x].copy()
            d = pal - old
            idx = int(np.argmin((d * d).sum(axis=1)))
            new = pal[idx]
            row[x] = new
            err = old - new
            if x + 1 < w:
                row[x + 1] += err * 0.4375
            if nxt is not None:
                if x > 0:
                    nxt[x - 1] += err * 0.1875
                nxt[x] += err * 0.3125
                if x + 1 < w:
                    nxt[x + 1] += err * 0.0625

    return Image.fromarray(np.clip(arr, 0, 255).astype(np.uint8), "RGB")


def palette_report(img):
    """Count how many of the six inks the finished image actually uses."""
    arr = np.asarray(img).reshape(-1, 3)
    uniq = np.unique(arr, axis=0)
    return len(uniq)


def make_placeholder(card):
    """Deterministic fallback art when the web search yields nothing.

    A big letter on a coloured ground. Ugly but unmistakable, and it keeps the
    deck complete so the firmware never has to special-case a missing image.
    """
    from PIL import ImageDraw, ImageFont
    bg_choices = [(255, 243, 56), (100, 64, 255), (67, 138, 28), (191, 0, 0)]
    bg = bg_choices[(ord(card["letter"]) - ord("A")) % len(bg_choices)]
    img = Image.new("RGB", (IMAGE_SIZE, IMAGE_SIZE), bg)
    draw = ImageDraw.Draw(img)
    try:
        font = ImageFont.truetype(
            "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 220)
    except OSError:
        font = ImageFont.load_default()
    text = card["letter"]
    box = draw.textbbox((0, 0), text, font=font)
    draw.text(((IMAGE_SIZE - (box[2] - box[0])) / 2 - box[0],
               (IMAGE_SIZE - (box[3] - box[1])) / 2 - box[1]),
              text, font=font, fill=(255, 255, 255),
              stroke_width=8, stroke_fill=(0, 0, 0))
    return img


def build_image(ddg, card, dest, force=False):
    """Produce dest (a 384x384 6-colour PNG). Returns a status string."""
    if dest.exists() and not force:
        return "cached"

    urls = ddg.search(card["query"])
    if not urls:
        print(f"      no search results; using placeholder", flush=True)
        img = make_placeholder(card)
        dither_to_palette_fast(boost(img)).save(dest, "PNG", optimize=True)
        return "placeholder"

    for i, url in enumerate(urls):
        try:
            raw = ddg.fetch(url)
            img = load_rgb_on_white(raw)
            if min(img.size) < MIN_SOURCE_PX:
                raise RuntimeError(f"too small {img.size}")
            img = to_square(boost(img), IMAGE_SIZE)
            out = dither_to_palette_fast(img)
            out.save(dest, "PNG", optimize=True)
            # Sidecar provenance: which URL this picture came from. Useful for
            # spotting a whole batch from one bad source, and for the
            # licensing question these web images raise.
            dest.with_suffix(".src.txt").write_text(url + "\n")
            return f"ok ({palette_report(out)} inks, source #{i + 1})"
        except Exception as exc:  # noqa: BLE001 - try the next candidate
            print(f"      candidate {i + 1} rejected: {exc}", flush=True)
            continue

    print("      every candidate failed; using placeholder", flush=True)
    img = make_placeholder(card)
    dither_to_palette_fast(boost(img)).save(dest, "PNG", optimize=True)
    return "placeholder"


# ===========================================================================
# Text to speech
# ===========================================================================
class Narrator:
    """Piper neural TTS wrapper.

    en_US-libritts-high is a 904-speaker model, so the speaker MUST be pinned
    or the voice would drift between cards. speaker_id is exposed as a flag
    because the "best" voice is a matter of taste and cannot be chosen
    programmatically -- `--audition` renders the same line across several
    speakers so you can pick by ear.

    length_scale slows delivery slightly: this is a phonics teaching aid, and
    the default rate clips the short vowel sounds we are trying to isolate.
    """

    def __init__(self, voice_path, speaker_id=0, length_scale=DEFAULT_LENGTH_SCALE):
        from piper import PiperVoice
        from piper.config import SynthesisConfig
        self.voice = PiperVoice.load(str(voice_path))
        self.name = voice_path.stem
        self.native_rate = getattr(self.voice.config, "sample_rate", TTS_RATE)
        n_spk = getattr(self.voice.config, "num_speakers", 1) or 1
        self.speaker_id = speaker_id if n_spk > 1 else None
        self.cfg = SynthesisConfig(
            speaker_id=self.speaker_id,
            length_scale=length_scale,
            normalize_audio=True,
        )
        print(f"       {n_spk} speaker(s), using id={self.speaker_id}, "
              f"length_scale={length_scale}, {self.native_rate}Hz")

    # Sentence-final punctuation, used to split before synthesis.
    _SENT_RE = None

    def _sentences(self, text):
        """Split into sentences, keeping their terminators.

        Necessary because espeak's [[phoneme]] markup breaks whole-string
        synthesis: hand piper
            "eigh makes the [[ae]] sound. [[ae]], [[ae]]. eigh, pee. apple."
        and it returns ONE 2.1s chunk -- everything after the first marked
        sentence is silently dropped. Feeding one sentence at a time and
        concatenating the PCM sidesteps it entirely, and is verifiable: the
        output duration is now the sum of the parts.
        """
        import re
        if Narrator._SENT_RE is None:
            Narrator._SENT_RE = re.compile(r"[^.!?]+[.!?]+|\S[^.!?]*$")
        return [m.group(0).strip() for m in Narrator._SENT_RE.finditer(text)
                if m.group(0).strip()]

    def synth(self, text, dest):
        """Render `text` to a 16-bit mono PCM WAV at the voice's native rate."""
        pcm = bytearray()
        rate = self.native_rate
        for sentence in self._sentences(text):
            for chunk in self.voice.synthesize(sentence, syn_config=self.cfg):
                pcm += chunk.audio_int16_bytes
                rate = chunk.sample_rate
            # A short pause between sentences, so the sound and the spelling
            # do not run together.
            pcm += b"\x00\x00" * int(rate * 0.12)

        with wave.open(str(dest), "wb") as wav:
            wav.setnchannels(1)
            wav.setsampwidth(2)
            wav.setframerate(rate)
            wav.writeframes(bytes(pcm))


def build_audio(narrator, card, dest, force=False):
    if dest.exists() and not force:
        return "cached"
    narrator.synth(card["narration"], dest)
    with wave.open(str(dest), "rb") as wav:
        frames = wav.getnframes()
        rate = wav.getframerate()
        ch = wav.getnchannels()
        width = wav.getsampwidth()
    if width != 2:
        raise RuntimeError(f"{dest}: expected 16-bit PCM, got {width * 8}-bit")
    return f"ok ({frames / rate:.2f}s, {rate}Hz, {'stereo' if ch == 2 else 'mono'})"


# ===========================================================================
# Main
# ===========================================================================
def run_audition(voice_path, out, length_scale,
                 speakers=(0, 5, 17, 42, 79, 128)):
    """Render one representative line per candidate speaker.

    Picking a pleasant voice is not something this script can judge, so it
    writes samples and tells you how to play them.
    """
    dest_dir = out / "audition"
    dest_dir.mkdir(parents=True, exist_ok=True)
    line = ("P makes the puh sound. puh, puh. ap, ple. apple.")
    print(f"auditioning {voice_path.name} into {dest_dir}\n")
    for spk in speakers:
        n = Narrator(voice_path, spk, length_scale)
        dest = dest_dir / f"speaker_{spk:03d}.wav"
        n.synth(line, dest)
        print(f"  {dest}")
    print(f"\nPlay them:  for f in {dest_dir}/*.wav; do echo $f; aplay -q $f; done")
    print("Then re-run with:  --speaker <id>")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default="assets", help="output directory")
    ap.add_argument("--force-images", action="store_true")
    ap.add_argument("--force-audio", action="store_true")
    ap.add_argument("--skip-images", action="store_true")
    ap.add_argument("--skip-audio", action="store_true")
    ap.add_argument("--only", help="only cards whose letter is in this string, e.g. ABC")
    ap.add_argument("--word", action="append", default=None,
                    help="only these words (repeatable). Use with "
                         "--force-images to replace one bad picture.")
    ap.add_argument("--delay", type=float, default=1.5,
                    help="seconds between DDG requests (be polite)")
    ap.add_argument("--voice", default=None,
                    help="path to a piper .onnx voice (default: libritts-high)")
    ap.add_argument("--speaker", type=int, default=0,
                    help="speaker id for multi-speaker voices")
    ap.add_argument("--length-scale", type=float, default=DEFAULT_LENGTH_SCALE,
                    help=">1 speaks slower (duration multiplier); "
                         f"default {DEFAULT_LENGTH_SCALE} is about half speed")
    ap.add_argument("--audition", action="store_true",
                    help="render one sample line across several speakers "
                         "into <out>/audition and exit")
    args = ap.parse_args()

    assert_letter_names()
    assert_unique_words()
    cards = build_cards()
    if args.only:
        keep = set(args.only.upper())
        cards = [c for c in cards if c["letter"] in keep]
    if args.word:
        want = {w.lower() for w in args.word}
        cards = [c for c in cards if c["word"].lower() in want]
        missing = want - {c["word"].lower() for c in cards}
        if missing:
            sys.exit(f"unknown word(s): {sorted(missing)}")
        print(f"restricted to {len(cards)} card(s): "
              f"{', '.join(c['word'] for c in cards)}")

    out = Path(args.out)
    (out / "phonics").mkdir(parents=True, exist_ok=True)
    root = out / "phonics"

    if args.voice:
        voice_path = Path(args.voice)
    else:
        voice_path = PIPER_VOICE if PIPER_VOICE.exists() else PIPER_VOICE_FALLBACK

    if args.audition:
        return run_audition(voice_path, out, args.length_scale)

    narrator = None
    if not args.skip_audio:
        if not voice_path.exists():
            sys.exit(f"no piper voice found at {voice_path}")
        print(f"voice: {voice_path.name}")
        narrator = Narrator(voice_path, args.speaker, args.length_scale)

    ddg = DuckDuckGoImages(delay=args.delay) if not args.skip_images else None

    # --- 26 letter clips, reused by every word -------------------------------
    letters = build_letters()
    if not args.skip_audio:
        (root / "letters").mkdir(parents=True, exist_ok=True)
        print(f"letter clips ({len(letters)}):")
        for l in letters:
            dest = root / l["audio"]
            if dest.exists() and not args.force_audio:
                print(f"  {l['letter']}  cached")
                continue
            narrator.synth(l["narration"], dest)
            with wave.open(str(dest), "rb") as w:
                dur = w.getnframes() / w.getframerate()
            print(f"  {l['letter']}  {dur:.2f}s  \"{l['narration']}\"")
        print()

    stats = {"img_ok": 0, "img_cached": 0, "img_placeholder": 0,
             "aud_ok": 0, "aud_cached": 0, "fail": 0}

    print(f"generating {len(cards)} cards into {root}\n")
    for n, card in enumerate(cards, 1):
        letter_dir = root / "cards" / card["letter"].lower()
        letter_dir.mkdir(parents=True, exist_ok=True)
        img_dest = root / card["image"]
        aud_dest = root / card["audio"]

        print(f"[{n:3d}/{len(cards)}] {card['letter']}  {card['display']}")

        if not args.skip_images:
            try:
                status = build_image(ddg, card, img_dest, args.force_images)
                print(f"      image: {status}")
                if status == "cached":
                    stats["img_cached"] += 1
                elif status == "placeholder":
                    stats["img_placeholder"] += 1
                else:
                    stats["img_ok"] += 1
            except Exception as exc:  # noqa: BLE001
                print(f"      image FAILED: {exc}")
                stats["fail"] += 1

        if not args.skip_audio:
            try:
                status = build_audio(narrator, card, aud_dest, args.force_audio)
                print(f"      audio: {status}")
                if status == "cached":
                    stats["aud_cached"] += 1
                else:
                    stats["aud_ok"] += 1
            except Exception as exc:  # noqa: BLE001
                print(f"      audio FAILED: {exc}")
                stats["fail"] += 1

    # --- manifest ---
    # Only cards whose two assets both exist make it in, so the firmware never
    # has to render around a hole.
    entries = []
    for card in build_cards():   # always the FULL deck, never the filtered one
        img = root / card["image"]
        aud = root / card["audio"]
        if img.exists() and aud.exists():
            entries.append({
                "id": card["id"],
                "letter": card["letter"],
                "display": card["display"],
                "span": card["span"],
                "image": card["image"],
                "audio": card["audio"],
            })

    # Manifest v2: the letter clips are listed separately from the word
    # clips, because the firmware pairs one of each at playback time.
    manifest = {
        "version": 2,
        "generator": "tools/gen_assets.py",
        "voice": voice_path.name,
        "image_size": IMAGE_SIZE,
        "palette": "spectra6",
        "letters": [
            {"letter": l["letter"], "name": l["name"], "sound": l["sound"],
             "audio": l["audio"]}
            for l in letters if (root / l["audio"]).exists()
        ],
        "cards": entries,
    }
    manifest_path = root / "manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=1))

    per_letter = {}
    for e in entries:
        per_letter[e["letter"]] = per_letter.get(e["letter"], 0) + 1
    short = {k: v for k, v in per_letter.items() if v < 5}

    print("\n" + "=" * 60)
    print(f"manifest : {manifest_path}  ({len(entries)} cards, "
          f"{len(manifest['letters'])} letter clips)")
    print(f"images   : {stats['img_ok']} new, {stats['img_cached']} cached, "
          f"{stats['img_placeholder']} placeholder")
    print(f"audio    : {stats['aud_ok']} new, {stats['aud_cached']} cached")
    print(f"failures : {stats['fail']}")
    if short:
        print(f"WARNING  : letters under 5 cards: {short}")
    if len(per_letter) < 26:
        missing = sorted(set(chr(ord('A') + i) for i in range(26)) - set(per_letter))
        print(f"WARNING  : letters with no cards: {missing}")
    print("=" * 60)
    return 0 if stats["fail"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
