FROM python:3.12-slim

WORKDIR /app

COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY . .

# Containerising ingestion means a clean checkout needs only Docker — no host
# Python, no pip install — to produce the corpus that everything else consumes.
#
# Compose mounts ingest/data at /data, so the default output lands on the host.
ENTRYPOINT ["python", "hn_fetch.py"]
CMD ["--target", "1000", "-o", "/data/corpus.sample.jsonl"]
