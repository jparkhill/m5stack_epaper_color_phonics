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
# The sound each letter makes, as espeak inline phoneme markup.
#
# Plain spellings do NOT work and fail SILENTLY. Verified against piper's own
# phonemizer:
#     "fff" -> /ɛf ɛf ɛf/  espeak says the NAME "eff" three times
#     "eh"  -> /eɪ/        "ay", not short-e
#     "ih"  -> /aɪ/        "eye", not short-i
#     "ks"  -> /keɪ ɛs/    "kay-ess"
# and a bare vowel letter is always read as that letter's name. espeak's
# [[...]] markup passes phonemes through verbatim, which is the only way to
# get an isolated short vowel.
#
# Stops get a "-uh" syllable (buh, kuh, duh) because a plosive with no
# following vowel is essentially inaudible -- also what phonics programmes
# teach. Continuants and vowels are the pure phoneme.
SOUNDS = {
    "A": "[[æ]]",   "B": "[[bʌ]]",  "C": "[[kʌ]]",  "D": "[[dʌ]]",
    "E": "[[ɛ]]",   "F": "[[f]]",   "G": "[[ɡʌ]]",  "H": "[[hʌ]]",
    "I": "[[ɪ]]",   "J": "[[dʒʌ]]", "K": "[[kʌ]]",  "L": "[[l]]",
    "M": "[[m]]",   "N": "[[n]]",   "O": "[[ɑː]]",  "P": "[[pʌ]]",
    "Q": "[[kwʌ]]", "R": "[[ɹ]]",   "S": "[[s]]",   "T": "[[tʌ]]",
    "U": "[[ʌ]]",   "V": "[[v]]",   "W": "[[wʌ]]",  "X": "[[ks]]",
    "Y": "[[jʌ]]",  "Z": "[[z]]",
}

# Human-readable form of each sound, for logs, docs and the manifest.
# Never fed to the TTS.
SOUND_LABELS = {
    "A": "a",   "B": "buh", "C": "kuh", "D": "duh", "E": "e",   "F": "fff",
    "G": "guh", "H": "huh", "I": "i",   "J": "juh", "K": "kuh", "L": "lll",
    "M": "mmm", "N": "nnn", "O": "o",   "P": "puh", "Q": "kwuh","R": "rrr",
    "S": "sss", "T": "tuh", "U": "u",   "V": "vvv", "W": "wuh", "X": "ks",
    "Y": "yuh", "Z": "zzz",
}

# How the letter's NAME is spoken, as opposed to the sound it makes.
#
# Piper phonemises a bare capital letter unreliably -- "C" comes out as the
# /k/ sound rather than the name "see", which defeats the whole point of the
# sentence "C makes the kuh sound". These are explicit spellings so the voice
# says the name.
#
# Z is "zee" to match the American voice (en_US-libritts-high). Change it to
# "zed" here if you would rather have the British name.
LETTER_NAMES = {
    "A": "eigh",  "B": "bee",   "C": "see",   "D": "dee",   "E": "ee",
    "F": "ef",    "G": "jee",   "H": "aitch", "I": "eye",   "J": "jay",
    "K": "kay",   "L": "ell",   "M": "em",    "N": "en",    "O": "oh",
    "P": "pee",   "Q": "cue",   "R": "ar",    "S": "ess",   "T": "tee",
    "U": "you",   "V": "vee",   "W": "double you",          "X": "ex",
    "Y": "why",   "Z": "zee",
}

# Extra hint appended to the image search to bias towards flat, high-contrast
# cartoon art, which is what survives a 6-colour e-paper panel.
IMAGE_STYLE_HINT = "cartoon clipart for kids simple white background"

