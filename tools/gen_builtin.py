#!/usr/bin/env python3
"""
Regenerate the assets embedded in the firmware image.

Two built-in cards (picture + word clip) plus ALL 26 shared letter clips, so
the device behaves identically with no microSD inserted -- including stepping
the taught letter through the word.

The letter clips were originally left out to save flash, and built-in cards
carried one combined clip instead. That made the top button appear broken:
stepping moved the on-screen highlight but kept narrating the card's original
letter. 26 clips cost ~4MB of a 15MB partition, which is a much better trade
than a button that silently does the wrong thing.

    tools/.venv/bin/python tools/gen_builtin.py
"""

import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_assets import Narrator, PIPER_VOICE, PIPER_VOICE_FALLBACK  # noqa: E402
from phonics_data import (build_cards, letter_narration,  # noqa: E402
                          word_narration, LETTERS)

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "phonics"
EMBED = ROOT / "main" / "embedded"

# (letter, word) pairs to embed. Keep them short and visually distinct.
BUILTINS = [("A", "apple"), ("C", "cat")]


def main():
    voice = PIPER_VOICE if PIPER_VOICE.exists() else PIPER_VOICE_FALLBACK
    n = Narrator(voice)
    cards = {c["id"]: c for c in build_cards()}
    EMBED.mkdir(parents=True, exist_ok=True)
    (EMBED / "letters").mkdir(parents=True, exist_ok=True)

    import wave
    total = 0

    # --- the two cards: picture + word clip (spelling only) ---
    for letter, word in BUILTINS:
        card = cards[f"{letter.lower()}_{word}"]

        dest = EMBED / f"{word}.wav"
        n.synth(word_narration(word), dest)

        src_png = ASSETS / card["image"]
        if not src_png.exists():
            sys.exit(f"missing {src_png}; run gen_assets.py first")
        shutil.copy(src_png, EMBED / f"{word}.png")

        with wave.open(str(dest), "rb") as w:
            dur = w.getnframes() / w.getframerate()
        total += dest.stat().st_size
        print(f"  {word:6} {dur:5.2f}s {dest.stat().st_size:>8} B  "
              f"\"{word_narration(word)}\"")

    # --- all 26 letter clips, so stepping works without an SD card ---
    # Prefer the ones gen_assets already produced; synthesise if absent.
    print()
    for L in LETTERS:
        dest = EMBED / "letters" / f"ltr_{L.lower()}.wav"
        src = ASSETS / "letters" / f"{L.lower()}.wav"
        if src.exists():
            shutil.copy(src, dest)
        else:
            n.synth(letter_narration(L), dest)
        total += dest.stat().st_size
    print(f"  26 letter clips -> {EMBED / 'letters'}")
    print(f"\n  embedded total: {total / 1024 / 1024:.2f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
