#!/usr/bin/env bash
# mame_trace.sh - run the Leapster-only MAME "oracle" headless and write a
# per-instruction CPU trace (PC, STATUS32, LP_COUNT, LP_START, LP_END, r0-r31,
# disassembly), capped at an exact number of instructions.
# See README.md next to this file for the exact line format and gotchas.
#
# Usage:
#   mame_trace.sh [options] [BIOS] [CART] [INSNS] [OUT]
#
# Positional (all optional, in this order; options override them):
#   BIOS    BIOS set name (default uni15; others for "leapster": uk21 sp10 ger21)
#   CART    cartridge .bin path, or "-" / "" for none (default: none)
#   INSNS   number of instructions to trace (default 200000; k/M suffix ok: 200k, 5M)
#   OUT     trace output path (default traces/<system>_<bios>[_<cart>]_<insns>.trace.gz
#           next to this script). A ".gz" suffix => gzip-compressed on the fly.
#
# Options:
#   -b BIOS     BIOS set name
#   -c CART     cartridge .bin
#   -n INSNS    number of instructions to trace (hard cap, exact)
#   -o OUT      output path (.gz => compressed)
#   -s SECONDS  emulated-seconds safety cap (default 120). Only matters if the
#               CPU sleeps forever before INSNS instructions were executed.
#   -S SYSTEM   MAME system (default leapster; also leapstertv)
#   -N          NO trace: run SECONDS (default 15) emulated seconds with video
#               and save the final frame as PNG into OUT (a directory, default
#               snapshots/ next to this script)
#   -k          keep the work dir (debugscript + MAME stdout log)
#   -- ARGS     extra arguments passed straight to MAME
#
# Env overrides: MAME_BIN, MAME_ROMPATH, ORACLE_WORKDIR (parent of the work dir;
# default traces/ next to this script -- deliberately NOT /tmp, which is a
# small tmpfs here).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
MAME_BIN="${MAME_BIN:-$ROOT/external/mame-leapster-oracle/leapster}"
MAME_ROMPATH="${MAME_ROMPATH:-$ROOT/external/mame-oracle-roms}"

SYSTEM=leapster
BIOS="" ; CART="" ; SECS="" ; OUT="" ; INSNS="" ; NOTRACE=0 ; KEEP=0
EXTRA=()

while getopts ":b:c:s:n:o:S:Nkh" opt; do
    case "$opt" in
        b) BIOS="$OPTARG" ;;
        c) CART="$OPTARG" ;;
        s) SECS="$OPTARG" ;;
        n) INSNS="$OPTARG" ;;
        o) OUT="$OPTARG" ;;
        S) SYSTEM="$OPTARG" ;;
        N) NOTRACE=1 ;;
        k) KEEP=1 ;;
        h) sed -n '2,33p' "$0"; exit 0 ;;
        :) echo "option -$OPTARG needs an argument" >&2; exit 2 ;;
        \?) echo "unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
