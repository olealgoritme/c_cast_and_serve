#!/bin/sh
# Script to convert audio in MP4 files to AAC format with specific settings
for f in *.mp4; do \
  ffmpeg -i "$f" \
    -c:v copy \
    -c:a aac -b:a 256k -ac 2 -ar 48000 \
    -movflags +faststart \
    "${f%.mp4}-cast-ready.mp4"; \
