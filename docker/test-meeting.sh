#!/usr/bin/env bash
# Drives gromarch-e2e through N ~30 s "meeting increments": a Them track plays into the
# meeting sink while short Me interjections play into the virtual mic.
set -euo pipefail

MODEL=/opt/models/ggml-base.en.bin
TDRZ_MODEL=/opt/models/ggml-small.en-tdrz.bin
OUT=/out
WORK=/tmp/meeting
CHUNK_SEC=30
CAPTURE_SEC=32
INCREMENTS=2
YOUTUBE=""
MULTI=0
TTS_PANEL=0
PANEL_SRC=/opt/samples/panel.mp3
MODEL_SET=0
INCREMENTS_SET=0
# Gap between panel turns. It must stay BELOW the engine's 0.7 s end-of-utterance
# silence: the turn counter only advances when whisper's [SPEAKER_TURN] token lands
# inside a decoded window, so consecutive speakers have to share one window.
GAP_SEC=0.4

while [ $# -gt 0 ]; do
    case "$1" in
        --youtube)    YOUTUBE="$2"; shift 2 ;;
        --increments) INCREMENTS="$2"; INCREMENTS_SET=1; shift 2 ;;
        --model)      MODEL="$2"; MODEL_SET=1; shift 2 ;;
        --chunk-sec)  CHUNK_SEC="$2"; CAPTURE_SEC=$(( $2 + 2 )); shift 2 ;;
        --multi)      MULTI=1; shift ;;
        --tts-panel)  MULTI=1; TTS_PANEL=1; shift ;;
        -h|--help)
            echo "usage: test-meeting.sh [--youtube URL] [--increments N] [--model PATH]"
            echo "                       [--multi] [--tts-panel]"
            echo "  --multi       multi-speaker Them panel + tinydiarize model, asserts turn indices"
            echo "  --tts-panel   --multi but with the synthesized three-voice panel (debug only:"
            echo "                tinydiarize does not fire on TTS, so this will not meet the bar)"
            exit 0 ;;
        *) echo "unknown flag: $1" >&2; exit 1 ;;
    esac
done

# --multi: multi-speaker Them track transcribed with the tinydiarize model, so
# System segments carry turn indices (Them#0, Them#1, ...).
if [ "$MULTI" -eq 1 ]; then
    [ "$MODEL_SET" -eq 1 ] || MODEL=$TDRZ_MODEL
    [ "$INCREMENTS_SET" -eq 1 ] || INCREMENTS=3
fi

