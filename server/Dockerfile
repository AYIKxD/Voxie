FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    libopus-dev \
    ffmpeg \
    wget \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Download a piper binary (mock path for simplicity)
RUN wget -qO piper.tar.gz https://github.com/rhasspy/piper/releases/download/v1.2.0/piper_linux_x86_64.tar.gz \
    && tar -xzf piper.tar.gz -C /usr/local/bin \
    && rm piper.tar.gz

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Copy server code
COPY . .

# Expose port
EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
