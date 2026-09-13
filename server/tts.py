import os
import subprocess
import logging

import numpy as np
import soxr

logger = logging.getLogger(__name__)

PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-lessac-medium")
# Piper medium voices are 22050 Hz; the device expects 16 kHz mono.
PIPER_RATE = int(os.environ.get("PIPER_RATE", "22050"))
TARGET_RATE = 16000


def synthesize(text: str) -> bytes:
    """Synthesize speech with Piper; returns 16 kHz mono 16-bit PCM bytes."""
    logger.info(f"Synthesizing: {text}")
    try:
        cmd = ["piper", "--model", PIPER_VOICE, "--output_raw"]
        process = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        stdout, stderr = process.communicate(input=text.encode("utf-8"))

        if process.returncode != 0:
            logger.error(f"Piper error: {stderr.decode('utf-8', errors='replace')}")
            return b""

        if PIPER_RATE != TARGET_RATE:
            audio = np.frombuffer(stdout, dtype=np.int16).astype(np.float32)
            if audio.size:
                audio = soxr.resample(audio, PIPER_RATE, TARGET_RATE)
            stdout = np.clip(audio, -32768, 32767).astype(np.int16).tobytes()

        return stdout
    except Exception as e:
        logger.error(f"Piper TTS synthesis failed: {e}")
        return b""
