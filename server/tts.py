"""
TTS module for Voxie.

Primary engine: Microsoft EdgeTTS (free, neural, 24kHz native).
Fallback engine: Piper (local, offline).

EdgeTTS produces dramatically better quality than Piper — natural prosody,
correct emphasis, and genuine 24kHz audio (no resampling artifacts).

Set TTS_ENGINE=piper in .env to force the local fallback.
Set EDGE_TTS_VOICE to change the voice (default: en-US-AriaNeural).
"""
import asyncio
import io
import logging
import os
import re
import subprocess

import numpy as np
import soxr

logger = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
TTS_ENGINE      = os.environ.get("TTS_ENGINE", "edge").lower()  # "edge" or "piper"
TARGET_RATE     = 24000          # Must match SPK_SAMPLE_RATE in firmware config.h

# EdgeTTS settings (primary)
EDGE_VOICE      = os.environ.get("EDGE_TTS_VOICE", "en-US-AriaNeural")
EDGE_RATE       = "+0%"          # speaking rate offset
EDGE_VOLUME     = "+0%"

# Piper settings (fallback)
PIPER_VOICE     = os.environ.get("PIPER_VOICE", "en_US-lessac-medium")
PIPER_RATE      = int(os.environ.get("PIPER_RATE", "22050"))


# ---------------------------------------------------------------------------
# EdgeTTS (primary)
# ---------------------------------------------------------------------------

def _edge_synthesize(text: str) -> bytes:
    """
    Synthesize via edge-tts (async under the hood, called synchronously here).
    Returns 24kHz 16-bit mono PCM bytes.
    edge-tts delivers MP3; we decode with ffmpeg to raw PCM.
    """
    try:
        import edge_tts  # pip install edge-tts
    except ImportError:
        logger.warning("edge-tts not installed, falling back to Piper")
        return _piper_synthesize(text)

    async def _run():
        communicate = edge_tts.Communicate(text, EDGE_VOICE,
                                           rate=EDGE_RATE, volume=EDGE_VOLUME)
        mp3_chunks = []
        async for chunk in communicate.stream():
            if chunk["type"] == "audio":
                mp3_chunks.append(chunk["data"])
        return b"".join(mp3_chunks)

    try:
        mp3_bytes = asyncio.run(_run())
    except RuntimeError:
        # Already inside an event loop (e.g. tests) — use a new thread
        import concurrent.futures
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            mp3_bytes = pool.submit(asyncio.run, _run()).result()

    if not mp3_bytes:
        logger.error("EdgeTTS returned empty audio")
        return b""

    # Decode MP3 → raw 16-bit PCM at TARGET_RATE via ffmpeg
    try:
        result = subprocess.run(
            [
                "ffmpeg", "-y",
                "-i", "pipe:0",
                "-f", "s16le",
                "-ar", str(TARGET_RATE),
                "-ac", "1",
                "pipe:1",
            ],
            input=mp3_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode != 0:
            logger.error(f"ffmpeg decode error: {result.stderr.decode(errors='replace')[:200]}")
            return b""
        return result.stdout
    except FileNotFoundError:
        logger.error("ffmpeg not found — install ffmpeg or set TTS_ENGINE=piper")
        return b""


# ---------------------------------------------------------------------------
# Piper (fallback / offline)
# ---------------------------------------------------------------------------

def _resample(pcm_bytes: bytes, src_rate: int, dst_rate: int) -> bytes:
    """Resample raw 16-bit mono PCM from src_rate to dst_rate."""
    if src_rate == dst_rate or not pcm_bytes:
        return pcm_bytes
    audio = np.frombuffer(pcm_bytes, dtype=np.int16).astype(np.float32)
    if audio.size == 0:
        return b""
    audio = soxr.resample(audio, src_rate, dst_rate)
    return np.clip(audio, -32768, 32767).astype(np.int16).tobytes()


def _piper_synthesize(text: str) -> bytes:
    """Synthesize with local Piper TTS; resamples to TARGET_RATE."""
    try:
        cmd = ["piper", "--model", PIPER_VOICE, "--output_raw"]
        proc = subprocess.Popen(
            cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        )
        stdout, stderr = proc.communicate(input=text.encode("utf-8"))
        if proc.returncode != 0:
            logger.error(f"Piper error: {stderr.decode('utf-8', errors='replace')}")
            return b""
        return _resample(stdout, PIPER_RATE, TARGET_RATE)
    except Exception as e:
        logger.error(f"Piper TTS failed: {e}")
        return b""


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def synthesize(text: str) -> bytes:
    """
    Synthesize speech for text.  Returns TARGET_RATE (24kHz) 16-bit mono PCM.
    Uses EdgeTTS by default; falls back to Piper when TTS_ENGINE=piper or
    if edge-tts / ffmpeg is unavailable.
    """
    logger.info(f"Synthesizing [{TTS_ENGINE}]: {text}")
    if TTS_ENGINE == "piper":
        return _piper_synthesize(text)
    return _edge_synthesize(text)


# ---------------------------------------------------------------------------
# Sentence splitter for streaming TTS
# ---------------------------------------------------------------------------

# Splits on sentence boundaries while keeping short fragments merged.
_SENTENCE_RE = re.compile(r'(?<=[.!?])\s+')
_MIN_CHUNK_LEN = 20


def split_sentences(text: str) -> list[str]:
    """Split text into sentence-sized chunks suitable for streaming TTS."""
    raw = _SENTENCE_RE.split(text.strip())
    if not raw:
        return []

    chunks: list[str] = []
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
