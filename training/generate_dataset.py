#!/usr/bin/env python3
"""
generate_dataset.py — Generate synthetic wake-word training data using Piper TTS.

Produces hundreds of positive samples of the wake word spoken by different synthetic
voices with varied pitch/speed, plus negative samples from common words.

Usage:
    python generate_dataset.py --wake-word "Hey Voxie" --output-dir data/
"""
import argparse
import os
import subprocess
import random

def generate_positive_samples(wake_word: str, output_dir: str, num_samples: int = 500):
    """Generate synthetic positive samples using Piper TTS with varied voices."""
    pos_dir = os.path.join(output_dir, "positive")
    os.makedirs(pos_dir, exist_ok=True)

    # Piper voice models to use (download separately)
    voices = [
        "en_US-lessac-medium",
        "en_US-amy-medium",
        "en_US-ryan-medium",
        "en_GB-alba-medium",
    ]

    print(f"Generating {num_samples} positive samples for: '{wake_word}'")

    for i in range(num_samples):
        voice = random.choice(voices)
        # Vary speed slightly (0.8x to 1.2x)
        speed = random.uniform(0.8, 1.2)
        output_file = os.path.join(pos_dir, f"pos_{i:04d}.wav")

        try:
            # Use Piper TTS to synthesize
            cmd = [
                "piper",
                "--model", voice,
                "--output_file", output_file,
                "--length-scale", str(1.0 / speed),
            ]
            proc = subprocess.run(
                cmd, input=wake_word, text=True,
                capture_output=True, timeout=30
            )
            if proc.returncode == 0:
                if (i + 1) % 50 == 0:
                    print(f"  Generated {i + 1}/{num_samples}")
            else:
                print(f"  Warning: Piper failed for sample {i}: {proc.stderr}")
        except FileNotFoundError:
            print("ERROR: 'piper' not found. Install piper-tts or add to PATH.")
            print("       pip install piper-tts")
            return
        except Exception as e:
            print(f"  Error generating sample {i}: {e}")

    print(f"Done. {num_samples} positive samples in {pos_dir}")


def generate_negative_samples(output_dir: str, num_samples: int = 200):
    """Generate negative samples from common words that should NOT trigger."""
    neg_dir = os.path.join(output_dir, "negative")
    os.makedirs(neg_dir, exist_ok=True)

    # Common words and phrases that might cause false positives
    negative_phrases = [
        "hey there", "how are you", "good morning", "excuse me",
        "okay fine", "hey buddy", "what's up", "hello world",
        "hey siri", "hey google", "alexa", "computer",
        "hey foxy", "hey proxy", "hey boxy",  # phonetically similar
        "the vox", "a voice", "invoking", "provoking",
    ]

    print(f"Generating {num_samples} negative samples")

    voices = ["en_US-lessac-medium", "en_US-amy-medium"]
    idx = 0
    while idx < num_samples:
        phrase = random.choice(negative_phrases)
        voice = random.choice(voices)
        speed = random.uniform(0.85, 1.15)
        output_file = os.path.join(neg_dir, f"neg_{idx:04d}.wav")

        try:
            cmd = [
                "piper",
                "--model", voice,
                "--output_file", output_file,
                "--length-scale", str(1.0 / speed),
            ]
            subprocess.run(cmd, input=phrase, text=True, capture_output=True, timeout=30)
            idx += 1
        except Exception:
            idx += 1  # Skip on error

    print(f"Done. {num_samples} negative samples in {neg_dir}")


def main():
    parser = argparse.ArgumentParser(description="Generate wake-word training dataset")
    parser.add_argument("--wake-word", default="Hey Voxie", help="Wake word phrase")
    parser.add_argument("--output-dir", default="data", help="Output directory")
    parser.add_argument("--num-positive", type=int, default=500, help="Number of positive samples")
    parser.add_argument("--num-negative", type=int, default=200, help="Number of negative samples")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)
    generate_positive_samples(args.wake_word, args.output_dir, args.num_positive)
    generate_negative_samples(args.output_dir, args.num_negative)
    print(f"\nDataset ready in {args.output_dir}/")
    print("Next steps:")
    print("  1. python augment.py --input-dir data/ --output-dir data_augmented/")
    print("  2. python train.py --data-dir data_augmented/")


if __name__ == "__main__":
    main()
