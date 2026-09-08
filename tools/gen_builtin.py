#!/usr/bin/env python3
"""
Regenerate the two cards embedded in the firmware image.

Built-in cards carry ONE combined clip (letter narration + word narration
concatenated) rather than the split pair the SD deck uses, because embedding
all 26 letter clips would cost ~6.5MB of flash. The practical consequence is
that stepping the taught letter through a built-in word replays the same
audio -- per-letter stepping needs the SD card. The built-ins exist so the
device is testable with no card at all, and that trade is worth it.

    tools/.venv/bin/python tools/gen_builtin.py
"""

import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_assets import Narrator, PIPER_VOICE, PIPER_VOICE_FALLBACK  # noqa: E402
from phonics_data import (build_cards, letter_narration,  # noqa: E402
                          word_narration)

ROOT = Path(__file__).resolve().parent.parent
ASSETS = ROOT / "assets" / "phonics"
EMBED = ROOT / "main" / "embedded"

# (letter, word) pairs to embed. Keep them short and visually distinct.
BUILTINS = [("A", "apple"), ("C", "cat")]


def main():
    voice = PIPER_VOICE if PIPER_VOICE.exists() else PIPER_VOICE_FALLBACK
    n = Narrator(voice)
    cards = {c["id"]: c for c in build_cards()}

    for letter, word in BUILTINS:
        card = cards[f"{letter.lower()}_{word}"]

        # One utterance, so the two halves are prosodically joined rather than
        # sounding like two recordings butted together.
        text = f"{letter_narration(letter)} {word_narration(word)}"
        dest = EMBED / f"{word}.wav"
        n.synth(text, dest)

        src_png = ASSETS / card["image"]
        if not src_png.exists():
            sys.exit(f"missing {src_png}; run gen_assets.py first")
        shutil.copy(src_png, EMBED / f"{word}.png")

        import wave
        with wave.open(str(dest), "rb") as w:
            dur = w.getnframes() / w.getframerate()
        print(f"  {word:6} {dur:5.2f}s  {dest.stat().st_size:>7} B")
        print(f"         \"{text}\"")
    return 0


if __name__ == "__main__":
    sys.exit(main())
