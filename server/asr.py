import os
import time
import logging
import io
import wave

logger = logging.getLogger(__name__)

WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "base")
# Pin language to avoid Whisper guessing wrong language on noisy audio.
# Set to None to auto-detect (slower, unreliable with mic noise).
WHISPER_LANGUAGE = os.environ.get("WHISPER_LANGUAGE", "en")

_model = None


def _get_model():
    """Lazily load the Whisper model (avoids blocking server startup)."""
    global _model
    if _model is None:
        from faster_whisper import WhisperModel

        logger.info(f"Loading Whisper model: {WHISPER_MODEL}")
        _model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
        logger.info("Whisper model loaded.")
    return _model


def transcribe(pcm_audio: bytes, sample_rate: int) -> str:
    """
    Transcribes PCM audio bytes using faster-whisper.
    """
    model = _get_model()

    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2)  # 16-bit
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_audio)

    wav_io.seek(0)

    t0 = time.time()
    segments, info = model.transcribe(
        wav_io,
        beam_size=1,
        language=WHISPER_LANGUAGE,          # pin language — skip auto-detect
        no_speech_threshold=0.6,            # suppress empty/noise-only segments
        condition_on_previous_text=False,   # don't hallucinate based on prior
        vad_filter=True,                    # skip silent frames before decoding
        vad_parameters={"min_silence_duration_ms": 300},
    )

    transcript = " ".join([segment.text for segment in segments])
    logger.info(f"Transcription took {time.time() - t0:.3f}s")
    return transcript.strip()
