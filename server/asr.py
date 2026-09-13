import os
import time
import logging
from faster_whisper import WhisperModel
import io
import wave

logger = logging.getLogger(__name__)

WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "base")
logger.info(f"Loading Whisper model: {WHISPER_MODEL}")
model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type="int8")
logger.info("Whisper model loaded.")

def transcribe(pcm_audio: bytes, sample_rate: int) -> str:
    """
    Transcribes PCM audio bytes using faster-whisper.
    """
    # faster-whisper can take a file path or a binary stream/array.
    # To keep things simple, we create an in-memory wav file and pass it.
    wav_io = io.BytesIO()
    with wave.open(wav_io, 'wb') as wav_file:
        wav_file.setnchannels(1)
        wav_file.setsampwidth(2) # 16-bit
        wav_file.setframerate(sample_rate)
        wav_file.writeframes(pcm_audio)
    
    wav_io.seek(0)
    
    t0 = time.time()
    segments, info = model.transcribe(wav_io, beam_size=5)
    
    transcript = " ".join([segment.text for segment in segments])
    logger.info(f"Transcription took {time.time() - t0:.3f}s")
    return transcript.strip()
