#!/usr/bin/env python3
"""hades reference embedder — a warm subprocess speaking the hades embedding line protocol.

Protocol (one JSON line in, one JSON line out, repeated for the process lifetime):
  in : {"texts": ["t1", "t2", ...]}
  out: {"model": "<name>", "dim": <int>, "embeddings": [[...], [...]]}
  err: {"error": "<msg>"}

Setup:  pip install sentence-transformers
Manifest: Embedding { provider = subprocess ; command = python3 /abs/path/tools/embed_reference.py }
  (NB: the manifest is one-key-per-line — write the block multi-line, see manifests/dev.hades.)

Alternative LOCAL backends (recommended — no Python dep, OpenAI-compatible HTTP, provider = http):
  (a) ollama:    `ollama pull nomic-embed-text` then run ollama; set
        Embedding { provider = http ; endpoint = http://localhost:11434/v1 ; model = nomic-embed-text }
  (b) llama.cpp: `llama-server --embedding -m <gguf>` exposes an OpenAI-compat /embeddings; set
        Embedding { provider = http ; endpoint = http://localhost:8080/v1 ; model = <name> }
  Either serves the same line protocol's HTTP sibling — hades' HttpEmbeddingProvider POSTs /embeddings.
"""
import sys, json
from sentence_transformers import SentenceTransformer   # loaded ONCE (warm)

MODEL_NAME = "all-MiniLM-L6-v2"   # 384-dim, small, CPU-fast
model = SentenceTransformer(MODEL_NAME)
DIM = model.get_sentence_embedding_dimension()

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    try:
        req = json.loads(line)
        texts = req.get("texts", [])
        vecs = model.encode(texts, normalize_embeddings=False).tolist() if texts else []
        out = {"model": MODEL_NAME, "dim": DIM, "embeddings": vecs}
    except Exception as e:
        out = {"error": str(e)}
    sys.stdout.write(json.dumps(out) + "\n")
    sys.stdout.flush()   # CRITICAL: flush so hades reads the reply