mkdir -p "$WORK/chunks" "$OUT"
rm -f "$WORK"/*.wav "$WORK"/chunks/*.wav "$OUT"/inc*.txt "$OUT"/inc*.ogg

say() { # say <voice> <speed> <outfile> <text>
    espeak-ng -v "$1" -s "$2" -a 190 -w "$3" "$4"
}

sayp() { # sayp <voice> <speed> <pitch> <outfile> <text>
    espeak-ng -v "$1" -s "$2" -p "$3" -a 190 -w "$4" "$5"
}

# ---------------------------------------------------------------- Them source
if [ "$MULTI" -eq 1 ] && [ "$TTS_PANEL" -eq 0 ] && [ -z "$YOUTUBE" ]; then
    # Real three-way radio traffic baked into the image. Loudness-normalized so it
    # lands in the same range as the espeak tracks the rest of the rig plays.
    ffmpeg -nostdin -loglevel error -y -i "$PANEL_SRC" \
        -af "loudnorm=I=-16:TP=-1.5:LRA=11" -ar 48000 -ac 2 -c:a pcm_s16le "$WORK/them-raw.wav"
    RAW="$WORK/them-raw.wav"
    echo "panel track: $PANEL_SRC, $(
        ffprobe -v error -show_entries format=duration -of csv=p=0 "$RAW")s of real multi-speaker audio"
elif [ "$MULTI" -eq 1 ] && [ -z "$YOUTUBE" ]; then
    # Three maximally contrasting espeak voices (deep slow male / high fast female /
    # mid male) take turns of ~4 s each. Turns are kept short on purpose: the engine
    # closes a window after 7 s of voiced audio, so a pair of adjacent turns has to
    # fit inside one window for the tinydiarize turn token to be seen at all.
    PANEL_VOICE=(en-us+m3   en-us+f4   en-us+m7)
    PANEL_SPEED=(140        175        155)
    PANEL_PITCH=(15         85         45)
    PANEL_TEXT=(
"Good morning everyone. Thanks for joining the Blue Harbor standup."
"Morning. The backfill finished overnight with no errors at all."
"The indexing service is writing to the new schema now."
"We processed just over four million documents end to end."
"That matches the number from the old index exactly."
"Query latency dropped from two hundred milliseconds down to sixty."
"On staging though. The working set there is much smaller."
"Agreed. We should measure that again on the production cluster."
"The remaining risk is the ranking model. It needs a fresh training run."
"That takes about six hours on the shared machines."
"I can kick off the training job right after this call."
"Good. We need real numbers before the review on Thursday."
"I will post the evaluation report in the shared channel."
"The results page is behind a feature flag for internal users."
"Two people said the filter chips do not clear when you go back."
"What about the migration plan for the old index?"
"Keep both clusters running in parallel for one full week."
"Ten days works. I will update the runbook this afternoon."
    )
    : > "$WORK/panel.txt"
    silence="$WORK/silence.wav"
    ffmpeg -nostdin -loglevel error -y -f lavfi \
        -i "anullsrc=r=48000:cl=stereo:d=$GAP_SEC" -c:a pcm_s16le "$silence"
    for t in "${!PANEL_TEXT[@]}"; do
        v=$(( t % 3 ))
        sayp "${PANEL_VOICE[$v]}" "${PANEL_SPEED[$v]}" "${PANEL_PITCH[$v]}" \
            "$WORK/turn-$t.wav" "${PANEL_TEXT[$t]}"
        ffmpeg -nostdin -loglevel error -y -i "$WORK/turn-$t.wav" \
            -ar 48000 -ac 2 -c:a pcm_s16le "$WORK/turn-$t-48k.wav"
        printf "file '%s'\nfile '%s'\n" "$WORK/turn-$t-48k.wav" "$silence" >> "$WORK/panel.txt"
    done
    ffmpeg -nostdin -loglevel error -y -f concat -safe 0 -i "$WORK/panel.txt" \
        -ar 48000 -ac 2 -c:a pcm_s16le "$WORK/them-raw.wav"
    RAW="$WORK/them-raw.wav"
    echo "panel track: ${#PANEL_TEXT[@]} turns across 3 voices, $(
        ffprobe -v error -show_entries format=duration -of csv=p=0 "$RAW")s"
elif [ -n "$YOUTUBE" ]; then
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
multi_max_speakers=0    # best per-increment `speakers=` seen across the run
multi_bad=0             # increment had them-speech but no turn index, or exited non-zero
multi_hash_me=0         # a Me line carried a "#" turn suffix (must never happen)
BREAKDOWN=()

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
    them=$(grep -cE '^Them(#[0-9]+)? \[' "$WORK/inc$i.stdout" || true)
    echo "increment $i: me=$me them=$them"
    total_me=$(( total_me + me ))
    total_them=$(( total_them + them ))

    if [ "$MULTI" -eq 1 ]; then
        # `speakers=` counts distinct turn indices within this e2e run; indices
        # restart at 0 each increment, so we never sum them across increments.
        speakers=$(sed -n 's/.*[[:space:]]speakers=\([0-9]*\).*/\1/p' "$WORK/inc$i.stderr" | tail -n1)
        speakers=${speakers:-0}
        turns=$(grep -oE '^Them#[0-9]+' "$WORK/inc$i.stdout" | sort -u | tr '\n' ' ')
        me_hash=$(grep -c '^Me#' "$WORK/inc$i.stdout" || true)
        if [ "$speakers" -gt "$multi_max_speakers" ]; then multi_max_speakers=$speakers; fi
        [ "$me_hash" -eq 0 ] || multi_hash_me=1
        note=ok
        if [ "$rc" -ne 0 ]; then note="BAD exit=$rc"; multi_bad=1
        elif [ "$them" -gt 0 ] && [ "$speakers" -lt 1 ]; then note="BAD them-speech with speakers=0"; multi_bad=1
        elif [ "$me_hash" -ne 0 ]; then note="BAD Me line has # suffix"
        fi
        BREAKDOWN+=("increment $i: exit=$rc me=$me them=$them speakers=$speakers turns=[${turns% }] $note")
        echo "increment $i: speakers=$speakers turns=[${turns% }]"
    fi
done

echo
echo "=== summary ==="
echo "increments: $INCREMENTS"
echo "Me segments:   $total_me"
echo "Them segments: $total_them"

if [ "$MULTI" -eq 1 ]; then
    echo
    echo "--- multi-speaker breakdown ---"
    printf '%s\n' "${BREAKDOWN[@]}"
    echo "model: $MODEL"
    echo "best per-increment speakers: $multi_max_speakers (need >= 2 somewhere)"
    echo "Me lines with '#' suffix: $multi_hash_me (need 0)"
    if [ "$multi_bad" -ne 0 ] || [ "$multi_max_speakers" -lt 2 ] \
       || [ "$multi_hash_me" -ne 0 ] || [ "$total_me" -eq 0 ]; then
        failed=1
    fi
fi

echo
ls -la "$OUT"

if [ "$failed" -ne 0 ] || [ "$total_me" -eq 0 ] || [ "$total_them" -eq 0 ]; then
    echo "FAIL"
    exit 1
fi
echo "PASS"
