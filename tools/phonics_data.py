"""
Phonics content: 26 letters x 5 word/picture/sound triples.

Each entry is (word, syllables, grapheme_override).

  word       lowercase spelling; also the DuckDuckGo image search term
  syllables  hyphenated for the narration's sound-out step ("ap-ple")
  grapheme   the letter span that makes the taught sound. Normally None,
             meaning "the first occurrence of the target letter" -- which is
             correct for every word here except Q, where the sound comes from
             the digraph "qu".

Word choice notes:
  * Concrete, illustratable nouns wherever possible -- these become image
    searches, and "up" or "under" produce nonsense pictures.
  * The target letter does NOT have to be word-initial. For the short vowels
    especially, a medial letter is clearer: "bUg" teaches short-u far better
    than a stretch like "unicycle" (which is actually a /juː/ sound).
  * Every word is unique across the whole deck, so no two cards ever want the
    same picture. assert_unique_words() enforces this.
"""

# Letter -> the sound name spoken in the narration.
#
# Stops get the conventional "-uh" form (buh, duh, kuh); continuants get their
# stretched form (fff, lll, mmm) as phonics programmes teach them. These are
# also chosen to be strings a TTS model pronounces sensibly -- change them
# here if a particular voice mangles one.
SOUNDS = {
    "A": "ah",   "B": "buh",  "C": "kuh",  "D": "duh",  "E": "eh",
    "F": "fff",  "G": "guh",  "H": "huh",  "I": "ih",   "J": "juh",
    "K": "kuh",  "L": "lll",  "M": "mmm",  "N": "nnn",  "O": "aw",
    "P": "puh",  "Q": "kwuh", "R": "rrr",  "S": "sss",  "T": "tuh",
    "U": "uh",   "V": "vvv",  "W": "wuh",  "X": "ks",   "Y": "yuh",
    "Z": "zzz",
}

# Extra hint appended to the image search to bias towards flat, high-contrast
# cartoon art, which is what survives a 6-colour e-paper panel.
IMAGE_STYLE_HINT = "cartoon clipart for kids simple white background"

WORDS = {
    "A": [("apple", "ap-ple", None), ("ant", "ant", None),
          ("anchor", "an-chor", None), ("astronaut", "as-tro-naut", None),
          ("alligator", "al-li-ga-tor", None)],
    "B": [("ball", "ball", None), ("bear", "bear", None),
          ("bike", "bike", None), ("boat", "boat", None),
          ("bug", "bug", None)],
    "C": [("cat", "cat", None), ("cow", "cow", None),
          ("cup", "cup", None), ("car", "car", None),
          ("cake", "cake", None)],
    "D": [("dog", "dog", None), ("duck", "duck", None),
          ("door", "door", None), ("drum", "drum", None),
          ("dolphin", "dol-phin", None)],
    "E": [("egg", "egg", None), ("elephant", "el-e-phant", None),
          ("bed", "bed", None), ("hen", "hen", None),
          ("web", "web", None)],
    "F": [("fish", "fish", None), ("frog", "frog", None),
          ("fan", "fan", None), ("foot", "foot", None),
          ("flower", "flow-er", None)],
    "G": [("goat", "goat", None), ("girl", "girl", None),
          ("guitar", "gui-tar", None), ("grapes", "grapes", None),
          ("goose", "goose", None)],
    "H": [("hat", "hat", None), ("horse", "horse", None),
          ("house", "house", None), ("hand", "hand", None),
          ("honey", "hon-ey", None)],
    "I": [("igloo", "ig-loo", None), ("insect", "in-sect", None),
          ("ink", "ink", None), ("lips", "lips", None),
          ("fig", "fig", None)],
    "J": [("jam", "jam", None), ("jet", "jet", None),
          ("jug", "jug", None), ("jacket", "jack-et", None),
          ("jellyfish", "jel-ly-fish", None)],
    "K": [("kite", "kite", None), ("key", "key", None),
          ("king", "king", None), ("koala", "ko-a-la", None),
          ("kangaroo", "kan-ga-roo", None)],
    "L": [("lion", "li-on", None), ("leaf", "leaf", None),
          ("lamp", "lamp", None), ("lemon", "lem-on", None),
          ("ladder", "lad-der", None)],
    "M": [("moon", "moon", None), ("mouse", "mouse", None),
          ("milk", "milk", None), ("mountain", "moun-tain", None),
          ("mushroom", "mush-room", None)],
    "N": [("nest", "nest", None), ("nose", "nose", None),
          ("net", "net", None), ("nurse", "nurse", None),
          ("noodle", "noo-dle", None)],
    "O": [("octopus", "oc-to-pus", None), ("otter", "ot-ter", None),
          ("olive", "ol-ive", None), ("ostrich", "os-trich", None),
          ("ox", "ox", None)],
    "P": [("pig", "pig", None), ("pen", "pen", None),
          ("pizza", "piz-za", None), ("panda", "pan-da", None),
          ("pumpkin", "pump-kin", None)],
    # The /kw/ sound comes from the digraph, so both letters are highlighted.
    "Q": [("queen", "queen", "qu"), ("quilt", "quilt", "qu"),
          ("quail", "quail", "qu"), ("quarter", "quar-ter", "qu"),
          ("question", "ques-tion", "qu")],
    "R": [("rain", "rain", None), ("rabbit", "rab-bit", None),
          ("robot", "ro-bot", None), ("ring", "ring", None),
          ("rocket", "rock-et", None)],
    "S": [("sun", "sun", None), ("snake", "snake", None),
          ("star", "star", None), ("sock", "sock", None),
          ("seal", "seal", None)],
    "T": [("tiger", "ti-ger", None), ("tree", "tree", None),
          ("train", "train", None), ("tooth", "tooth", None),
          ("turtle", "tur-tle", None)],
    "U": [("umbrella", "um-brel-la", None), ("umpire", "um-pire", None),
          ("uncle", "un-cle", None), ("bus", "bus", None),
          ("nut", "nut", None)],
    "V": [("van", "van", None), ("vest", "vest", None),
          ("violin", "vi-o-lin", None), ("volcano", "vol-ca-no", None),
          ("vase", "vase", None)],
    "W": [("water", "wa-ter", None), ("wagon", "wag-on", None),
          ("window", "win-dow", None), ("worm", "worm", None),
          ("wolf", "wolf", None)],
    # X almost never starts a word with its /ks/ sound, so these are medial
    # and final -- which is exactly where children meet it.
    "X": [("box", "box", None), ("fox", "fox", None),
          ("axe", "axe", None), ("exit", "ex-it", None),
          ("six", "six", None)],
    "Y": [("yarn", "yarn", None), ("yellow", "yel-low", None),
          ("yoyo", "yo-yo", None), ("yogurt", "yo-gurt", None),
          ("yak", "yak", None)],
    "Z": [("zebra", "ze-bra", None), ("zipper", "zip-per", None),
          ("zoo", "zoo", None), ("zero", "ze-ro", None),
          ("zigzag", "zig-zag", None)],
}

