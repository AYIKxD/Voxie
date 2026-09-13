import os
import subprocess
import logging

logger = logging.getLogger(__name__)

PIPER_VOICE = os.environ.get("PIPER_VOICE", "en_US-lessac-medium")

def synthesize(text: str) -> bytes:
    """
    Synthesizes speech using Piper TTS.
    Returns 16kHz mono 16-bit PCM bytes.
    """
    logger.info(f"Synthesizing: {text}")
    try:
        # Assuming piper is installed and in PATH, and the voice model is present
        cmd = [
            "piper", 
            "--model", PIPER_VOICE,
            "--output_raw"
        ]
        
        process = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        
        stdout, stderr = process.communicate(input=text.encode('utf-8'))
        
        if process.returncode != 0:
            logger.error(f"Piper error: {stderr.decode('utf-8')}")
            return b""
            
        return stdout
    except Exception as e:
        logger.error(f"Error in Piper TTS synthesis: {e}")
        return b""