WORDS = {
    "A": [("apple", "ap-ple", None), ("ant", "ant", None),
          ("anchor", "an-chor", None), ("astronaut", "as-tro-naut", None),
          ("alligator", "al-li-ga-tor", None),
          ("arrow", "ar-row", None),
          ("album", "al-bum", None),
          ("antler", "ant-ler", None),
          ("ambulance", "am-bu-lance", None),
          ("avocado", "av-o-ca-do", None),],
    "B": [("ball", "ball", None), ("bear", "bear", None),
          ("bike", "bike", None), ("boat", "boat", None),
          ("bug", "bug", None),
          ("bell", "bell", None),
          ("bird", "bird", None),
          ("book", "book", None),
          ("banana", "ba-na-na", None),
          ("butterfly", "but-ter-fly", None),],
    "C": [("cat", "cat", None), ("cow", "cow", None),
          ("cup", "cup", None), ("car", "car", None),
          ("cake", "cake", None),
          ("candle", "can-dle", None),
          ("camel", "cam-el", None),
          ("carrot", "car-rot", None),
          ("corn", "corn", None),
          ("castle", "cas-tle", None),],
    "D": [("dog", "dog", None), ("duck", "duck", None),
          ("door", "door", None), ("drum", "drum", None),
          ("dolphin", "dol-phin", None),
          ("desk", "desk", None),
          ("dice", "dice", None),
          ("deer", "deer", None),
          ("donut", "do-nut", None),
          ("dragon", "dra-gon", None),],
    "E": [("egg", "egg", None), ("elephant", "el-e-phant", None),
          ("bed", "bed", None), ("hen", "hen", None),
          ("web", "web", None),
          ("elbow", "el-bow", None),
          ("envelope", "en-ve-lope", None),
          ("engine", "en-gine", None),
          ("elk", "elk", None),
          ("shell", "shell", None),],
    "F": [("fish", "fish", None), ("frog", "frog", None),
          ("fan", "fan", None), ("foot", "foot", None),
          ("flower", "flow-er", None),
          ("fire", "fire", None),
          ("fork", "fork", None),
          ("farm", "farm", None),
          ("feather", "fea-ther", None),
          ("football", "foot-ball", None),],
    "G": [("goat", "goat", None), ("girl", "girl", None),
          ("guitar", "gui-tar", None), ("grapes", "grapes", None),
          ("goose", "goose", None),
          ("gift", "gift", None),
          ("glove", "glove", None),
          ("garden", "gar-den", None),
          ("gum", "gum", None),
          ("grass", "grass", None),],
    "H": [("hat", "hat", None), ("horse", "horse", None),
          ("house", "house", None), ("hand", "hand", None),
          ("honey", "hon-ey", None),
          ("hammer", "ham-mer", None),
          ("heart", "heart", None),
          ("helmet", "hel-met", None),
          ("hill", "hill", None),
          ("hippo", "hip-po", None),],
    "I": [("igloo", "ig-loo", None), ("insect", "in-sect", None),
          ("ink", "ink", None), ("lips", "lips", None),
          ("fig", "fig", None),
          ("iguana", "i-gua-na", None),
          ("inchworm", "inch-worm", None),
          ("pin", "pin", None),
          ("ship", "ship", None),
          ("zip", "zip", None),],
    "J": [("jam", "jam", None), ("jet", "jet", None),
          ("jug", "jug", None), ("jacket", "jack-et", None),
          ("jellyfish", "jel-ly-fish", None),
          ("jeep", "jeep", None),
          ("jigsaw", "jig-saw", None),
          ("juice", "juice", None),
          ("jungle", "jun-gle", None),
          ("jar", "jar", None),],
    "K": [("kite", "kite", None), ("key", "key", None),
          ("king", "king", None), ("koala", "ko-a-la", None),
          ("kangaroo", "kan-ga-roo", None),
          ("kitten", "kit-ten", None),
          ("kettle", "ket-tle", None),
          ("ketchup", "ketch-up", None),
          ("kiwi", "ki-wi", None),
          ("kayak", "kay-ak", None),],
    "L": [("lion", "li-on", None), ("leaf", "leaf", None),
          ("lamp", "lamp", None), ("lemon", "lem-on", None),
          ("ladder", "lad-der", None),
          ("lock", "lock", None),
          ("log", "log", None),
          ("ladybug", "la-dy-bug", None),
          ("lettuce", "let-tuce", None),
          ("lizard", "liz-ard", None),],
    "M": [("moon", "moon", None), ("mouse", "mouse", None),
          ("milk", "milk", None), ("mountain", "moun-tain", None),
          ("mushroom", "mush-room", None),
          ("map", "map", None),
          ("mask", "mask", None),
          ("monkey", "mon-key", None),
          ("muffin", "muf-fin", None),
          ("magnet", "mag-net", None),],
    "N": [("nest", "nest", None), ("nose", "nose", None),
          ("net", "net", None), ("nurse", "nurse", None),
          ("noodle", "noo-dle", None),
          ("nail", "nail", None),
          ("night", "night", None),
          ("nine", "nine", None),
          ("notebook", "note-book", None),
          ("necklace", "neck-lace", None),],
    "O": [("octopus", "oc-to-pus", None), ("otter", "ot-ter", None),
          ("olive", "ol-ive", None), ("ostrich", "os-trich", None),
          ("ox", "ox", None),
          ("octagon", "oc-ta-gon", None),
          ("pot", "pot", None),
          ("rock", "rock", None),
          ("mop", "mop", None),
          ("top", "top", None),],
    "P": [("pig", "pig", None), ("pen", "pen", None),
          ("pizza", "piz-za", None), ("panda", "pan-da", None),
          ("pumpkin", "pump-kin", None),
          ("pencil", "pen-cil", None),
          ("present", "pre-sent", None),
          ("puzzle", "puz-zle", None),
          ("popcorn", "pop-corn", None),
          ("penguin", "pen-guin", None),],
    # The /kw/ sound comes from the digraph, so both letters are highlighted.
    "Q": [("queen", "queen", "qu"), ("quilt", "quilt", "qu"),
          ("quail", "quail", "qu"), ("quarter", "quar-ter", "qu"),
          ("question", "ques-tion", "qu"),
          ("quiz", "quiz", None),
          ("quill", "quill", None),
          ("quiver", "qui-ver", None),
          ("quicksand", "quick-sand", None),
          ("quokka", "quok-ka", None),],
    "R": [("rain", "rain", None), ("rabbit", "rab-bit", None),
          ("robot", "ro-bot", None), ("ring", "ring", None),
          ("rocket", "rock-et", None),
          ("rope", "rope", None),
          ("ruler", "ru-ler", None),
          ("rose", "rose", None),
          ("rainbow", "rain-bow", None),
          ("radish", "ra-dish", None),],
    "S": [("sun", "sun", None), ("snake", "snake", None),
          ("star", "star", None), ("sock", "sock", None),
          ("seal", "seal", None),
          ("soap", "soap", None),
          ("spoon", "spoon", None),
          ("sandwich", "sand-wich", None),
          ("scissors", "scis-sors", None),
          ("strawberry", "straw-ber-ry", None),],
    "T": [("tiger", "ti-ger", None), ("tree", "tree", None),
          ("train", "train", None), ("tooth", "tooth", None),
          ("turtle", "tur-tle", None),
          ("table", "ta-ble", None),
          ("tent", "tent", None),
          ("towel", "tow-el", None),
          ("truck", "truck", None),
          ("teapot", "tea-pot", None),],
    "U": [("umbrella", "um-brel-la", None), ("umpire", "um-pire", None),
          ("uncle", "un-cle", None), ("bus", "bus", None),
          ("nut", "nut", None),
          ("mud", "mud", None),
          ("tub", "tub", None),
          ("plug", "plug", None),
          ("brush", "brush", None),
          ("skunk", "skunk", None),],
    "V": [("van", "van", None), ("vest", "vest", None),
          ("violin", "vi-o-lin", None), ("volcano", "vol-ca-no", None),
          ("vase", "vase", None),
          ("violet", "vi-o-let", None),
          ("vulture", "vul-ture", None),
          ("valley", "val-ley", None),
          ("vacuum", "va-cuum", None),
          ("volleyball", "vol-ley-ball", None),],
    "W": [("water", "wa-ter", None), ("wagon", "wag-on", None),
          ("window", "win-dow", None), ("worm", "worm", None),
          ("wolf", "wolf", None),
          ("watermelon", "wa-ter-mel-on", None),
          ("whale", "whale", None),
          ("wheel", "wheel", None),
          ("witch", "witch", None),
          ("wing", "wing", None),],
    # X almost never starts a word with its /ks/ sound, so these are medial
    # and final -- which is exactly where children meet it.
    # xylophone is deliberately ABSENT: its X makes a /z/ sound, which would
    # teach the wrong thing on a card whose whole purpose is /ks/.
    "X": [("box", "box", None), ("fox", "fox", None),
          ("axe", "axe", None), ("exit", "ex-it", None),
          ("six", "six", None),
          ("fix", "fix", None),
          ("wax", "wax", None),
          ("mix", "mix", None),
          ("taxi", "tax-i", None),
          ("saxophone", "sax-o-phone", None),],
    "Y": [("yarn", "yarn", None), ("yellow", "yel-low", None),
          ("yoyo", "yo-yo", None), ("yogurt", "yo-gurt", None),
          ("yak", "yak", None),
          ("yolk", "yolk", None),
          ("yawn", "yawn", None),
          ("yard", "yard", None),
          ("yeti", "ye-ti", None),
          ("yam", "yam", None),],
    "Z": [("zebra", "ze-bra", None), ("zipper", "zip-per", None),
          ("zoo", "zoo", None), ("zero", "ze-ro", None),
          ("zigzag", "zig-zag", None),
          ("zucchini", "zuc-chi-ni", None),
          ("zombie", "zom-bie", None),
          ("zeppelin", "zep-pe-lin", None),
          ("zinnia", "zin-ni-a", None),
          ("zither", "zith-er", None),],
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


def letter_narration(letter):
    """Spoken script for a LETTER, independent of any word.

    Split from the word half on purpose. The device lets you step through
    every letter of the displayed word, so a single clip per (word, letter)
    pair would need ~1,400 recordings. Splitting it needs 26 + one per word,
    and the two clips are simply played back to back.
    """
    name = LETTER_NAMES[letter]
    sound = SOUNDS[letter]
    return f"{name} makes the {sound} sound. {sound}, {sound}."


def word_narration(word, syllables):
    """Spoken script for a WORD: sound it out, then say it."""
    parts = syllables.split("-")
    if len(parts) == 1:
        # Sounding out a one-syllable word would just say it twice.
        return f"{word}."
    return ", ".join(parts) + f". {word}."


def build_letters():
    """The 26 letter clips: one per letter, reused by every word."""
    return [{
        "letter": L,
        "name": LETTER_NAMES[L],
        "sound": SOUND_LABELS[L],
        "narration": letter_narration(L),
        "audio": f"letters/{L.lower()}.wav",
    } for L in LETTERS]


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
                "sound": SOUND_LABELS[letter],
                "narration": word_narration(word, syllables),
                "query": f"{word} {IMAGE_STYLE_HINT}",
                "image": f"cards/{letter.lower()}/{word}.png",
                "audio": f"cards/{letter.lower()}/{word}.wav",
            })
    return cards


def assert_letter_names():
    """Every letter needs a spoken name, or the narration says the wrong thing."""
    missing = [l for l in LETTERS if l not in LETTER_NAMES]
    if missing:
        raise ValueError(f"LETTER_NAMES missing: {missing}")
    return len(LETTER_NAMES)


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
    assert_letter_names()
    total = assert_unique_words()
    cards = build_cards()
    print(f"{len(cards)} cards, {total} unique words, {len(LETTERS)} letters")
    print()
    print("letter clips (26):")
    for l in build_letters()[:3]:
        print(f"  {l['letter']}  {l['narration']}")
    print("\nword clips:")
    for c in cards[:2] + cards[160:162]:
        print(f"  {c['letter']}  {c['display']:<12} span={c['span']}  \"{c['narration']}\"")
