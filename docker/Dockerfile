# Headless end-to-end rig for parfait: builds parfait-e2e, then runs it against a
# throwaway PipeWire graph with virtual mic/sink devices. Build context = repo root:
#   docker build -f docker/Dockerfile -t parfait-e2e .
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build g++ pkg-config git ca-certificates \
        qt6-base-dev qt6-declarative-dev \
        libpipewire-0.3-dev libspa-0.2-dev libopusenc-dev libsqlite3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
# Explicit COPYs: the build context is the repo root and we must not add a .dockerignore there.
COPY CMakeLists.txt /src/
COPY src/ /src/src/
COPY qml/ /src/qml/
COPY tests/ /src/tests/
COPY third_party/ /src/third_party/

RUN cmake -S /src -B /build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DWHISPER_BUILD_TESTS=OFF \
        -DWHISPER_BUILD_EXAMPLES=OFF \
        -DWHISPER_BUILD_SERVER=OFF \
    && cmake --build /build --target parfait-e2e


FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        pipewire pipewire-pulse pipewire-audio-client-libraries wireplumber \
        dbus-user-session pulseaudio-utils \
        espeak-ng ffmpeg yt-dlp python3 ca-certificates curl \
        libqt6core6t64 libqt6network6t64 libqt6sql6t64 \
        libpipewire-0.3-0t64 libopusenc0 libsqlite3-0 \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu's yt-dlp is years stale and YouTube rejects it outright; ship the upstream build.
RUN curl -fsSL -o /usr/local/bin/yt-dlp \
      https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp_linux \
    && chmod +x /usr/local/bin/yt-dlp

# Models live in their own layers, downloaded before anything that changes per-build.
RUN mkdir -p /opt/models && \
    curl -fSL --retry 3 -o /opt/models/ggml-base.en.bin \
      https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-base.en.bin && \
    test -s /opt/models/ggml-base.en.bin

# tinydiarize model (~488 MB): filename contains "tdrz", which is what switches
# speaker-turn detection on in TranscribeEngine. Separate layer so it is cached.
RUN curl -fSL --retry 3 -o /opt/models/ggml-small.en-tdrz.bin \
      https://huggingface.co/akashmjn/tinydiarize-whisper.cpp/resolve/main/ggml-small.en-tdrz.bin && \
    test -s /opt/models/ggml-small.en-tdrz.bin

# Multi-speaker "Them" panel for --multi. tinydiarize is trained on real
# conversational speech and does not fire on espeak/mbrola synthesis at all
# (verified: zero [SPEAKER_TURN] tokens for any voice/pitch/channel combination),
# so the turn-detection test needs genuine human audio. This is the same
# public-domain Apollo 13 loop whisper.cpp itself uses to demo tdrz: ~2:41 of
# three-way radio traffic (Swigert / Lovell / Houston). Own layer, own cache.
RUN mkdir -p /opt/samples && \
    curl -fSL --retry 3 -o /opt/samples/panel.mp3 \
      https://upload.wikimedia.org/wikipedia/commons/transcoded/6/6f/Apollo13-wehaveaproblem.ogg/Apollo13-wehaveaproblem.ogg.mp3 && \
    test -s /opt/samples/panel.mp3

# ubuntu:24.04 ships an "ubuntu" user at uid 1000; reuse it rather than fighting for the id.
RUN id -u tester >/dev/null 2>&1 || usermod -l tester -d /home/tester -m ubuntu && groupmod -n tester ubuntu

COPY docker/entrypoint.sh docker/test-meeting.sh /usr/local/bin/
RUN chmod +x /usr/local/bin/entrypoint.sh /usr/local/bin/test-meeting.sh

COPY --from=build /build/parfait-e2e /usr/local/bin/parfait-e2e
# whisper/ggml are built as shared libs alongside the binary.
COPY --from=build /build/bin/ /usr/local/lib/parfait/
RUN echo /usr/local/lib/parfait > /etc/ld.so.conf.d/parfait.conf && ldconfig

RUN mkdir -p /out /tmp/xdg && chown tester:tester /out /tmp/xdg && chmod 700 /tmp/xdg

USER tester
ENV XDG_RUNTIME_DIR=/tmp/xdg \
    HOME=/home/tester
WORKDIR /home/tester

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["test-meeting.sh"]
