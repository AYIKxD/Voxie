import torch
import logging

logger = logging.getLogger(__name__)

logger.info("Loading Silero VAD model...")
model, utils = torch.hub.load(repo_or_dir='snakers4/silero-vad',
                              model='silero_vad',
                              force_reload=False,
                              onnx=False)
(get_speech_timestamps, save_audio, read_audio, VADIterator, collect_chunks) = utils
logger.info("Silero VAD model loaded.")

def detect_end_of_speech(pcm_audio: bytes, sample_rate: int = 16000, silence_threshold_ms: int = 700) -> int:
    """
    Returns the byte offset where speech ends, applying a silence threshold.
    """
    import numpy as np
    
    # Convert bytes to int16 numpy array, then to float32 tensor
    audio_np = np.frombuffer(pcm_audio, dtype=np.int16).astype(np.float32) / 32768.0
    audio_tensor = torch.from_numpy(audio_np)
    
    speech_timestamps = get_speech_timestamps(audio_tensor, model, sampling_rate=sample_rate)
    
    if not speech_timestamps:
        return 0
        
    last_speech_sample = speech_timestamps[-1]['end']
    
    # Add silence threshold
    threshold_samples = int((silence_threshold_ms / 1000.0) * sample_rate)
    end_sample = min(last_speech_sample + threshold_samples, len(audio_tensor))
    
    return end_sample * 2 # 2 bytes per sample (16-bit)
