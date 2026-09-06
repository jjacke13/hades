# One session per day — design (2026-09-06)

**Goal:** a hades session is a **day**, not a process launch. Restarting the binary mid-day rejoins
the same conversation; the session ends when the day does. One session spans every front-end —
terminal, SimpleX, Telegram, web, bridge — because they already share one Arbiter.

Today the session id is a launch timestamp (`20260906-221544`), so every restart starts a blank
conversation and `--resume` is a manual opt-in. That is the behaviour being replaced.

---

## The day boundary

A "day" runs from **`day_cutoff_hour`** to the same hour next day, local time, default **04:00**.
Midnight was rejected: work in progress at 23:58 would land in a new session two minutes later,
splitting one train of thought. 04:00 matches when a day actually ends for this operator.

**Logical date** of an instant `t` is the calendar date of `t − day_cutoff_hour`:

| local time | cutoff | logical date |
|---|---|---|
| 2026-09-06 03:59 | 4 | **2026-09-05** |
| 2026-09-06 04:00 | 4 | **2026-09-06** |
| 2026-09-06 23:30 | 4 | 2026-09-06 |
| 2026-09-07 02:00 | 4 | 2026-09-06 |
| any | 0 | plain calendar date (midnight behaviour, still available) |

Session id is that logical date: **`2026-09-06`**, file `.hades/sessions/2026-09-06.jsonl`.
Machine-local timezone, the same rule cron already follows — set the box's TZ deliberately.

## Boot

`hades <manifest>` with no flags now:

1. compute today's logical date → `id`
2. if `<dir>/<id>.jsonl` exists → **use it**: append to it, and load its history (the existing
   tolerant `load_history`, including its orphan-pair sanitising and the compaction sidecar)
3. otherwise create it

So a restart at 14:00 rejoins the morning's conversation with no flag. That is the whole feature.

`--resume <id>` keeps working and now takes a date (`--resume 2026-09-04`), still `MalConfig` if
absent. Bare `--resume` keeps meaning "newest file" — usually today's, so it becomes a no-op; kept
for compatibility rather than removed.

## Rollover while running

Checked **lazily at `start_turn`**, not on a timer: recompute the logical date, and if it differs
from the running session's, rotate first and then run the turn. No thread, no wakeup, and the check
is a clock read on a path that already does far more work.

Consequence, accepted: a long-running process that is *idle* across the cutoff rotates on its next
turn, not at 04:00 exactly. Nothing observes a session boundary except the next turn, so the
distinction is invisible. A turn that *starts* at 03:59 and finishes at 04:01 completes in the old
session — turns are never split.

Rotation does exactly what `NEW_SESSION` already does, plus the session path:

- clear `history_`, `summary_text_`, `summarized_upto_`, `pending_compact_`
- `++turn_epoch_` (a stale in-flight response cannot contaminate the new day)
- `clear_pending()`
- switch the append path to the new logical date's file
- post **`SESSION_ROTATED { from, to, path }`**

## Embeddings — the one non-obvious interaction

`EmbeddingMemoryModule` excludes the **live** session from its index, and that exclusion is
currently fixed at launch (`set_live_session_path` before `on_attach`, so the write
happens-before the index worker reads it). CLAUDE.md already records that `/new` does not re-point
it, and that this is accepted because the stale exclusion only risks indexing a file mid-write,
which is parser-safe and self-heals next launch.

Daily rotation makes that worse rather than equal: **after every rollover the exclusion still names
the file the process started on**, so the skip points at a finished session while the live one is not
skipped at all.

Severity, stated precisely (an earlier draft of this spec overstated it as "the corpus loses whole
days" — it does not): the start-day file is excluded only while that process runs and becomes
indexable again after the next restart, so it is DELAYED, not lost. The real cost is the other half —
**today's live session gets indexed while it is still being appended to**, so turns from the current
conversation come back injected as "excerpts from earlier sessions" (exactly what the
memory-injection framing work exists to prevent), alongside partial and duplicated units from reading
a file mid-write. Staleness and duplication, not data loss.

So rotation must re-point it, which makes the member cross-thread mutable for the first time.

**Both rotation triggers use the one mechanism.** `SESSION_ROTATED` is posted by the daily rollover
AND by `/new` (`NEW_SESSION`), and the embedding module subscribes once. Doing it only for the daily
path would leave two rotations with different behaviour for no reason: the guarding cost is paid the
moment ANY runtime write exists, so the second trigger is free. This **retires the existing
CLAUDE.md gotcha** that `/new` does not re-point the exclusion — that bug predates this feature and
is fixed by it, not worked around.
**`live_session_path_` becomes mutex-guarded** (a small dedicated mutex, or the module's existing
`index_mu_` — whichever the implementer finds cleaner): written by the pump thread on
`SESSION_ROTATED`, read by the index worker. The TSan lane is the gate on getting this right.

No immediate reindex is triggered at rollover — the existing periodic timer (`reindex_interval_s`,
default daily) picks up the closed day, which is what "as we have them now" means.

## `/new`

Kept, and now yields a **same-day suffixed** session: `2026-09-06-2`, `-3`, … via the existing
`unique_fresh_path`. So the rule is "one session per day *by default*", with a deliberate escape
hatch for a clean context mid-day. Everything else about `/new` is unchanged.

A suffixed session does **not** change the logical date, so the next rollover check compares
against the base date and still fires correctly at the cutoff.

## Config

One new key on the existing `Session` block:

| key | default | meaning |
|---|---|---|
| `day_cutoff_hour` | `4` | local hour a session day begins; `0` = calendar midnight |

Out of range (`<0`, `>23`) or unparseable → default. Non-integer → default. House rule: garbage
never yields 0, it yields the documented default.

## What is free

**Cross-channel is already true.** Every front-end posts `USER_MESSAGE` to one Arbiter which owns
one `history_` and one session file; the TurnGate serialises them. A SimpleX turn and a terminal
turn already interleave into the same conversation. No work is needed for the "across all channels"
requirement — it is a property of the existing design, and this feature only changes *which file*
that shared conversation lands in.

## Migration

Existing `YYYYMMDD-HHMMSS.jsonl` files are left alone. They stay readable by `session_search`, the
embeddings indexer, and `--resume <id>`; they simply stop being created. No rename, no conversion —
the directory just holds both shapes, and the old ones age out of relevance.

## Testing

- `logical_date` as a pure function: the table above, cutoff 0 and 23, month/year boundaries
  (2026-01-01 02:00 with cutoff 4 → `2025-12-31`), and a DST-shifted day.
- Boot: existing file for today → resumed and appended; absent → created; `--resume <date>` →
  that file; `--resume <missing>` → `MalConfig`.
- Rotation: a turn after the cutoff rotates the path, clears history/summary, bumps the epoch, and
  posts `SESSION_ROTATED`; a turn before it does not.
- `/new` mid-day → `-2` suffix, same logical date, next rollover still fires, AND the embeddings
  exclusion follows it (the regression test for the retired gotcha).
- Embeddings: `SESSION_ROTATED` re-points the exclusion; TSan clean with the index worker running.

## Deferred

- No back-fill or rename of historical timestamp sessions.
- No `--new-session` flag to force a fresh one at boot (`/new` covers it after start).
- No per-front-end sessions — explicitly the opposite of this feature's intent.
- Cutoff is a whole hour; no minute granularity.
