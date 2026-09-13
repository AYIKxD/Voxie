import logging

import opuslib

logger = logging.getLogger(__name__)

SAMPLE_RATE = 16000
CHANNELS = 1
FRAME_MS = 60
FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000  # 960


class OpusStreamDecoder:
    """Stateful Opus decoder for one audio stream (one packet per WS frame)."""

    def __init__(self, sample_rate: int = SAMPLE_RATE, channels: int = CHANNELS):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_samples = sample_rate * FRAME_MS // 1000
        self.decoder = opuslib.Decoder(sample_rate, channels)

    def decode(self, packet: bytes) -> bytes:
        """Decode one Opus packet to 16-bit PCM bytes."""
        try:
            return self.decoder.decode(packet, self.frame_samples)
        except opuslib.OpusError as e:
            logger.warning(f"Opus decode error: {e}")
            return b""

    def reset(self):
        try:
            self.decoder.reset_state()
        except Exception:
            pass


class OpusStreamEncoder:
    """Stateful Opus encoder for TTS output."""

    def __init__(self, sample_rate: int = SAMPLE_RATE, channels: int = CHANNELS,
                 bitrate: int = 16000):
        self.sample_rate = sample_rate
        self.channels = channels
        self.frame_samples = sample_rate * FRAME_MS // 1000
        self.frame_bytes = self.frame_samples * 2
        self.encoder = opuslib.Encoder(sample_rate, channels, opuslib.APPLICATION_VOIP)
        self.encoder.bitrate = bitrate

    def encode_pcm(self, pcm: bytes):
        """Split 16-bit PCM into a list of Opus packets."""
        packets = []
        for i in range(0, len(pcm), self.frame_bytes):
            chunk = pcm[i:i + self.frame_bytes]
            if len(chunk) < self.frame_bytes:
                chunk += b"\x00" * (self.frame_bytes - len(chunk))
            packets.append(self.encoder.encode(chunk, self.frame_samples))
        return packets
