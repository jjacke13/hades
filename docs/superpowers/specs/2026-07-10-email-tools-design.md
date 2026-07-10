# hades email tools — design (mailbox access via himalaya)

**Date:** 2026-07-10
**Status:** approved (Vaios)
**Branch:** `feat/email-tools` off `main` @ `d88b168`

## The one idea

Give the agent access to the **user's** mailbox — read mail (info, summaries, "notify me if something
urgent") and send mail **on the user's explicit instruction** — the himalaya use case. This is
deliberately NOT an email front-end: the agent is not commanded by email and does not converse over
it (that is a recorded future item with its own design seeds — Auth-Results gate, backlog
drain-and-discard, In-Reply-To threading). No new module, no thread, no teardown concern: **three
stateless tools** wrapping the **himalaya** CLI.

## Why himalaya (Vaios's call, 2026-07-10)

A native libcurl IMAP/SMTP implementation was designed and rejected: it would be unknown, untested
code guarding a sensitive surface, and OAuth2-only providers (Outlook; Gmail without an app
password) need a token dance that is real scope in C++. himalaya is known, tested, Rust, lightweight,
speaks every provider (OAuth2 included), has JSON output, and is in nixpkgs. Consequences:

- **Zero mail-protocol code in hades.** No IMAP, no SMTP, no RFC822/MIME parsing.
- **Zero credentials in hades.** Accounts/passwords/tokens/keyring live in himalaya's own config
  (`~/.config/himalaya/config.toml`); hades never sees a secret — nothing to redact.
- himalaya is a **runtime dependency**, user-installed (added to the dev shell for convenience;
  NOT a build dep). Missing binary → tools fail closed with a clear message.
- Trust boundary: hades trusts himalaya's output the way it trusts the whisper/piper wrappers —
  a user-installed local binary, same class as every `command`-provider seam.

## The three tools

Stateless subprocess binaries (one-JSON-line protocol, self-describing, fail-closed). Each execs
himalaya via `run_subprocess` (NO shell — whitespace-split argv), parses its JSON, and returns a
compact result. The himalaya argv prefix + account come from wiring argv — never from the LLM.

- **`check_inbox`** `{ n?, unseen_only? }` — list recent envelopes: `himalaya envelope list -o json`
  (+ account flag; `n` caps the page size, default 10, bounded 1..50). Returns
  `{messages:[{id, from, subject, date, flags(seen/unseen)}...]}`. `unseen_only` filters on the
  himalaya flags.
- **`read_email`** `{ id }` — full message text: `himalaya message read <id>` (+ account, `-o json`
  where supported; plain-text body as himalaya renders it — it already strips MIME/HTML). Returns
  `{id, from, subject, date, body}`. The id is gated to `[A-Za-z0-9_-]+` (an id is never a flag —
  leading-dash rejected, the git_read precedent).
- **`send_email`** `{ to, subject, body }` — compose an RFC822 text message and pipe it to
  `himalaya message send` on stdin (+ account). `to` must contain exactly one `@` address
  (basic shape check; no display names v1); subject/body free text (CRLF-normalized; header
  injection blocked — `to`/`subject` are stripped of `\r`/`\n` before composing).

Exact himalaya subcommand/flag shapes are pinned at implementation time against the installed
version's `--help` (the CLI stabilized in v1.x; the plan's implementer verifies and adapts —
the tool owns the argv construction in ONE place per binary).

## Gating

- **Reads are allow-band** — a heartbeat "each morning summarize new mail; notify me if urgent"
  must run unattended (delivery via the existing Telegram notify). New `Capability::MailRead` →
  allow.
- **`MailReadGuard`** (SelfScheduleGuard mirror, auto-registered when either read tool is
  rostered): hard-vetoes `check_inbox`/`read_email` on **`peer:` origins unconditionally** — a
  peer must never read the user's mail through the agent (the Bridge SECURITY surface: mailbox
  content in context can be spoken back; the guard keeps it out of peer turns entirely).
  `heartbeat:` and human origins free.
