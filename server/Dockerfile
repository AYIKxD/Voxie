FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    libopus-dev \
    ffmpeg \
    wget \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .

# CPU-only PyTorch (needed by Silero VAD). Installing from the CPU index avoids
# pulling the multi-GB CUDA runtime packages from PyPI.
RUN pip install --no-cache-dir torch torchaudio --index-url https://download.pytorch.org/whl/cpu

RUN pip install --no-cache-dir -r requirements.txt

# Piper voice model (the `piper-tts` pip wheel is installed via requirements.txt)
RUN mkdir -p /app/voices \
    && wget -qO /app/voices/en_US-lessac-medium.onnx https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx \
    && wget -qO /app/voices/en_US-lessac-medium.onnx.json https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json

# Copy server code
COPY . .

# Expose port
EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
