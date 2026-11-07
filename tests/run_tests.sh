#!/usr/bin/env bash
# Round-trip tests for mp3read.
#
# Builds mp3read with AddressSanitizer and UBSan, encodes test signals with
# ffmpeg, and checks for each one that:
#   - an embedded payload comes back byte for byte,
#   - the decoded PCM of the output is bit-identical to the original,
#   - a payload one byte too big is refused and no file is written.
# Then checks error paths (Layer II, no payload, damaged payload, tiny and
# empty files) and runs a short mutation fuzz of the parser.
#
# Needs: g++ (or set CXX), ffmpeg built with libmp3lame, sha256sum.
# libshine is used if ffmpeg has it.
# Usage: tests/run_tests.sh [fuzz iterations, default 300]

set -u
FUZZ_RUNS="${1:-300}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
CXX="${CXX:-g++}"
BIN="$WORK/mp3read"
PASS=0
FAIL=0

pass() { PASS=$((PASS + 1)); echo "  ok    $1"; }
fail() { FAIL=$((FAIL + 1)); echo "  FAIL  $1"; }
pcm_hash() { ffmpeg -v error -i "$1" -f s16le - | sha256sum | cut -d' ' -f1; }
capacity() { "$BIN" "$1" | sed -n 's/^Total ancillary space: \([0-9]*\) bytes$/\1/p'; }

command -v ffmpeg >/dev/null || { echo "ffmpeg not found"; exit 2; }
ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libmp3lame || { echo "ffmpeg lacks libmp3lame"; exit 2; }

echo "Building with sanitizers"
"$CXX" -std=c++11 -Wall -Wextra -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
    -o "$BIN" "$ROOT/mp3read.cpp" || exit 2
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0 UBSAN_OPTIONS=halt_on_error=1

echo "Encoding test signals"
enc() { # name, input filter, ffmpeg output options...
    local name="$1" src="$2"; shift 2
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i "$src" "$@" "$WORK/$name"
}
enc cbr128_tone.mp3   "sine=frequency=440:duration=6"                       -ac 2 -ar 44100 -c:a libmp3lame -b:a 128k
enc cbr128_noise.mp3  "anoisesrc=color=pink:duration=6:amplitude=0.3"       -ac 2 -ar 44100 -c:a libmp3lame -b:a 128k
enc cbr320_48k.mp3    "anoisesrc=color=white:duration=6:amplitude=0.2"      -ac 2 -ar 48000 -c:a libmp3lame -b:a 320k
enc vbr_chirp.mp3     "aevalsrc=0.4*sin(2*PI*(200+300*t)*t)|0.4*sin(2*PI*(500+80*t)*t):d=6" -ar 44100 -c:a libmp3lame -q:a 2
enc mono64.mp3        "anoisesrc=color=brown:duration=6:amplitude=0.4"      -ac 1 -ar 44100 -c:a libmp3lame -b:a 64k
enc mpeg2_22k.mp3     "aevalsrc=0.3*sin(2*PI*330*t)+0.2*sin(2*PI*1250*t):d=6" -ac 2 -ar 22050 -c:a libmp3lame -b:a 32k
enc mpeg25_8k.mp3     "anoisesrc=color=pink:duration=6:amplitude=0.3"       -ac 1 -ar 8000 -c:a libmp3lame -b:a 16k
enc tagged192.mp3     "anoisesrc=color=pink:duration=6:amplitude=0.3"       -ac 2 -ar 44100 -c:a libmp3lame -b:a 192k \
    -id3v2_version 3 -metadata title="Test tag" -write_id3v1 1
SAMPLES="cbr128_tone cbr128_noise cbr320_48k vbr_chirp mono64 mpeg2_22k mpeg25_8k tagged192"
if ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libshine; then
    enc shine128.mp3 "anoisesrc=color=pink:duration=6:amplitude=0.3" -ac 2 -ar 44100 -c:a libshine -b:a 128k
    SAMPLES="$SAMPLES shine128"
fi
enc layer2.mp2 "sine=frequency=440:duration=3" -ac 2 -ar 44100 -c:a mp2 -b:a 192k

