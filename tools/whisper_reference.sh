#!/bin/sh
# tools/whisper_reference.sh — reference CommandSttProvider wrapper (whisper.cpp).
# Contract: last arg is the audio file; print ONLY the plain transcript to stdout.
# Language is English v1 (-l en). Adjust the binary/model path to your install.
#   Stt { provider = command  command = ./tools/whisper_reference.sh }
set -eu
AUDIO="$1"
# whisper.cpp: -nt = no timestamps, -otxt writes <AUDIO>.txt but also prints to stdout with -nt.
exec whisper-cli -l en -nt -f "$AUDIO"