- **`send_email` is ALWAYS confirm** — new `Capability::MailSend` → confirm (needs_confirm), the
  Exec precedent. The human approves every outgoing mail ("send if I tell him to"); on heartbeat/
  peer turns the confirm auto-denies → the agent can never mail anyone unattended, and the
  exfiltration surface stays shut. Unattended/allowlisted sending is a v2 scope
  (`mail_send_allow` à la `exec_allow`), not built now.

## Config — the `Email { }` block (no `Module =` line)

```
Email
{
  command = himalaya      # argv prefix for the CLI (whitespace-split; default "himalaya")
  account =               # optional himalaya account name -> "-a <name>" (default: himalaya's default account)
}
```

Wiring appends `<command…> <account-or-"">`-derived argv to the three tools (single source of
truth; whitespace splitting is the norm here — `command` MAY contain spaces since it is an argv
prefix, split like `exec_allow` entries; `account` must be whitespace-free → `MalConfig`).
Rostering any of the three tools **without** an `Email` block is fine (defaults apply: plain
`himalaya`, default account) — the block only overrides. A missing himalaya binary surfaces at
call time as `ok:false` ("himalaya not found — install it and configure an account"), not at boot
(the binary may live only on the deploy host; boot-time probing would break cross-builds).

## Non-goals (v1)

- **Email front-end** (agent commanded by / replying over email) — recorded future item with the
  Auth-Results/backlog/threading design seeds attached.
- **NOTIFY_USER email sink** — notifications stay Telegram (v1); an email sink belongs to the
  front-end future item (it needs the module/thread shape).
- **Native libcurl IMAP/SMTP transport** — rejected for v1 (above); revisit only if himalaya ever
  becomes a blocker.
- **Unattended sending** (`mail_send_allow` scope) · attachments · HTML composition · folders
  beyond the default listing · mail search (himalaya has it; add on demand) · marking read/unread,
  delete, move — read + send only.

## Testing

- **Tool-level with a FAKE himalaya** (a tiny shell/python script on PATH via the tool's argv
  prefix — the tools take the command from argv, so tests point them at the fake): check_inbox
  parses envelope JSON (n cap, unseen filter); read_email returns body, rejects a leading-dash id;
  send_email composes correct RFC822 to stdin (To/Subject/body, CRLF, no header injection from a
  `subject` containing `\n`), one-`@` check; missing binary → ok:false; malformed himalaya JSON →
  ok:false (never throws); describe schemas (send documents the confirm expectation).
- **Capability/guard:** `capability_of` rows (MailRead→allow, MailSend→confirm); MailReadGuard
  origin tests (peer always vetoed incl. allow-switch irrelevance; heartbeat/human free);
  send_email confirm-band on human turn / auto-denied on heartbeat turn (Rig test).
- **Wiring:** argv threading (command prefix + account reach the binaries — side-effect proven via
  the fake), whitespace account → `MalConfig`, no-block defaults.

## Pieces (anticipated)

- `tools/{check_inbox,read_email,send_email}_main.cpp` — the three binaries (nlohmann_json +
  the existing `run_subprocess` seam — either compile its .cpp in (the cron_store pattern) or link
  `hades_core` (the ask_agent precedent); implementer picks whichever is lighter in the tree).
- `src/behaviors/standard_behaviors.cpp` + `include/hades/objective/mail_read_guard.h` —
  `MailReadGuard`.
- `include/hades/objective/capability_policy.h` + `src/behaviors/capability_policy.cpp` —
  `MailRead`/`MailSend` rows.
- `app/agent_wiring.cpp` — `Email` block parse + argv append + guard auto-registration.
- `flake.nix` devShell + docs (`manifest-reference.md` new §16 Email block + tools; soul.md one
  paragraph: you can read the user's mail when asked/scheduled and send only with approval;
  CLAUDE.md; dev.hades commented example). package.nix: 3 new bins.
