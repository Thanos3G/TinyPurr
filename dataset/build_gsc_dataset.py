"""
Build a yes / no / happy / background dataset for Edge Impulse.

    python dataset/build_gsc_dataset.py

Everything comes from Google Speech Commands. 

Output is the layout edge-impulse-uploader wants:

    gsc_set/training/<label>.<name>.wav
    gsc_set/testing/<label>.<name>.wav
"""

import glob
import os
import random
import wave

SR = 16000
WIN = 16000                     # exactly 1 s; short clips are zero-padded

GSC = "gsc"
OUT = "gsc_set"

KEYWORDS = ["yes", "no", "happy"]   
    "yes": 2500,
    "no": 2500,
    "happy": 2500,              # only ~2054 exist; all of them are used
    "background": 6000,         # spread across the remaining words
}
NOISE_CLIPS = 600               # 1 s chunks cut from _background_noise_

random.seed(7)


def read_wav(path):
    try:
        with wave.open(path, "rb") as w:
            if w.getframerate() != SR or w.getnchannels() != 1:
                return None
            raw = w.readframes(w.getnframes())
    except Exception:
        return None
    return raw


def write_wav(path, raw):
    # Pad or trim to one second.
    need = WIN * 2
    if len(raw) < need:
        raw = raw + b"\x00" * (need - len(raw))
    elif len(raw) > need:
        raw = raw[:need]
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(raw)


def main():
    for split in ("training", "testing"):
        os.makedirs(f"{OUT}/{split}", exist_ok=True)

    # Speech Commands' test split.
    test_set = set()
    with open(f"{GSC}/testing_list.txt", encoding="utf-8") as f:
        for line in f:
            test_set.add(line.strip())

    counts = {}

    def emit(label, path, split, tag):
        raw = read_wav(path)
        if raw is None:
            return False
        name = f"{label}.{tag}.wav"
        write_wav(f"{OUT}/{split}/{name}", raw)
        counts[(label, split)] = counts.get((label, split), 0) + 1
        return True

    # --- the keywords -------------------------------------------------
    for kw in KEYWORDS:
        files = sorted(glob.glob(f"{GSC}/{kw}/*.wav"))
        random.shuffle(files)
        ntr = 0
        for p in files:
            rel = f"{kw}/{os.path.basename(p)}"
            split = "testing" if rel in test_set else "training"
            if split == "training":
                if ntr >= CAP[kw]:
                    continue
                ntr += 1
            emit(kw, p, split, os.path.basename(p)[:-4].replace("_", ""))

    # --- background, every other word--------------------------
    words = [d for d in sorted(os.listdir(GSC))
             if os.path.isdir(f"{GSC}/{d}") and not d.startswith("_")
             and d not in KEYWORDS]
    per_word = max(1, CAP["background"] // max(1, len(words)))
    print(f"background drawn from {len(words)} words, ~{per_word} each")
    for w in words:
        files = sorted(glob.glob(f"{GSC}/{w}/*.wav"))
        random.shuffle(files)
        n = 0
        for p in files:
            if n >= per_word:
                break
            rel = f"{w}/{os.path.basename(p)}"
            split = "testing" if rel in test_set else "training"
            if emit("background", p, split, f"{w}{n:04d}"):
                n += 1

    # --- background, real noise --------------------
    noise_files = sorted(glob.glob(f"{GSC}/_background_noise_/*.wav"))
    made = 0
    for p in noise_files:
        raw = read_wav(p)
        if raw is None:
            continue
        total = len(raw) // 2
        step = WIN // 2      # 50% overlap:
        for off in range(0, total - WIN, step):
            if made >= NOISE_CLIPS:
                break
            chunk = raw[off * 2:(off + WIN) * 2]
            split = "testing" if (made % 9 == 8) else "training"
            name = f"background.noise{made:04d}.wav"
            write_wav(f"{OUT}/{split}/{name}", chunk)
            counts[("background", split)] = counts.get(("background", split), 0) + 1
            made += 1
    print(f"{made} noise clips cut from {len(noise_files)} recordings")

    print()
    for label in KEYWORDS + ["background"]:
        tr = counts.get((label, "training"), 0)
        te = counts.get((label, "testing"), 0)
        print(f"  {label:11s} {tr:5d} training  {te:4d} testing")
    print(f"\n{os.path.abspath(OUT)}")


if __name__ == "__main__":
    main()
