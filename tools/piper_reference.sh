#!/bin/sh
# tools/piper_reference.sh — reference CommandTtsProvider wrapper (piper + ffmpeg).
# Contract: reply TEXT arrives on stdin; write ONLY ogg-opus bytes to stdout.
# piper synthesizes a wav; ffmpeg transcodes to OGG/Opus (what Telegram sendVoice requires).
# Adjust the piper binary + --model path to your install.
#   Tts
#   {
#     provider = command
#     command  = ./tools/piper_reference.sh
#   }
set -eu
piper --model en_US-lessac-medium.onnx --output_file - \
  | ffmpeg -hide_banner -loglevel error -i - -c:a libopus -f ogg pipe:1