echo "Round trips"
for s in $SAMPLES; do
    in="$WORK/$s.mp3"; out="$WORK/$s.out.mp3"
    cap="$(capacity "$in")"
    if [ -z "$cap" ]; then fail "$s: no capacity reported"; continue; fi
    max=$((cap - 13))
    if [ "$max" -lt 1 ]; then
        head -c 16 /dev/urandom > "$WORK/p.bin"
        if ! "$BIN" "$in" -w "$WORK/p.bin" -o "$out" >/dev/null 2>&1 && [ ! -e "$out" ]; then
            pass "$s: $cap bytes of space, payload refused, nothing written"
        else
            fail "$s: $cap bytes of space but a payload was accepted"
        fi
        continue
    fi
    size=$max; [ "$size" -gt 4096 ] && size=4096
    head -c "$size" /dev/urandom > "$WORK/p.bin"
    if "$BIN" "$in" -w "$WORK/p.bin" -o "$out" >/dev/null 2>&1 \
        && "$BIN" "$out" -e "$WORK/r.bin" >/dev/null 2>&1 \
        && cmp -s "$WORK/p.bin" "$WORK/r.bin"; then
        pass "$s: $size-byte payload round-trips ($cap bytes of space)"
    else
        fail "$s: round trip"
    fi
    if [ "$(pcm_hash "$in")" = "$(pcm_hash "$out")" ]; then
        pass "$s: decoded PCM bit-identical"
    else
        fail "$s: decoded PCM differs"
    fi
    head -c $((max + 1)) /dev/urandom > "$WORK/big.bin"
    rm -f "$WORK/big.mp3"
    if ! "$BIN" "$in" -w "$WORK/big.bin" -o "$WORK/big.mp3" >/dev/null 2>&1 && [ ! -e "$WORK/big.mp3" ]; then
        pass "$s: payload 1 byte too big refused, nothing written"
    else
        fail "$s: oversized payload accepted"
    fi
done

echo "Error paths"
head -c 16 /dev/urandom > "$WORK/p.bin"
if ! "$BIN" "$WORK/layer2.mp2" -w "$WORK/p.bin" -o "$WORK/l2.mp2" >/dev/null 2>&1 && [ ! -e "$WORK/l2.mp2" ]; then
    pass "Layer II file refused"
else
    fail "Layer II file accepted"
fi
if ! "$BIN" "$WORK/cbr128_tone.mp3" -e "$WORK/none.bin" >/dev/null 2>&1 && [ ! -e "$WORK/none.bin" ]; then
    pass "file without a payload: extraction fails cleanly"
else
    fail "file without a payload: extraction succeeded"
fi
# Put one changed byte back to its original value: the CRC must catch it
first="$(cmp -l "$WORK/cbr128_tone.mp3" "$WORK/cbr128_tone.out.mp3" | sed -n '20p')"
if [ -n "$first" ]; then
    set -- $first
    cp "$WORK/cbr128_tone.out.mp3" "$WORK/damaged.mp3"
    printf "\\$2" | dd of="$WORK/damaged.mp3" bs=1 seek=$(($1 - 1)) conv=notrunc 2>/dev/null
    if ! "$BIN" "$WORK/damaged.mp3" -e "$WORK/d.bin" >/dev/null 2>&1; then
        pass "damaged payload rejected by CRC"
    else
        fail "damaged payload accepted"
    fi
else
    fail "could not find a payload byte to damage"
fi
: > "$WORK/empty.bin"; printf 'ID' > "$WORK/two.bin"; printf 'ID3\003\000\000\177\177\177\177' > "$WORK/id3only.bin"
for f in empty.bin two.bin id3only.bin; do
    "$BIN" "$WORK/$f" >/dev/null 2>"$WORK/err.txt"
    if grep -q "Sanitizer\|runtime error" "$WORK/err.txt"; then fail "$f: sanitizer error"; else pass "$f: parsed without errors"; fi
done

echo "Mutation fuzz ($FUZZ_RUNS runs)"
crashes=0
for i in $(seq 1 "$FUZZ_RUNS"); do
    src="$WORK/$(echo $SAMPLES | tr ' ' '\n' | sed -n "$(( (i % $(echo $SAMPLES | wc -w)) + 1 ))p").out.mp3"
    [ -e "$src" ] || src="$WORK/cbr128_tone.out.mp3"
    cp "$src" "$WORK/fz.mp3"
    size=$(wc -c < "$WORK/fz.mp3")
    for _ in 1 2 3 4 5 6 7 8; do
        dd if=/dev/urandom of="$WORK/fz.mp3" bs=1 count=$((RANDOM % 16 + 1)) \
           seek=$(( (RANDOM * 32768 + RANDOM) % size )) conv=notrunc 2>/dev/null
    done
    [ $((i % 5)) -eq 0 ] && truncate -s $(( (RANDOM * 32768 + RANDOM) % size )) "$WORK/fz.mp3"
    for args in "" "-e $WORK/fz.bin"; do
        "$BIN" "$WORK/fz.mp3" $args >/dev/null 2>"$WORK/err.txt"
        if grep -q "Sanitizer\|runtime error" "$WORK/err.txt"; then
            crashes=$((crashes + 1)); cp "$WORK/fz.mp3" "$ROOT/tests/crash-$i.mp3"
        fi
    done
done
if [ "$crashes" -eq 0 ]; then pass "no sanitizer errors"; else fail "$crashes sanitizer errors (inputs saved as tests/crash-*.mp3)"; fi

echo
echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
