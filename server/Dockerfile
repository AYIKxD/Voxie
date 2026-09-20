FROM python:3.11-slim

RUN apt-get update && apt-get install -y \
    libopus-dev \
    ffmpeg \
    wget \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY requirements.txt .

RUN pip install --no-cache-dir -r requirements.txt

# Copy server code
COPY . .

# Expose port
EXPOSE 8000

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8000"]