pos=()
while [[ $# -gt 0 ]]; do
    if [[ "$1" == "--" ]]; then shift; EXTRA=("$@"); break; fi
    pos+=("$1"); shift
done
[[ ${#pos[@]} -ge 1 && -z "$BIOS"  ]] && BIOS="${pos[0]}"
[[ ${#pos[@]} -ge 2 && -z "$CART"  ]] && CART="${pos[1]}"
[[ ${#pos[@]} -ge 3 && -z "$INSNS" ]] && INSNS="${pos[2]}"
[[ ${#pos[@]} -ge 4 && -z "$OUT"   ]] && OUT="${pos[3]}"

if [[ -z "$BIOS" ]]; then
    case "$SYSTEM" in
        leapster)   BIOS=uni15 ;;
        leapstertv) BIOS=uni2111 ;;
    esac
fi
[[ "$CART" == "-" ]] && CART=""

# instruction count: allow k / M suffixes
INSNS="${INSNS:-200000}"
case "$INSNS" in
    *[kK]) INSNS=$(( ${INSNS%?} * 1000 )) ;;
    *[mM]) INSNS=$(( ${INSNS%?} * 1000000 )) ;;
esac
[[ "$INSNS" =~ ^[0-9]+$ && "$INSNS" -ge 1 ]] || { echo "bad instruction count: $INSNS" >&2; exit 2; }

if [[ -z "$SECS" ]]; then
    if [[ $NOTRACE -eq 1 ]]; then SECS=15; else SECS=120; fi
fi

[[ -x "$MAME_BIN" ]] || { echo "MAME binary not found: $MAME_BIN" >&2; exit 1; }
# MAME silently falls back to the default BIOS for an unknown name -> check.
# Buffer the XML first: with pipefail, `grep -q` exiting early SIGPIPEs MAME
# and makes the check fail spuriously.
LISTXML=$("$MAME_BIN" -listxml "$SYSTEM" 2>/dev/null || true)
if [[ -n "$BIOS" ]] && ! grep -q "<biosset name=\"$BIOS\"" <<<"$LISTXML"; then
    echo "unknown BIOS '$BIOS' for $SYSTEM; valid:" \
        $("$MAME_BIN" -listxml "$SYSTEM" | sed -n "/<machine name=\"$SYSTEM\"/,/<\/machine>/s/.*<biosset name=\"\([^\"]*\)\".*/\1/p") >&2
    exit 2
fi
CARTTAG=""
if [[ -n "$CART" ]]; then
    [[ -f "$CART" ]] || { echo "cart not found: $CART" >&2; exit 1; }
    CART="$(realpath "$CART")"
    CARTTAG="_$(basename "${CART%.*}" | tr -c 'A-Za-z0-9_\n' '_' | tr -s '_' | sed 's/_$//')"
fi

if [[ -z "$OUT" ]]; then
    if [[ $NOTRACE -eq 1 ]]; then OUT="$HERE/snapshots"
    else OUT="$HERE/traces/${SYSTEM}_${BIOS:-default}${CARTTAG}_${INSNS}.trace.gz"; fi
fi

# Work dir on the big disk (NOT /tmp). Its path must not contain spaces or
# commas: the trace file name is passed through the MAME debugger's parser.
WORKPARENT="${ORACLE_WORKDIR:-$HERE/traces}"
mkdir -p "$WORKPARENT"
WORK="$(mktemp -d "$WORKPARENT/.work.XXXXXX")"
case "$WORK" in *[\ ,]*) echo "work dir path must not contain spaces/commas: $WORK" >&2; exit 1 ;; esac
MAME_PID="" ; READER_PID=""
cleanup() {
    [[ -n "$MAME_PID" ]] && kill -9 "$MAME_PID" 2>/dev/null || true
    [[ -n "$READER_PID" ]] && kill "$READER_PID" 2>/dev/null || true
    if [[ $KEEP -eq 0 ]]; then rm -rf "$WORK"; else echo "work dir kept: $WORK" >&2; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# Common MAME args: ignore any user mame.ini, keep all state in the work dir
# (every run is a clean cold boot), never throttle, no sound.
ARGS=(
    "$SYSTEM"
    -noreadconfig
    -rompath "$MAME_ROMPATH"
    -cfg_directory "$WORK/cfg" -nvram_directory "$WORK/nvram"
    -input_directory "$WORK/inp" -state_directory "$WORK/sta"
    -diff_directory "$WORK/diff" -comment_directory "$WORK/comments"
    -snapshot_directory "$WORK/snap" -homepath "$WORK"
    -skip_gameinfo -nothrottle -sound none -nomouse -window
    -noautosave -norewind
    -seconds_to_run "$SECS"
)
[[ -n "$BIOS" ]] && ARGS+=(-bios "$BIOS")
[[ -n "$CART" ]] && ARGS+=(-cart "$CART")

export SDL_AUDIODRIVER=dummy
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-offscreen}"

if [[ $NOTRACE -eq 1 ]]; then
    # ---- snapshot mode: no debugger. -seconds_to_run writes a final PNG. ----
    t0=$(date +%s.%N)
    "$MAME_BIN" "${ARGS[@]}" -video soft "${EXTRA[@]}" > "$WORK/mame.log" 2>&1 \
        || { rc=$?; echo "MAME exited with $rc; log:" >&2; tail -50 "$WORK/mame.log" >&2; exit 1; }
    t1=$(date +%s.%N)
    mkdir -p "$OUT"
    n=0
    while IFS= read -r -d '' png; do
        dest="$OUT/${SYSTEM}_${BIOS:-default}${CARTTAG}_${SECS}s.png"
        cp "$png" "$dest"; echo "snapshot: $dest"; n=$((n+1))
    done < <(find "$WORK/snap" -name '*.png' -print0 2>/dev/null)
    [[ $n -gt 0 ]] || { echo "no snapshot produced; MAME log:" >&2; tail -50 "$WORK/mame.log" >&2; exit 1; }
    printf 'wall time: %.1fs\n' "$(echo "$t1 - $t0" | bc)" >&2
    exit 0
fi

# ---- trace mode ----
# The debugger must be enabled (-debug). "-debugger none" can NOT be used: that
# module resumes execution before the -debugscript is ever read. Use the Qt
# debugger with Qt's offscreen platform plugin instead (fully headless).
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-offscreen}"

# tracelog prints the register dump (no newline); the trace engine then appends
# "PPPPPPPP: <disassembly>\n", giving exactly one line per executed instruction.
# Register values are the state BEFORE the instruction at PC executes.
FMT='%08X %08X %08X %08X %08X'
REGS='pc,status32,r60,lp_start,lp_end'
for i in $(seq 0 31); do
    FMT+=' %08X'
    case $i in
        26) REGS+=',r26_gp' ;;  27) REGS+=',r27_fp' ;;  28) REGS+=',r28_sp' ;;
        29) REGS+=',r29_ilink1' ;; 30) REGS+=',r30_ilink2' ;; 31) REGS+=',r31_blink' ;;
        *)  REGS+=",r$i" ;;
    esac
done
FMT+=' | '

# GOTCHA 1: MAME -debug stops *at* the first instruction (reset vector,
# PC=40000000) and this script runs during that stop, so the trace engine never
# sees that instruction. We emit it by hand: disassemble it first ("dasm"),
# then a "FIRST ..." tracelog line with the reset register state; the reader
# below splices the two together.
# GOTCHA 2: debugger numbers are HEX by default -> "#" prefix for decimal.
{
    echo "dasm $WORK/first.dasm,pc,1,0"
    echo "trace $WORK/trace.fifo,maincpu,noloop,{tracelog \"$FMT\",$REGS}"
    echo "tracelog \"FIRST $FMT\\n\",$REGS"
    # "step N" executes exactly N more instructions (each one traced), then
    # breaks back into the debugger, which continues reading this script.
    if (( INSNS > 1 )); then echo "step #$((INSNS - 1))"; fi
    echo "trace off"
    echo "quit"
} > "$WORK/trace.cmd"

# The trace goes through a FIFO into a small filter (first-line splice) and
# optionally gzip, so nothing uncompressed ever hits the disk.
mkfifo "$WORK/trace.fifo"
mkdir -p "$(dirname "$OUT")"
TMPOUT="$OUT.part"
SPLICE='
import sys, shutil
dasm = sys.argv[1]
fi, fo = sys.stdin.buffer, sys.stdout.buffer
line = fi.readline().decode()
if line.startswith("FIRST "):
    first = open(dasm).readline().strip()
    line = line[6:].rstrip("\n").rstrip() + " " + first + "\n"
fo.write(line.encode())
shutil.copyfileobj(fi, fo, 1 << 20)
'
if [[ "$OUT" == *.gz ]]; then
    ( python3 -c "$SPLICE" "$WORK/first.dasm" < "$WORK/trace.fifo" | gzip -1 > "$TMPOUT" ) &
else
    ( python3 -c "$SPLICE" "$WORK/first.dasm" < "$WORK/trace.fifo" > "$TMPOUT" ) &
fi
READER_PID=$!

t0=$(date +%s.%N)
set +e
"$MAME_BIN" "${ARGS[@]}" -video none -debug -debugger qt -debugscript "$WORK/trace.cmd" "${EXTRA[@]}" \
    > "$WORK/mame.log" 2>&1 &
MAME_PID=$!
wait "$MAME_PID"; rc=$?
MAME_PID=""
set -e
t1=$(date +%s.%N)

# If MAME died before opening the FIFO, the reader is still blocked in open():
# open it read/write (never blocks) and close it so the reader sees EOF.
if kill -0 "$READER_PID" 2>/dev/null; then exec 3<>"$WORK/trace.fifo"; exec 3>&-; fi
wait "$READER_PID" || true
READER_PID=""

if [[ $rc -ne 0 ]]; then
    echo "MAME exited with $rc; log tail:" >&2; tail -50 "$WORK/mame.log" >&2
    rm -f "$TMPOUT"; exit 1
fi
mv "$TMPOUT" "$OUT"
if [[ "$OUT" == *.gz ]]; then lines=$(gzip -dc "$OUT" | wc -l); else lines=$(wc -l < "$OUT"); fi
printf 'trace: %s (%s lines, wall %.1fs)\n' "$OUT" "$lines" "$(echo "$t1 - $t0" | bc)" >&2
if (( lines != INSNS )); then
    echo "WARNING: expected $INSNS lines; CPU may have slept (ZZ) or -s cap ($SECS emulated s) hit" >&2
fi
