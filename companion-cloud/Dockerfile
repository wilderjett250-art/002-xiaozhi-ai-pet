FROM python:3.11-slim

ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1

WORKDIR /app

RUN apt-get update \
    && apt-get install -y --no-install-recommends libgomp1 libopus0 \
    && rm -rf /var/lib/apt/lists/*

COPY requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt

COPY scripts/download_speech_models.py ./scripts/download_speech_models.py
RUN python scripts/download_speech_models.py --target /app/models

COPY app.py ./
COPY ambient_audio.py ambient_decision.py speech_engine.py ./
COPY static ./static
COPY scripts ./scripts

RUN mkdir -p /app/data \
    && useradd --system --uid 10001 --home-dir /app petcloud \
    && chown -R petcloud:petcloud /app/data

USER petcloud
EXPOSE 8765

CMD ["uvicorn", "app:app", "--host", "0.0.0.0", "--port", "8765"]
