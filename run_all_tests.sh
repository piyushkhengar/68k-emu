#!/bin/bash
cd /Users/Piyush.Khengar/Documents/Cursor\ Playground/68k-emu
for f in ProcessorTests/68000/v1/*.json.gz; do
  basename="${f##*/}"
  basename="${basename%.json.gz}"
  result=$(./68k-emu --processor-tests ProcessorTests/68000/v1 "$basename" 2>&1 | tail -1)
  echo "$basename|$result"
done | sort -t'|' -k1
