#!/usr/bin/env python3
"""
export_tflite.py — Export trained model to quantized TFLite format.

Takes a trained Keras/TensorFlow model and converts it to an int8 quantized
TFLite model suitable for ESP32-S3 deployment via TFLite Micro.

Usage:
    python export_tflite.py --model-dir output/ --output model.tflite
"""
import argparse
import os
import sys
import numpy as np


def main():
    parser = argparse.ArgumentParser(description="Export model to TFLite")
    parser.add_argument("--model-dir", required=True, help="Directory with trained model")
    parser.add_argument("--output", default="model.tflite", help="Output .tflite path")
    parser.add_argument("--quantize", choices=["int8", "float16", "none"], default="int8")
    args = parser.parse_args()

    try:
        import tensorflow as tf
    except ImportError:
        print("ERROR: TensorFlow not installed. pip install tensorflow")
        sys.exit(1)

    # Find the saved model or .h5 file
    model_path = None
    for name in ["saved_model", "model.h5", "model.keras"]:
        candidate = os.path.join(args.model_dir, name)
        if os.path.exists(candidate):
            model_path = candidate
            break

    if model_path is None:
        print(f"ERROR: No model found in {args.model_dir}")
        print("Expected: saved_model/ directory, model.h5, or model.keras")
        sys.exit(1)

    print(f"Loading model from: {model_path}")
    if os.path.isdir(model_path):
        model = tf.saved_model.load(model_path)
        converter = tf.lite.TFLiteConverter.from_saved_model(model_path)
    else:
        model = tf.keras.models.load_model(model_path)
        converter = tf.lite.TFLiteConverter.from_keras_model(model)

    # Quantization
    if args.quantize == "int8":
        print("Applying int8 quantization (full integer)")
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
        converter.inference_input_type = tf.int8
        converter.inference_output_type = tf.int8

        # Representative dataset for calibration
        def representative_dataset():
            for _ in range(100):
                # Generate random input matching model's expected shape
                yield [np.random.randn(1, 40).astype(np.float32)]

        converter.representative_dataset = representative_dataset
        print("  Note: using random calibration data. For best quality,")
        print("  replace representative_dataset with actual training features.")

    elif args.quantize == "float16":
        print("Applying float16 quantization")
        converter.optimizations = [tf.lite.Optimize.DEFAULT]
        converter.target_spec.supported_types = [tf.float16]

    tflite_model = converter.convert()

    # Save
    with open(args.output, 'wb') as f:
        f.write(tflite_model)

    size_kb = len(tflite_model) / 1024
    print(f"\nExported: {args.output} ({size_kb:.1f} KB)")
    print(f"\nTo embed in firmware:")
    print(f"  cp {args.output} ../firmware/main/kws/model_data.tflite")
    print(f"  # CMake's EMBED_FILES handles the rest")


if __name__ == "__main__":
    main()
