#!/usr/bin/env python3
"""
train.py — Train the wake-word model using the microWakeWord pipeline.

Builds 40-value mel-filterbank spectrogram features from the generated dataset,
trains a small streaming MixConv network with microWakeWord, quantizes it to
int8, and copies the resulting .tflite into the firmware.

Usage:
    python train.py --data-dir data/ --output-dir output/ --steps 300
"""
import argparse
import os
import shutil
import subprocess
import sys

import numpy as np
import yaml

TARGET_WAKE_WORD = "Hey Voxie"


def make_background_assets(assets_dir: str, seed: int = 1234):
    """Generate small synthetic background-noise and RIR WAV files.

    This keeps the reduced training run self-contained (no multi-GB dataset
    downloads) while still exercising the background-noise and reverb
    augmentations that microWakeWord relies on for robustness.
    """
    import soundfile as sf

    rng = np.random.default_rng(seed)
    noise_dir = os.path.join(assets_dir, "noise")
    rir_dir = os.path.join(assets_dir, "rir")
    os.makedirs(noise_dir, exist_ok=True)
    os.makedirs(rir_dir, exist_ok=True)

    sr = 16000
    if not os.listdir(noise_dir):
        for i in range(8):
            seconds = 8.0
            n = int(sr * seconds)
            # Mix of white and low-frequency "hum" noise.
            white = rng.normal(0, 0.05, n).astype(np.float32)
            hum = 0.03 * np.sin(2 * np.pi * rng.uniform(50, 400) *
                                np.arange(n) / sr).astype(np.float32)
            noise = np.clip(white + hum, -1, 1)
            sf.write(os.path.join(noise_dir, f"noise_{i:02d}.wav"), noise, sr)

    if not os.listdir(rir_dir):
        for i in range(4):
            n = int(sr * 0.6)
            decay = np.exp(-np.arange(n) / (0.05 * sr))
            ir = (rng.normal(0, 1, n) * decay).astype(np.float32)
            ir[0] = 1.0  # direct path
            ir /= np.max(np.abs(ir)) + 1e-9
            sf.write(os.path.join(rir_dir, f"rir_{i:02d}.wav"), ir, sr)

    return noise_dir, rir_dir


def build_feature_mmaps(clips_dir, out_dir, augmenter, slide_frames,
                        step_ms, seed, split_count, split_map):
    """Generate RaggedMmap spectrogram features for one clip set."""
    from microwakeword.audio.clips import Clips
    from microwakeword.audio.spectrograms import SpectrogramGeneration
    from mmap_ninja.ragged import RaggedMmap

    if not os.path.isdir(clips_dir) or not any(
        f.endswith(".wav") for f in os.listdir(clips_dir)
    ):
        raise SystemExit(f"No .wav clips found in {clips_dir}")

    clips = Clips(
        input_directory=clips_dir,
        file_pattern="*.wav",
        max_clip_duration_s=None,
        remove_silence=False,
        random_split_seed=seed,
        split_count=split_count,
    )

    for set_name, clips_split in split_map.items():
        set_dir = os.path.join(out_dir, set_name)
        os.makedirs(set_dir, exist_ok=True)
        mmap_path = os.path.join(set_dir, "features_mmap")
        if os.path.isdir(mmap_path):
            shutil.rmtree(mmap_path)

        slide = slide_frames.get(set_name, 1)
        spectrograms = SpectrogramGeneration(
            clips=clips,
            augmenter=augmenter,
            slide_frames=slide,
            step_ms=step_ms,
        )

        RaggedMmap.from_generator(
            out_dir=mmap_path,
            sample_generator=spectrograms.spectrogram_generator(
                split=clips_split, repeat=1),
            batch_size=50,
            verbose=False,
        )
        count = len(RaggedMmap(mmap_path))
        print(f"  {set_name}/{os.path.basename(clips_split)}: {count} spectrograms")


