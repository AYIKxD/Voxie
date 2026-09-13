#!/usr/bin/env python3
"""
train.py — Train the wake-word model using the microWakeWord pipeline.

Wraps the microWakeWord training framework to produce a TFLite model
from the augmented dataset.

Usage:
    python train.py --data-dir data_augmented/ --output-dir output/
"""
import argparse
import os
import sys


def main():
    parser = argparse.ArgumentParser(description="Train wake-word model")
    parser.add_argument("--data-dir", required=True, help="Augmented data directory")
    parser.add_argument("--output-dir", default="output", help="Model output directory")
    parser.add_argument("--epochs", type=int, default=50, help="Training epochs")
    parser.add_argument("--batch-size", type=int, default=64, help="Batch size")
    args = parser.parse_args()

    os.makedirs(args.output_dir, exist_ok=True)

    print("=" * 60)
    print("microWakeWord Training Pipeline")
    print("=" * 60)
    print(f"Data directory:  {args.data_dir}")
    print(f"Output directory: {args.output_dir}")
    print(f"Epochs:          {args.epochs}")
    print(f"Batch size:      {args.batch_size}")
    print()

    # Check for microWakeWord installation
    try:
        import microwakeword
        print(f"microWakeWord version: {microwakeword.__version__}")
    except ImportError:
        print("ERROR: microWakeWord not installed.")
        print("Install from: https://github.com/OHF-Voice/micro-wake-word")
        print("  pip install microwakeword")
        print()
        print("Alternatively, follow the microWakeWord training notebook:")
        print("  https://github.com/OHF-Voice/micro-wake-word/blob/main/notebooks/")
        sys.exit(1)

    # The actual training uses microWakeWord's pipeline
    # This is a wrapper that sets up the right parameters
    print("Training model...")
    print("  Using streaming architecture (Google's Keyword Spotting paper)")
    print("  Feature extractor: 40-value mel filterbank, 20ms stride")
    print("  Quantization: int8 (for TFLite Micro)")
    print()

    # TODO: Call microWakeWord's training API
    # The exact API depends on the version installed.
    # See: https://github.com/OHF-Voice/micro-wake-word/blob/main/microwakeword/
    #
    # Typical flow:
    #   1. Load positive/negative WAV files
    #   2. Extract features (mel spectrograms)
    #   3. Train streaming CNN/CRNN
    #   4. Quantize to int8
    #   5. Export as .tflite

    print("=" * 60)
    print("TODO: Wire microWakeWord training API")
    print("For manual training, use the microWakeWord notebook directly.")
    print("=" * 60)
    print()
    print("After training, export with:")
    print(f"  python export_tflite.py --model-dir {args.output_dir} --output model.tflite")


if __name__ == "__main__":
    main()