LETTERS = [chr(ord("A") + i) for i in range(26)]


def grapheme_span(letter, word, override):
    """Return (start, length) of the span to render uppercase.

    Raises if the target letter is not in the word at all, which would mean a
    typo in the table above rather than something to paper over at runtime.
    """
    needle = (override or letter).lower()
    start = word.lower().find(needle)
    if start < 0:
        raise ValueError(
            f"letter {letter!r}: grapheme {needle!r} not found in {word!r}"
        )
    return start, len(needle)


def display_form(word, start, length):
    """'apple' + span(1,2) -> 'aPPle'. The taught letters are uppercase."""
    return word[:start] + word[start:start + length].upper() + word[start + length:]


def narration(letter, word, syllables):
    """Spoken script for the card.

    Template: name the sound, repeat it, sound out the syllables, then say the
    whole word. Kept as one f-string so the whole deck's phrasing can be
    retuned in a single place.
    """
    sound = SOUNDS[letter]
    parts = syllables.split("-")
    head = f"{letter} makes the {sound} sound. {sound}, {sound}. "
    if len(parts) == 1:
        # Single-syllable word: sounding it out would just say it twice.
        return head + f"{word}."
    return head + ", ".join(parts) + f". {word}."


def build_cards():
    """Flatten the table into a list of fully-derived card dicts."""
    cards = []
    for letter in LETTERS:
        entries = WORDS.get(letter, [])
        if len(entries) < 5:
            raise ValueError(f"letter {letter} has only {len(entries)} words")
        for word, syllables, override in entries:
            start, length = grapheme_span(letter, word, override)
            cards.append({
                "id": f"{letter.lower()}_{word}",
                "letter": letter,
                "word": word,
                "display": display_form(word, start, length),
                "span": [start, length],
                "syllables": syllables,
                "sound": SOUNDS[letter],
                "narration": narration(letter, word, syllables),
                "query": f"{word} {IMAGE_STYLE_HINT}",
                "image": f"cards/{letter.lower()}/{word}.png",
                "audio": f"cards/{letter.lower()}/{word}.wav",
            })
    return cards


def assert_unique_words():
    """No word may appear twice: each card needs its own picture."""
    seen = {}
    for letter, entries in WORDS.items():
        for word, _, _ in entries:
            if word in seen:
                raise ValueError(
                    f"word {word!r} used by both {seen[word]} and {letter}"
                )
            seen[word] = letter
    return len(seen)


if __name__ == "__main__":
    total = assert_unique_words()
    cards = build_cards()
    print(f"{len(cards)} cards, {total} unique words, {len(LETTERS)} letters")
    print()
    for c in cards[:3] + cards[80:83]:
        print(f"  {c['letter']}  {c['display']:<12} span={c['span']}")
        print(f"     narration: {c['narration']}")