def build_ambient_mmaps(noise_dir, out_dir, step_ms):
    """Build long-duration ambient spectrograms for false-accepts/hour metrics."""
    from microwakeword.audio.clips import Clips
    from microwakeword.audio.spectrograms import SpectrogramGeneration
    from mmap_ninja.ragged import RaggedMmap

    clips = Clips(
        input_directory=noise_dir,
        file_pattern="*.wav",
        random_split_seed=7,
        split_count=0.25,
    )
    spectrograms = SpectrogramGeneration(clips=clips, augmenter=None,
                                         step_ms=step_ms)
    for set_name, clips_split in [("validation_ambient", "validation"),
                                  ("testing_ambient", "test")]:
        set_dir = os.path.join(out_dir, set_name)
        os.makedirs(set_dir, exist_ok=True)
        mmap_path = os.path.join(set_dir, "ambient_mmap")
        if os.path.isdir(mmap_path):
            shutil.rmtree(mmap_path)
        RaggedMmap.from_generator(
            out_dir=mmap_path,
            sample_generator=spectrograms.spectrogram_generator(
                split=clips_split, repeat=1),
            batch_size=10,
            verbose=False,
        )
        print(f"  {set_name}: {len(RaggedMmap(mmap_path))} spectrograms")


def main():
    parser = argparse.ArgumentParser(description="Train wake-word model")
    parser.add_argument("--data-dir", default="data", help="Raw dataset dir")
    parser.add_argument("--output-dir", default="output", help="Model output dir")
    parser.add_argument("--steps", type=int, default=300, help="Training steps")
    parser.add_argument("--batch-size", type=int, default=32, help="Batch size")
    parser.add_argument("--seed", type=int, default=10)
    parser.add_argument("--step-ms", type=int, default=10,
                        help="Spectrogram window step (ms)")
    parser.add_argument("--firmware-model",
                        default=os.path.join("..", "firmware", "main", "kws",
                                             "model_data.tflite"),
                        help="Where to copy the exported .tflite")
    parser.add_argument("--skip-features", action="store_true",
                        help="Reuse existing feature mmaps")
    args = parser.parse_args()

    try:
        import microwakeword  # noqa: F401
    except ImportError:
        print("ERROR: microWakeWord not installed.")
        print("  pip install -r requirements.txt")
        sys.exit(1)

    data_dir = os.path.abspath(args.data_dir)
    output_dir = os.path.abspath(args.output_dir)
    os.makedirs(output_dir, exist_ok=True)

    print("=" * 64)
    print("microWakeWord training pipeline")
    print("=" * 64)
    print(f"Wake word:   {TARGET_WAKE_WORD}")
    print(f"Data dir:    {data_dir}")
    print(f"Output dir:  {output_dir}")
    print(f"Steps:       {args.steps}  Batch: {args.batch_size}")
    print()

    features_dir = os.path.join(output_dir, "features")
    assets_dir = os.path.join(output_dir, "assets")

    if not args.skip_features:
        from microwakeword.audio.augmentation import Augmentation

        noise_dir, rir_dir = make_background_assets(assets_dir, args.seed)

        pos_augmenter = Augmentation(
            augmentation_duration_s=1.6,
            augmentation_probabilities={
                "SevenBandParametricEQ": 0.1,
                "TanhDistortion": 0.1,
                "PitchShift": 0.0,
                "BandStopFilter": 0.1,
                "AddColorNoise": 0.25,
                "AddBackgroundNoise": 0.5,
                "Gain": 1.0,
                "GainTransition": 0.25,
                "RIR": 0.4,
            },
            impulse_paths=[rir_dir],
            background_paths=[noise_dir],
            background_min_snr_db=-5,
            background_max_snr_db=10,
            min_jitter_s=0.05,
            max_jitter_s=0.15,
        )
        neg_augmenter = Augmentation(
            augmentation_duration_s=1.6,
            augmentation_probabilities={
                "SevenBandParametricEQ": 0.1,
                "TanhDistortion": 0.1,
                "PitchShift": 0.0,
                "BandStopFilter": 0.1,
                "AddColorNoise": 0.25,
                "AddBackgroundNoise": 0.5,
                "Gain": 1.0,
                "GainTransition": 0.25,
                "RIR": 0.4,
            },
            impulse_paths=[rir_dir],
            background_paths=[noise_dir],
            background_min_snr_db=-5,
            background_max_snr_db=10,
        )

        print("[1/3] Building positive spectrograms...")
        build_feature_mmaps(
            os.path.join(data_dir, "positive"),
            os.path.join(features_dir, "positive"),
            pos_augmenter,
            slide_frames={"training": 3, "validation": 3, "testing": 1},
            step_ms=args.step_ms,
            seed=args.seed,
            split_count=0.1,
            split_map={"training": "train", "validation": "validation",
                       "testing": "test"},
        )

        print("[2/3] Building negative spectrograms...")
        build_feature_mmaps(
            os.path.join(data_dir, "negative"),
            os.path.join(features_dir, "negative"),
            neg_augmenter,
            slide_frames={"training": 1, "validation": 1, "testing": 1},
            step_ms=args.step_ms,
            seed=args.seed,
            split_count=0.1,
            split_map={"training": "train", "validation": "validation",
                       "testing": "test"},
        )

        print("[3/3] Building ambient spectrograms...")
        build_ambient_mmaps(noise_dir,
                            os.path.join(features_dir, "ambient"), args.step_ms)
        print()

    train_dir = os.path.join(output_dir, "trained_models", "wakeword")
    config = {
        "window_step_ms": args.step_ms,
        "train_dir": train_dir,
        "features": [
            {
                "features_dir": os.path.join(features_dir, "positive"),
                "sampling_weight": 2.0,
                "penalty_weight": 1.0,
                "truth": True,
                "truncation_strategy": "truncate_start",
                "type": "mmap",
            },
            {
                "features_dir": os.path.join(features_dir, "negative"),
                "sampling_weight": 10.0,
                "penalty_weight": 1.0,
                "truth": False,
                "truncation_strategy": "random",
                "type": "mmap",
            },
            {
                "features_dir": os.path.join(features_dir, "ambient"),
                "sampling_weight": 0.0,
                "penalty_weight": 1.0,
                "truth": False,
                "truncation_strategy": "split",
                "type": "mmap",
            },
        ],
        "training_steps": [args.steps],
        "positive_class_weight": [1],
        "negative_class_weight": [20],
        "learning_rates": [0.001],
        "batch_size": args.batch_size,
        "time_mask_max_size": [0],
        "time_mask_count": [0],
        "freq_mask_max_size": [0],
        "freq_mask_count": [0],
        "mix_up_augmentation_prob": [0.0],
        "freq_mix_augmentation_prob": [0.0],
        "eval_step_interval": max(10, args.steps // 10),
        "clip_duration_ms": 1500,
        "target_minimization": 0.9,
        "minimization_metric": None,
        "maximization_metric": "recall",
    }

    config_path = os.path.join(output_dir, "training_parameters.yaml")
    with open(config_path, "w") as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)
    print(f"Wrote training config: {config_path}")

    cmd = [
        sys.executable, "-m", "microwakeword.model_train_eval",
        "--training_config", config_path,
        "--train", "1",
        "--restore_checkpoint", "1",
        "--test_tf_nonstreaming", "0",
        "--test_tflite_nonstreaming", "0",
        "--test_tflite_nonstreaming_quantized", "0",
        "--test_tflite_streaming", "0",
        "--test_tflite_streaming_quantized", "1",
        "--use_weights", "best_weights",
        "mixednet",
        "--pointwise_filters", "32,64",
        "--repeat_in_block", "1,1",
        "--mixconv_kernel_sizes", "[5], [7, 11]",
        "--residual_connection", "0,0",
        "--first_conv_filters", "16",
        "--first_conv_kernel_size", "5",
        "--stride", "3",
    ]
    print("\nRunning microWakeWord training...")
    print(" ".join(cmd))
    result = subprocess.run(cmd, cwd=output_dir)
    if result.returncode != 0:
        print(f"\nERROR: training failed (exit {result.returncode})")
        sys.exit(result.returncode)

    tflite = os.path.join(
        train_dir, "tflite_stream_state_internal_quant",
        "stream_state_internal_quant.tflite")
    if not os.path.exists(tflite):
        print(f"\nERROR: expected model not found: {tflite}")
        sys.exit(1)

    fw_model = os.path.abspath(args.firmware_model)
    os.makedirs(os.path.dirname(fw_model), exist_ok=True)
    shutil.copyfile(tflite, fw_model)
    size_kb = os.path.getsize(tflite) / 1024
    print("\n" + "=" * 64)
    print(f"Trained model: {tflite} ({size_kb:.1f} KB)")
    print(f"Copied to firmware: {fw_model}")
    print("Rebuild the firmware with: idf.py build")


if __name__ == "__main__":
    main()
