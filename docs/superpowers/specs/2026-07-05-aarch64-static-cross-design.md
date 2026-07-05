# aarch64 static cross-build — design

**Date:** 2026-07-05
**Status:** Approved (Vaios — brainstorm 2026-07-05, both sections approved)
**Branch:** `feat/aarch64-cross` (off `main` @ `474f0fc`)

## Problem

Vaios wants to run hades on a **Raspberry Pi Zero 2 W** running **Raspberry Pi OS Lite 64-bit**
(Debian bookworm-based, aarch64/ARM64, glibc). Requirement: **compile on the x86_64 NixOS dev machine,
`scp` the result to the Pi, run** — minimal fuss, no per-dep install on the Pi.

Two facts force the approach: (1) `cpr` is **not** in Debian's apt repos, so even a native-Debian build
would build cpr from source; (2) a Nix cross build produces store-path-linked binaries that won't run on
bare Debian **unless fully static**.

## Decision (Vaios)

**Static Nix cross** via `pkgsCross.aarch64-multiplatform.pkgsStatic` (musl, **fully static** → zero
libc/shared-lib dependency → runs on any aarch64 Linux incl. Debian, no glibc-version-mismatch risk).
Rejected: QEMU-Debian dynamic (cpr still built from source + apt deps on the Pi); native-on-Pi (Zero 2 W
too slow, contradicts "compile here").

## Architecture

### `package.nix` — `stdenv.mkDerivation`

- `src = ./.` (the repo).
- `nativeBuildInputs`: cmake, ninja, pkg-config.
- `buildInputs`: cpr, nlohmann_json, httplib, readline (transitive curl/openssl/nghttp2/zlib/ncurses
  pulled statically by `pkgsStatic`). `httplib` is loopback HTTP only (no TLS — cpr handles HTTPS), so it
  needs no static openssl of its own.
- Build via the existing `CMakeLists.txt` (cmake/ninja). **`doCheck = false`** — aarch64 gtest can't run
  on x86 without emulation; the suite already gates natively.
- **Install (`$out` IS the deploy dir, all paths relative to its root):**
  ```
  $out/bin/     hades  hades-scope  hades-fs-read … (15 tools)
  $out/web/     $out/prompts/     $out/tools/*.sh
  $out/pi.hades   (deploy-relative manifest — see below)
  ```

### flake output

`packages.x86_64-linux.hades-aarch64-static =
   pkgsCross.aarch64-multiplatform.pkgsStatic.callPackage ./package.nix {}` → `nix build
.#hades-aarch64-static`. The devShell is untouched. (Optionally a native
`packages.x86_64-linux.hades` for a local release build.)

### `manifests/pi.hades` (committed)

A clean Pi-oriented manifest with **deploy-relative paths** (`native = ./bin/hades-fs-read`, `webroot =
web`, `system_prompt_file = prompts/soul.md`, `memory_file = memory/facts.md`), core modules on
(llm/tool_runner/memory/chat/arbiter), optional modules (serve/telegram/stt/tts/bridge) **commented**.
Distinct from `dev.hades` (different deploy target + paths — not a DRY violation). Command STT/TTS
wrappers require whisper/piper **on the Pi**; the **http** providers need no local binaries (the
Pi-friendly default).

## Deploy + smoke

- **Deploy:** `nix build .#hades-aarch64-static` → `result/`. `scp -rL result/ pi:hades/` → on the Pi
  `cd hades; export HADES_API_KEY=…; ./bin/hades pi.hades`. One dir, every relative path resolves, zero apt.
- **Smoke HERE (no Pi):**
  - `file result/bin/hades` → `ELF 64-bit … ARM aarch64 … statically linked`.
  - `ldd result/bin/hades` → "not a dynamic executable".
  - `qemu-aarch64 result/bin/hades-fs-read <<< '{"call":"describe"}'` (qemu-user, nixpkgs) → runs → the
    static aarch64 binary executes + a tool works.

## Risk (the one hard part)

Static-linking **cpr → curl → openssl / nghttp2 / zlib** in `pkgsStatic`. Nix's static curl builds
routinely; `pkgsStatic.libcpr` is the unknown. **This is a build-bringup task — iterative (build → fix
link error → build), not clean TDD.** Fallbacks if `pkgsStatic.libcpr` fails to build static:
1. an override/patch on the cpr (or curl) derivation (e.g. force `enableStatic`, disable a failing
   transitive feature);
2. build cpr via CMake `FetchContent` inside our derivation against Nix's static curl;
3. worst case, fall back to method C (QEMU-Debian) for the shipped artifact.

## Acceptance

`nix build .#hades-aarch64-static` green → `file` says static aarch64 ELF → `ldd` says not-dynamic →
qemu-aarch64 smoke passes → (Vaios) scp + run on the Pi + send a message → live.

## Non-goals / v2

`.deb` packaging · systemd service unit · auto-deploy script (documented scp+run) · a `.tar.gz` flake
output (nice-to-have, addable) · running the aarch64 test suite under emulation.
