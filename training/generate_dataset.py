#!/usr/bin/env python3
"""
generate_dataset.py — Generate synthetic wake-word training data using Piper TTS.

Produces positive samples of the wake word spoken by different synthetic voices
with varied pitch/speed, plus negative samples from common words.

Usage:
    python generate_dataset.py --wake-word "Hey Voxie" --output-dir data/
"""
import argparse
import glob
import os
import random
import shutil
import subprocess
import sys
import tempfile


def find_piper() -> str:
    """Locate the Piper TTS executable (venv first, then PATH)."""
    exe = "piper.exe" if os.name == "nt" else "piper"
    candidate = os.path.join(os.path.dirname(sys.executable), exe)
    if os.path.exists(candidate):
        return candidate
    found = shutil.which("piper")
    if found:
        return found
    print("ERROR: 'piper' not found. Install with: pip install piper-tts")
    sys.exit(1)


def find_voices(voices_dir: str):
    voices = sorted(glob.glob(os.path.join(voices_dir, "*.onnx")))
    if not voices:
        print(f"ERROR: no Piper voices (*.onnx) found in {voices_dir}")
        print("Download e.g. en_US-lessac-medium.onnx from "
              "https://huggingface.co/rhasspy/piper-voices")
        sys.exit(1)
    return voices


def synthesize(piper: str, model: str, text: str, output_file: str,
               length_scale: float, noise_scale: float, noise_w: float) -> bool:
    # Passing text via --input-file is more reliable than stdin on Windows,
    # where piper occasionally deadlocks waiting on the pipe.
    fd, text_file = tempfile.mkstemp(suffix=".txt")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write(text + "\n")

        cmd = [
            piper,
            "--model", model,
            "--input-file", text_file,
            "--output_file", output_file,
            "--length-scale", f"{length_scale:.3f}",
            "--noise-scale", f"{noise_scale:.3f}",
            "--noise-w-scale", f"{noise_w:.3f}",
        ]
        proc = subprocess.run(cmd, capture_output=True, timeout=120)
        if proc.returncode != 0:
            print(f"  Warning: piper failed: {proc.stderr.decode(errors='replace').strip()}")
            return False
        return os.path.exists(output_file) and os.path.getsize(output_file) > 0
    except subprocess.TimeoutExpired:
        print("  Warning: piper timed out")
        return False
    finally:
        try:
            os.remove(text_file)
        except OSError:
            pass


def generate_positive_samples(wake_word, voices, piper, output_dir,
                              num_samples):
    pos_dir = os.path.join(output_dir, "positive")
    os.makedirs(pos_dir, exist_ok=True)

    # Phonetic spellings of the wake word help cover pronunciation variety.
    variants = [
        wake_word,
        wake_word.lower(),
        wake_word.replace("Voxie", "Voxi"),
        wake_word.replace("Voxie", "Voxee"),
    ]

    print(f"Generating {num_samples} positive samples for: '{wake_word}'")
    for i in range(num_samples):
        voice = random.choice(voices)
        text = random.choice(variants)
        output_file = os.path.join(pos_dir, f"pos_{i:05d}.wav")

        length_scale = random.uniform(0.85, 1.15)   # slower < 1 < faster
        noise_scale = random.uniform(0.4, 0.8)
        noise_w = random.uniform(0.6, 1.0)

        if synthesize(piper, voice, text, output_file,
                      length_scale, noise_scale, noise_w):
            if (i + 1) % 50 == 0:
                print(f"  Generated {i + 1}/{num_samples}")

    print(f"Done. Positive samples in {pos_dir}")


def generate_negative_samples(voices, piper, output_dir, num_samples):
    neg_dir = os.path.join(output_dir, "negative")
    os.makedirs(neg_dir, exist_ok=True)

    # Common words / phonetically-similar phrases that must NOT trigger.
    negative_phrases = [
        "hey there", "how are you", "good morning", "excuse me",
        "okay fine", "hey buddy", "what's up", "hello world",
        "hey siri", "hey google", "alexa", "computer",
        "hey foxy", "hey proxy", "hey boxy", "hey rocksy",   # similar
        "the vox", "a voice", "invoking", "provoking",
        "turn on the lamp", "what's the weather", "play some music",
        "set a timer", "volume up", "volume down", "stop", "next song",
    ]

    print(f"Generating {num_samples} negative samples")
    for idx in range(num_samples):
        voice = random.choice(voices)
        phrase = random.choice(negative_phrases)
        output_file = os.path.join(neg_dir, f"neg_{idx:05d}.wav")
        length_scale = random.uniform(0.9, 1.1)
        synthesize(piper, voice, phrase, output_file,
                   length_scale, 0.667, 0.8)
        if (idx + 1) % 50 == 0:
            print(f"  Generated {idx + 1}/{num_samples}")

    print(f"Done. Negative samples in {neg_dir}")


def main():
    parser = argparse.ArgumentParser(description="Generate wake-word dataset")
    parser.add_argument("--wake-word", default="Hey Voxie", help="Wake word phrase")
    parser.add_argument("--output-dir", default="data", help="Output directory")
    parser.add_argument("--voices-dir", default="voices",
                        help="Directory containing Piper *.onnx voices")
    parser.add_argument("--num-positive", type=int, default=500,
                        help="Number of positive samples")
    parser.add_argument("--num-negative", type=int, default=200,
                        help="Number of negative samples")
    parser.add_argument("--seed", type=int, default=42)
    args = parser.parse_args()

    random.seed(args.seed)
    piper = find_piper()
    voices = find_voices(args.voices_dir)
    print(f"Using piper: {piper}")
    print(f"Voices: {[os.path.basename(v) for v in voices]}")

    os.makedirs(args.output_dir, exist_ok=True)
    generate_positive_samples(args.wake_word, voices, piper,
                              args.output_dir, args.num_positive)
    generate_negative_samples(voices, piper, args.output_dir, args.num_negative)
    print(f"\nDataset ready in {args.output_dir}/")
    print("Next: python train.py --data-dir", args.output_dir)


if __name__ == "__main__":
    main()
