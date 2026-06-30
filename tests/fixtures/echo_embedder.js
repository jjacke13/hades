#!/usr/bin/env node
// Test fixture: a warm embedder honoring the hades line protocol (one JSON line in, one out),
// in node (nodejs is in the nix dev shell; python3 is not). Deterministic, no ML deps.
// in : {"texts": ["t1", ...]}   out: {"model","dim","embeddings":[[...],...]}   err: {"error":"..."}
const DIM = 3;
function embed(text) {                       // deterministic pseudo-embedding from char codes
  const v = [0.0, 0.0, 0.0];
  for (let i = 0; i < text.length; i++) v[i % DIM] += (text.charCodeAt(i) % 17) / 17.0;
  return v;
}
const rl = require('readline').createInterface({ input: process.stdin });
rl.on('line', (line) => {
  line = line.trim();
  if (!line) return;
  let out;
  try {
    const req = JSON.parse(line);
    const texts = Array.isArray(req.texts) ? req.texts : [];
    out = { model: "echo", dim: DIM, embeddings: texts.map(embed) };
  } catch (e) {                              // protocol error -> one error line
    out = { error: String(e) };
  }
  process.stdout.write(JSON.stringify(out) + "\n");   // pipe write flushes (libuv)
});
