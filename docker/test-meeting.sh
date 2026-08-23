#!/usr/bin/env bash
# Drives gromarch-e2e through N ~30 s "meeting increments": a Them track plays into the
# meeting sink while short Me interjections play into the virtual mic.
set -euo pipefail

MODEL=/opt/models/ggml-base.en.bin
OUT=/out
WORK=/tmp/meeting
CHUNK_SEC=30
CAPTURE_SEC=32
INCREMENTS=2
YOUTUBE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --youtube)    YOUTUBE="$2"; shift 2 ;;
        --increments) INCREMENTS="$2"; shift 2 ;;
        --model)      MODEL="$2"; shift 2 ;;
        --chunk-sec)  CHUNK_SEC="$2"; CAPTURE_SEC=$(( $2 + 2 )); shift 2 ;;
        -h|--help)
            echo "usage: test-meeting.sh [--youtube URL] [--increments N] [--model PATH]"
            exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 1 ;;
    esac
done

mkdir -p "$WORK/chunks" "$OUT"
rm -f "$WORK"/*.wav "$WORK"/chunks/*.wav "$OUT"/inc*.txt "$OUT"/inc*.ogg

say() { # say <voice> <speed> <outfile> <text>
    espeak-ng -v "$1" -s "$2" -a 190 -w "$3" "$4"
}

# ---------------------------------------------------------------- Them source
if [ -n "$YOUTUBE" ]; then
    echo "fetching Them audio from $YOUTUBE"
    yt-dlp -x --audio-format wav --no-playlist --no-progress \
        -o "$WORK/them-raw.%(ext)s" "$YOUTUBE"
    RAW="$WORK/them-raw.wav"   # --audio-format wav guarantees the extension
else
    say en-us 145 "$WORK/them-raw.wav" \
"Good morning everyone, thanks for joining the Blue Harbor standup.
Let me walk through where we landed yesterday on the search rewrite.
The indexing service is now writing to the new schema, and the backfill finished overnight.
We processed about four million documents with no errors in the queue.
Query latency dropped from two hundred milliseconds down to sixty on the staging cluster.
The remaining risk is the ranking model. It still needs a fresh training run on the
updated feature set, and that takes roughly six hours on the shared machines.
I would like to start that today so we have numbers before the review on Thursday.
On the client side, the new results page is behind a feature flag for internal users only.
Two people reported that the filter chips do not clear properly when you go back.
That is a small fix and it is already in review.
The last thing is the migration plan for the old index. We will keep both running in
parallel for one week, then delete the old cluster and reclaim the storage budget.
Any questions before we move on to the next topic?"
    RAW="$WORK/them-raw.wav"
fi

ffmpeg -nostdin -loglevel error -y -i "$RAW" -ar 48000 -ac 2 -c:a pcm_s16le "$WORK/them.wav"
ffmpeg -nostdin -loglevel error -y -i "$WORK/them.wav" \
    -f segment -segment_time "$CHUNK_SEC" -ar 48000 -ac 2 -c:a pcm_s16le \
    "$WORK/chunks/chunk%03d.wav"

mapfile -t CHUNKS < <(ls "$WORK"/chunks/chunk*.wav)
echo "them.wav split into ${#CHUNKS[@]} chunk(s) of ${CHUNK_SEC}s"

# ------------------------------------------------------------------- Me lines
# One line per (increment, slot); played with gaps so the VAD sees separate utterances.
ME_LINES=(
    "Thanks Dana. Quick question about the backfill."
    "Did we verify the document counts against the old index?"
    "That sounds good to me."
    "I can take the filter chip fix this afternoon."
    "Let us keep the old cluster for a full week."
    "Agreed, no questions from me."
)
for i in "${!ME_LINES[@]}"; do
    say en-gb 150 "$WORK/me-$i.wav" "${ME_LINES[$i]}"
    ffmpeg -nostdin -loglevel error -y -i "$WORK/me-$i.wav" \
        -ar 48000 -ac 2 -c:a pcm_s16le "$WORK/me-$i-48k.wav"
done
ME_PER_INC=3

n_chunks=${#CHUNKS[@]}
if [ "$INCREMENTS" -gt "$n_chunks" ]; then INCREMENTS=$n_chunks; fi

total_me=0
total_them=0
failed=0

for (( i=0; i<INCREMENTS; i++ )); do
    chunk="${CHUNKS[$i]}"
    echo
    echo "=== increment $i ==="
    echo "them chunk: $chunk"

    gromarch-e2e --model "$MODEL" --seconds "$CAPTURE_SEC" \
        --audio "$OUT/inc$i.ogg" --out "$OUT/inc$i.txt" >"$WORK/inc$i.stdout" 2>"$WORK/inc$i.stderr" &
    e2e_pid=$!

    sleep 2   # let both capture streams settle before anything plays

    paplay --device=meeting "$chunk" &
    them_pid=$!

    (
        sleep 3
        for (( s=0; s<ME_PER_INC; s++ )); do
            idx=$(( (i * ME_PER_INC + s) % ${#ME_LINES[@]} ))
            paplay --device=micfeed "$WORK/me-$idx-48k.wav" || true
            sleep 5
        done
    ) &
    me_pid=$!

    wait "$e2e_pid" && rc=0 || rc=$?
    kill "$them_pid" "$me_pid" 2>/dev/null || true
    wait "$them_pid" "$me_pid" 2>/dev/null || true

    echo "--- transcript (exit $rc) ---"
    cat "$WORK/inc$i.stdout"
    if [ ! -s "$WORK/inc$i.stdout" ]; then
        echo "(no segments)"
        echo "--- stderr tail ---"
        tail -n 20 "$WORK/inc$i.stderr"
        failed=1
    fi

    me=$(grep -c '^Me \[' "$WORK/inc$i.stdout" || true)
    them=$(grep -c '^Them \[' "$WORK/inc$i.stdout" || true)
    echo "increment $i: me=$me them=$them"
    total_me=$(( total_me + me ))
    total_them=$(( total_them + them ))
done

echo
echo "=== summary ==="
echo "increments: $INCREMENTS"
echo "Me segments:   $total_me"
echo "Them segments: $total_them"
echo
ls -la "$OUT"

if [ "$failed" -ne 0 ] || [ "$total_me" -eq 0 ] || [ "$total_them" -eq 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
