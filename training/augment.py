#!/usr/bin/env python3
"""
augment.py — Apply audio augmentation to the training dataset.

Augmentations:
  - Background noise mixing (MUSAN or custom ambient recordings)
  - Room impulse response (RIR) convolution for reverb simulation
  - Speed/pitch perturbation
  - Volume variation

Usage:
    python augment.py --input-dir data/ --output-dir data_augmented/
"""
import argparse
import os
import random
import numpy as np

try:
    import soundfile as sf
except ImportError:
    sf = None
    print("Warning: soundfile not installed. pip install soundfile")

try:
    from scipy.signal import fftconvolve
except ImportError:
    fftconvolve = None
    print("Warning: scipy not installed. pip install scipy")


def load_wav(path: str) -> tuple:
    """Load a WAV file, return (samples, sample_rate)."""
    if sf is None:
        raise ImportError("soundfile required: pip install soundfile")
    data, sr = sf.read(path, dtype='float32')
    if len(data.shape) > 1:
        data = data[:, 0]  # mono
    return data, sr


def save_wav(path: str, data: np.ndarray, sr: int):
    """Save audio samples to WAV."""
    sf.write(path, data, sr)


def add_noise(audio: np.ndarray, noise_dir: str, snr_db: float = None) -> np.ndarray:
    """Mix background noise at a random SNR (5-20 dB)."""
    if snr_db is None:
        snr_db = random.uniform(5, 20)

    noise_files = [f for f in os.listdir(noise_dir) if f.endswith('.wav')]
    if not noise_files:
        return audio

    noise_path = os.path.join(noise_dir, random.choice(noise_files))
    noise, _ = load_wav(noise_path)

    # Loop noise if shorter than audio
    while len(noise) < len(audio):
        noise = np.concatenate([noise, noise])
    noise = noise[:len(audio)]

    # Scale noise to desired SNR
    audio_power = np.mean(audio ** 2) + 1e-10
    noise_power = np.mean(noise ** 2) + 1e-10
    snr_linear = 10 ** (snr_db / 10)
    noise_scale = np.sqrt(audio_power / (noise_power * snr_linear))

    return audio + noise * noise_scale


def apply_rir(audio: np.ndarray, rir_dir: str) -> np.ndarray:
    """Apply room impulse response convolution."""
    if fftconvolve is None:
        return audio

    rir_files = [f for f in os.listdir(rir_dir) if f.endswith('.wav')]
    if not rir_files:
        return audio

    rir_path = os.path.join(rir_dir, random.choice(rir_files))
    rir, _ = load_wav(rir_path)

    # Normalize RIR
    rir = rir / (np.max(np.abs(rir)) + 1e-10)

    result = fftconvolve(audio, rir, mode='full')[:len(audio)]
    return result / (np.max(np.abs(result)) + 1e-10)


def speed_perturb(audio: np.ndarray, sr: int) -> np.ndarray:
    """Apply random speed perturbation (0.9x to 1.1x)."""
    speed = random.uniform(0.9, 1.1)
    indices = np.round(np.arange(0, len(audio), speed)).astype(int)
    indices = indices[indices < len(audio)]
    return audio[indices]


def volume_perturb(audio: np.ndarray) -> np.ndarray:
    """Apply random volume scaling (0.5x to 1.5x)."""
    gain = random.uniform(0.5, 1.5)
    return np.clip(audio * gain, -1.0, 1.0)


def augment_file(input_path: str, output_dir: str, noise_dir: str = None,
                  rir_dir: str = None, num_augmentations: int = 3):
    """Generate augmented versions of a single audio file."""
    audio, sr = load_wav(input_path)
    basename = os.path.splitext(os.path.basename(input_path))[0]

    for i in range(num_augmentations):
        aug = audio.copy()

        # Randomly apply augmentations
        if random.random() < 0.5:
            aug = speed_perturb(aug, sr)

        if random.random() < 0.7:
            aug = volume_perturb(aug)

        if noise_dir and os.path.isdir(noise_dir) and random.random() < 0.6:
            aug = add_noise(aug, noise_dir)

        if rir_dir and os.path.isdir(rir_dir) and random.random() < 0.3:
            aug = apply_rir(aug, rir_dir)

        output_path = os.path.join(output_dir, f"{basename}_aug{i}.wav")
        save_wav(output_path, aug, sr)


def main():
    parser = argparse.ArgumentParser(description="Augment wake-word training data")
    parser.add_argument("--input-dir", required=True, help="Input data directory")
    parser.add_argument("--output-dir", required=True, help="Output augmented directory")
    parser.add_argument("--noise-dir", default=None, help="Background noise WAV directory")
    parser.add_argument("--rir-dir", default=None, help="Room impulse response WAV directory")
    parser.add_argument("--augmentations-per-file", type=int, default=3)
    args = parser.parse_args()

    for split in ["positive", "negative"]:
        in_dir = os.path.join(args.input_dir, split)
        out_dir = os.path.join(args.output_dir, split)
        os.makedirs(out_dir, exist_ok=True)

        if not os.path.isdir(in_dir):
            print(f"Skipping {split} (not found: {in_dir})")
            continue

        wav_files = [f for f in os.listdir(in_dir) if f.endswith('.wav')]
        print(f"Augmenting {len(wav_files)} {split} files ({args.augmentations_per_file}x each)")

        for j, fname in enumerate(wav_files):
            augment_file(
                os.path.join(in_dir, fname), out_dir,
                noise_dir=args.noise_dir, rir_dir=args.rir_dir,
                num_augmentations=args.augmentations_per_file
            )
            if (j + 1) % 100 == 0:
                print(f"  Processed {j + 1}/{len(wav_files)}")

    print(f"\nAugmented data in {args.output_dir}/")
    print("Next: python train.py --data-dir", args.output_dir)


if __name__ == "__main__":
    main()
