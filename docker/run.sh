#!/usr/bin/env bash
# Host-side driver: build the e2e image and run a meeting simulation in it.
#   ./docker/run.sh [--youtube URL] [--increments N] [--multi]
# --multi runs the three-voice panel against the tinydiarize model and asserts
# speaker-turn indices show up on Them.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=${PARFAIT_E2E_IMAGE:-parfait-e2e}
HOST_OUT=${PARFAIT_E2E_OUT:-$REPO_ROOT/build/e2e-out}

mkdir -p "$HOST_OUT"

docker build -f "$REPO_ROOT/docker/Dockerfile" -t "$IMAGE" "$REPO_ROOT"

exec docker run --rm \
    --shm-size=256m \
    -v "$HOST_OUT:/out" \
    "$IMAGE" test-meeting.sh "$@"
