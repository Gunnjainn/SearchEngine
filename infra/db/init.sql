-- Search Engine — database bootstrap
-- Runs once on first container start (mounted into /docker-entrypoint-initdb.d/).

CREATE EXTENSION IF NOT EXISTS vector;

-- Core document metadata (mirrors Contract 1 fields minus `text`,
-- which lives in the engine's inverted index).
CREATE TABLE IF NOT EXISTS documents (
    doc_id   BIGINT PRIMARY KEY,
    title    TEXT   NOT NULL,
    url      TEXT   NOT NULL
);

-- Dense vector embeddings for semantic / hybrid search.
-- TODO(phase-4): Set the embedding dimension once the model is chosen,
--                e.g.  embedding vector(768)  for a 768-d model.
CREATE TABLE IF NOT EXISTS doc_vectors (
    doc_id    BIGINT NOT NULL REFERENCES documents(doc_id) ON DELETE CASCADE,
    embedding vector,  -- dimension TBD; see TODO above
    PRIMARY KEY (doc_id)
);
