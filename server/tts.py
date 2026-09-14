import os
import subprocess
import logging
import re

import numpy as np
import soxr

logger = logging.getLogger(__name__)

PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-lessac-medium")
# Piper medium voices are 22050 Hz; the device expects 16 kHz mono.
PIPER_RATE = int(os.environ.get("PIPER_RATE", "22050"))
TARGET_RATE = 16000

# Persistent Piper process for lower latency (avoids respawning per sentence).
_piper_proc = None


def _get_piper_proc():
    """Return a long-lived Piper process that accepts text on stdin and emits
    raw PCM on stdout (one utterance per line)."""
    global _piper_proc
    if _piper_proc is None or _piper_proc.poll() is not None:
        cmd = ["piper", "--model", PIPER_VOICE, "--output_raw"]
        _piper_proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        logger.info("Started persistent Piper TTS process")
    return _piper_proc


def _resample(pcm_bytes: bytes) -> bytes:
    """Resample raw 16-bit PCM from PIPER_RATE to TARGET_RATE."""
    if PIPER_RATE == TARGET_RATE or not pcm_bytes:
        return pcm_bytes
    audio = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
    if audio.size == 0:
        return b""
    audio = soxr.resample(audio, PIPER_RATE, TARGET_RATE)
    return np.clip(audio, -32768, 32767).astype(np.int16).tobytes()


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

        return _resample(stdout)
    except Exception as e:
        logger.error(f"Piper TTS synthesis failed: {e}")
        return b""


# ---------------------------------------------------------------------------
# Sentence splitter for streaming TTS
# ---------------------------------------------------------------------------

# Splits on sentence boundaries (. ! ? followed by space/EOL) while keeping
# short fragments attached to the next sentence to avoid choppy delivery.
_SENTENCE_RE = re.compile(r'(?<=[.!?])\s+')
_MIN_CHUNK_LEN = 20  # merge tiny fragments with the next chunk


def split_sentences(text: str) -> list[str]:
    """Split text into sentence-sized chunks suitable for streaming TTS."""
    raw = _SENTENCE_RE.split(text.strip())
    if not raw:
        return []

    chunks = []
    buf = ""
    for part in raw:
        buf = (buf + " " + part).strip() if buf else part
        if len(buf) >= _MIN_CHUNK_LEN:
            chunks.append(buf)
            buf = ""
    if buf:
        if chunks:
            chunks[-1] = chunks[-1] + " " + buf
        else:
            chunks.append(buf)
    return chunks
