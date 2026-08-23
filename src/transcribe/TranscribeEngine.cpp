#include "TranscribeEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#ifdef GROMARCH_WITH_WHISPER
#include <whisper.h>
#endif

namespace gromarch {
namespace {

constexpr int    kSampleRate      = 16000;
constexpr int    kVadFrame        = 320;                                  // 20 ms
constexpr double kFrameSec        = double(kVadFrame) / double(kSampleRate);
constexpr double kSpeechWindowSec = 7.0;   // voiced audio that forces a window close
constexpr double kMaxWindowSec    = 12.0;  // hard cap on window length
constexpr double kSilenceEndSec   = 0.7;   // trailing silence that ends an utterance
constexpr double kOverlapSec      = 0.2;   // audio kept after finalizing a window
constexpr double kPartialEverySec = 1.0;   // re-decode cadence for partials
constexpr double kMinDecodeSec    = 1.0;   // whisper dislikes very short inputs
constexpr double kIdleTrimSec     = 2.0;   // drop silence when no speech is buffered
constexpr double kResyncSec       = 0.5;   // clock drift before we trust feed()'s t0
constexpr double kMaxQueuedSec    = 30.0;  // backpressure: audio waiting in the queue
constexpr float  kVadFloor        = 0.006f;  // absolute RMS gate
constexpr float  kVadFactor       = 2.5f;    // ... and relative to the noise floor

// Speaker-turn probe (tinydiarize, System stream only). A turn that falls on a
// window boundary is invisible: the two voices never share a decode, no
// [SPEAKER_TURN] token is ever produced, and the new speaker inherits the old
// label. So each finalized window is decoded a second time with the previous
// window's spoken tail glued in front of it, and only the *turn times* are taken
// from that decode -- the transcript always comes from the window's own decode,
// which the probe therefore cannot perturb. Turn times from the two decodes are
// merged, so the probe can only ever add turns, never remove one.
constexpr double kTurnTailSec      = 1.75;  // spoken audio carried over as the probe's head
constexpr double kTailKeepSilSec   = 0.3;   // trailing silence kept with that tail
constexpr double kTailSlackSec     = 0.2;   // timestamp slop when classifying tail segments
constexpr double kTailMaxGapSec    = 8.0;   // a tail older than this is stale, not probed
constexpr double kTurnMergeSec     = 1.0;   // turns this close are the same boundary seen twice
constexpr double kTurnSlackSec     = 0.35;  // timestamp slop when placing a turn between segments

// Per-capture-stream sliding window plus its VAD bookkeeping.
struct StreamState {
    std::vector<float> buf;              // audio not yet finalized
    std::vector<float> vadTail;          // leftover samples of a partial VAD frame
    double bufStart = 0.0;               // seconds from meeting start of buf[0]
    double clock = 0.0;                  // seconds from meeting start of the next sample
    bool started = false;
    float noiseFloor = kVadFloor;
    double speechSec = 0.0;              // voiced audio in the open window
    double silenceSec = 0.0;             // trailing silence
    bool hadSpeech = false;
    double sincePartialSec = 0.0;
    bool partialOpen = false;

    // Speaker-turn replay state (System stream with a tdrz model only).
    std::vector<float> turnTail;         // spoken tail of the previous finalized window
    double turnTailEnd = 0.0;            // seconds from meeting start of its last sample
    bool pendingTurn = false;            // turn token on that window's last segment, not yet counted

    void resetWindow() {
        speechSec = 0.0;
        silenceSec = 0.0;
        hadSpeech = false;
        sincePartialSec = 0.0;
        partialOpen = false;
    }

    void clearTurnTail() {
        turnTail.clear();
        turnTail.shrink_to_fit();
        turnTailEnd = 0.0;
    }

    void reset() {
        buf.clear();
        vadTail.clear();
        bufStart = 0.0;
        clock = 0.0;
        started = false;
        noiseFloor = kVadFloor;
        clearTurnTail();
        pendingTurn = false;
        resetWindow();
    }
};

// One decoded whisper segment inside a window: times are relative to the window.
struct DecodedSeg {
    double t0 = 0.0;
    double t1 = 0.0;
    QString text;
    bool turnNext = false;   // tinydiarize: a speaker turn follows this segment
};

// tinydiarize is a property of the weights, not a runtime switch: only the
// *-tdrz models emit the [SPEAKER_TURN] token, and turning tdrz_enable on for a
// plain model makes whisper hunt for a token it was never trained to produce,
// which degrades the transcript. whisper.cpp itself has no "does this model
// support tdrz" query, so we go by the filename convention used by the upstream
// download scripts (ggml-small.en-tdrz.bin).
bool modelSupportsTdrz(const QString& path) {
    return QFileInfo(path).fileName().contains(QStringLiteral("tdrz"), Qt::CaseInsensitive);
}

QString defaultModelPath() {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(QStringLiteral("gromarch/models/ggml-base.en.bin"));
}

// whisper emits bracketed markers for music/silence; they are noise in a transcript.
QString cleanupText(QString text) {
    text.replace(QRegularExpression(QStringLiteral("\\[[^\\]]*\\]")), QString());
    text.replace(QRegularExpression(QStringLiteral("\\((?:BLANK_AUDIO|blank_audio|silence|SILENCE)\\)")),
                 QString());
    text.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    return text.trimmed();
}

// RMS of the kVadFrame samples ending at `end` (exclusive).
float frameRms(const std::vector<float>& buf, size_t end) {
    double sum = 0.0;
    for (size_t i = end - size_t(kVadFrame); i < end; ++i) {
        const double v = double(buf[i]);
        sum += v * v;
    }
    return float(std::sqrt(sum / kVadFrame));
}

} // namespace

struct TranscribeEngine::Impl {
    explicit Impl(TranscribeEngine* engine) : q(engine) {}

    TranscribeEngine* const q;

    std::atomic<bool> available{false};
    std::atomic<bool> quit{false};
    std::atomic<qint64> queuedSamples{0};

    mutable std::mutex pathMutex;
    QString path = defaultModelPath();
    QString loadedPath;

    std::mutex taskMutex;
    std::condition_variable taskCv;
    std::deque<std::function<void()>> tasks;
    std::thread worker;

    StreamState streams[2];

    // Speaker-turn counter for the System stream; monotonic across the meeting.
    std::atomic<bool> tdrz{false};       // loaded model supports tinydiarize
    int systemTurn = 0;                  // worker thread only

#ifdef GROMARCH_WITH_WHISPER
    std::mutex ctxMutex;
    whisper_context* ctx = nullptr;
    bool loadFailed = false;
#endif

    // ---- signals -----------------------------------------------------------
    // Called on the worker thread; marshalled back onto the engine's thread.

    void emitSegment(const TranscriptSegment& seg) {
        TranscribeEngine* engine = q;
        QMetaObject::invokeMethod(engine, [engine, seg] { emit engine->segmentReady(seg); },
                                  Qt::QueuedConnection);
    }
    void emitFinished() {
        TranscribeEngine* engine = q;
        QMetaObject::invokeMethod(engine, [engine] { emit engine->finished(); }, Qt::QueuedConnection);
    }
    void emitError(const QString& message) {
        TranscribeEngine* engine = q;
        QMetaObject::invokeMethod(engine, [engine, message] { emit engine->error(message); },
                                  Qt::QueuedConnection);
    }
    void emitAvailability(bool ok) {
        TranscribeEngine* engine = q;
        QMetaObject::invokeMethod(engine, [engine, ok] { emit engine->availabilityChanged(ok); },
                                  Qt::QueuedConnection);
    }

    QString modelPath() const {
        std::lock_guard<std::mutex> lock(pathMutex);
        return path;
    }

    void setPath(const QString& p) {
        std::lock_guard<std::mutex> lock(pathMutex);
        path = p.isEmpty() ? defaultModelPath() : p;
    }

    void post(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            if (quit.load()) return;
            tasks.push_back(std::move(task));
        }
        taskCv.notify_one();
    }

    void run() {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(taskMutex);
                taskCv.wait(lock, [this] { return quit.load() || !tasks.empty(); });
                if (quit.load() && tasks.empty()) return;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(taskMutex);
            quit.store(true);
        }
        taskCv.notify_all();
        if (worker.joinable()) worker.join();
    }

    // ---- model -------------------------------------------------------------

    bool ensureModel() {
#ifdef GROMARCH_WITH_WHISPER
        const QString want = modelPath();
        {
            std::lock_guard<std::mutex> lock(ctxMutex);
            if (ctx && loadedPath == want) return true;
            if (!ctx && loadFailed && loadedPath == want) return false;
        }

        whisper_context* fresh = nullptr;
        QString failure;
        if (!QFileInfo::exists(want)) {
            failure = QStringLiteral("Whisper model not found: %1").arg(want);
        } else {
            whisper_context_params cparams = whisper_context_default_params();
            fresh = whisper_init_from_file_with_params(want.toUtf8().constData(), cparams);
            if (!fresh) failure = QStringLiteral("Failed to load whisper model: %1").arg(want);
        }

        {
            std::lock_guard<std::mutex> lock(ctxMutex);
            if (ctx) whisper_free(ctx);
            ctx = fresh;
            loadedPath = want;
            loadFailed = (fresh == nullptr);
        }

        const bool ok = fresh != nullptr;
        tdrz.store(ok && modelSupportsTdrz(want));
        available.store(ok);
        emitAvailability(ok);
        if (!ok) emitError(failure);
        return ok;
#else
        tdrz.store(false);
        if (available.exchange(false)) emitAvailability(false);
        return false;
#endif
    }

    // Let a new meeting retry a load that previously failed (model since downloaded).
    void clearLoadFailure() {
#ifdef GROMARCH_WITH_WHISPER
        std::lock_guard<std::mutex> lock(ctxMutex);
        loadFailed = false;
#endif
    }

    void freeModel() {
        tdrz.store(false);
#ifdef GROMARCH_WITH_WHISPER
        std::lock_guard<std::mutex> lock(ctxMutex);
        if (ctx) whisper_free(ctx);
        ctx = nullptr;
        loadedPath.clear();
        loadFailed = false;
#endif
    }

    // ---- decoding ----------------------------------------------------------

    // Decode a window into its whisper segments. Returns false if nothing was
    // decoded (no model, empty input, decode failure).
    bool decode(const std::vector<float>& pcm, bool wantTurns, std::vector<DecodedSeg>& out) {
        out.clear();
#ifdef GROMARCH_WITH_WHISPER
        if (pcm.empty()) return false;
        if (!ensureModel()) return false;

        // Pad short windows: whisper rejects sub-100 ms input and is unstable near it.
        const size_t minSamples = size_t(kMinDecodeSec * kSampleRate);
        const std::vector<float>* samples = &pcm;
        std::vector<float> padded;
        if (pcm.size() < minSamples) {
            padded = pcm;
            padded.resize(minSamples, 0.0f);
            samples = &padded;
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.n_threads        = std::max(1, std::min(4, int(std::thread::hardware_concurrency())));
        wparams.language         = "en";
        wparams.translate        = false;
        wparams.no_timestamps    = false;
        wparams.single_segment   = false;
        wparams.no_context       = true;    // windows overlap; carried context drifts
        wparams.suppress_blank   = true;
        wparams.temperature_inc  = 0.0f;    // no fallback retries in a live loop
        wparams.print_progress   = false;
        wparams.print_realtime   = false;
        wparams.print_timestamps = false;
        wparams.print_special    = false;
        wparams.tdrz_enable      = wantTurns;

        // Window length before padding: whisper can time-stamp into the padding.
        const double windowSec = double(pcm.size()) / kSampleRate;
        {
            std::lock_guard<std::mutex> lock(ctxMutex);
            if (!ctx) return false;
            if (whisper_full(ctx, wparams, samples->data(), int(samples->size())) != 0) {
                emitError(QStringLiteral("whisper_full() failed"));
                return false;
            }
            const int n = whisper_full_n_segments(ctx);
            out.reserve(size_t(n));
            for (int i = 0; i < n; ++i) {
                DecodedSeg d;
                const char* piece = whisper_full_get_segment_text(ctx, i);
                if (piece) d.text = QString::fromUtf8(piece);
                // whisper timestamps are in 10 ms units, relative to the window.
                d.t0 = std::clamp(double(whisper_full_get_segment_t0(ctx, i)) / 100.0, 0.0, windowSec);
                d.t1 = std::clamp(double(whisper_full_get_segment_t1(ctx, i)) / 100.0, d.t0, windowSec);
                d.turnNext = wantTurns && whisper_full_get_segment_speaker_turn_next(ctx, i);
                out.push_back(std::move(d));
            }
        }
        return true;
#else
        Q_UNUSED(pcm);
        Q_UNUSED(wantTurns);
        return false;
#endif
    }

    // ---- speaker-turn replay -----------------------------------------------

    // Keep the last kTurnTailSec of *spoken* audio from the window being closed:
    // the trailing silence carries no speaker identity, so it is skipped (bar a
    // short slice that keeps the utterance gap visible to whisper). Bounded by
    // construction at kTurnTailSec + kTailKeepSilSec of samples.
    void captureTurnTail(StreamState& s) {
        const size_t frame = size_t(kVadFrame);
        const float gate = std::max(kVadFloor, s.noiseFloor * kVadFactor);

        size_t end = (s.buf.size() / frame) * frame;
        while (end >= frame && frameRms(s.buf, end) <= gate) end -= frame;
        if (end < frame) {                  // nothing voiced in this window after all
            s.clearTurnTail();
            return;
        }

        end = std::min(s.buf.size(), end + size_t(kTailKeepSilSec * kSampleRate));
        const size_t want = size_t(kTurnTailSec * kSampleRate);
        const size_t start = end > want ? end - want : 0;
        s.turnTail.assign(s.buf.begin() + qsizetype(start), s.buf.begin() + qsizetype(end));
        s.turnTailEnd = s.bufStart + double(end) / kSampleRate;
    }

    // Decode [previous window's spoken tail | this window] and report where
    // whisper puts speaker turns, in this window's own seconds. A time at or
    // before the window's first text means the turn is on the seam itself. The
    // tail earns its keep twice: it is the only way a seam turn can be seen at
    // all, and it gives tdrz the conversational context it needs to fire inside
    // the window as well.
    void probeTurnBoundaries(StreamState& s, std::vector<double>& out) {
        if (s.turnTail.empty()) return;
        if (s.bufStart - s.turnTailEnd > kTailMaxGapSec) {   // stale: a long pause, not a seam
            s.clearTurnTail();
            return;
        }

        const double tailSec = double(s.turnTail.size()) / kSampleRate;
        std::vector<float> probe;
        probe.reserve(s.turnTail.size() + s.buf.size());
        probe.insert(probe.end(), s.turnTail.begin(), s.turnTail.end());
        probe.insert(probe.end(), s.buf.begin(), s.buf.end());

        std::vector<DecodedSeg> segs;
        if (!decode(probe, true, segs)) return;
        for (const DecodedSeg& d : segs)
            if (d.turnNext) out.push_back(d.t1 - tailSec);
    }

    void emitWindow(int stream, bool final) {
        StreamState& s = streams[stream];
        if (s.buf.empty()) return;

        // Turn detection only makes sense on the far end; "Me" is always speaker -1.
        const bool turns = tdrz.load() && stream == int(Stream::System);
        const double windowEnd = s.bufStart + double(s.buf.size()) / kSampleRate;

        std::vector<DecodedSeg> decoded;
        decode(s.buf, turns, decoded);

        auto emitOne = [&](double t0, double t1, const QString& text, int speaker) {
            TranscriptSegment seg;
            seg.stream = stream;
            seg.t0 = t0;
            seg.t1 = t1;
            seg.text = text;
            seg.final = final;
            seg.speaker = speaker;
            if (!final) s.partialOpen = true;
            emitSegment(seg);
        };

        // Partials re-decode the same audio every second, so they never advance
        // the turn counter; they stay one concatenated segment carrying the
        // counter as it stands. Non-tdrz windows keep the same shape, speaker -1.
        if (!turns || !final) {
            QString text;
            for (const DecodedSeg& d : decoded) text += d.text;
            text = cleanupText(text);
            if (text.isEmpty() && !(final && s.partialOpen)) return;
            const int speaker = turns ? systemTurn + (s.pendingTurn ? 1 : 0) : -1;
            emitOne(s.bufStart, windowEnd, text, speaker);
            return;
        }

        // Where the speaker changes in this window: what the window's own decode
        // saw, plus what the tail probe saw, merged so one boundary reported by
        // both decodes is still one boundary.
        std::vector<double> boundaries;
        for (const DecodedSeg& d : decoded)
            if (d.turnNext) boundaries.push_back(d.t1);
        probeTurnBoundaries(s, boundaries);
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end(),
                                     [](double kept, double next) {
                                         return next - kept < kTurnMergeSec;
                                     }),
                         boundaries.end());

        // Boundaries sitting at or before the first text are on the seam, and owe
        // this window's head exactly one turn -- as does a turn deferred from the
        // previous window (see the run loop), which is the same boundary seen
        // from the other side and must not be counted twice.
        const double firstT0 = decoded.empty() ? 0.0 : decoded.front().t0;
        size_t bi = 0;
        bool seam = false;
        while (bi < boundaries.size() && boundaries[bi] <= firstT0 + kTurnSlackSec) {
            seam = true;
            ++bi;
        }
        const bool turnBefore = s.pendingTurn || seam;

        // Nothing decoded: hold the seam turn over rather than spending it on a
        // window that labels no text.
        if (decoded.empty()) {
            s.pendingTurn = turnBefore;
            if (s.partialOpen) emitOne(s.bufStart, windowEnd, QString(), systemTurn);
            return;
        }
        if (turnBefore) ++systemTurn;
        s.pendingTurn = false;

        // Emit one segment per run of whisper segments that share a speaker,
        // using whisper's own timestamps instead of the whole-window span.
        bool emitted = false;
        size_t i = 0;
        while (i < decoded.size()) {
            size_t j = i;
            QString text;
            bool cut = false;
            while (j < decoded.size()) {
                text += decoded[j].text;
                ++j;
                // Boundaries inside the segment just consumed are behind us.
                while (bi < boundaries.size() && boundaries[bi] <= decoded[j - 1].t0) ++bi;
                if (bi >= boundaries.size()) continue;
                const double nextT0 = j < decoded.size()
                                          ? decoded[j].t0 + kTurnSlackSec
                                          : std::numeric_limits<double>::max();
                if (boundaries[bi] <= nextT0) {
                    ++bi;
                    cut = true;
                    break;
                }
            }
            const QString clean = cleanupText(text);
            if (!clean.isEmpty()) {
                emitOne(std::min(s.bufStart + decoded[i].t0, windowEnd),
                        std::min(s.bufStart + decoded[j - 1].t1, windowEnd),
                        clean, systemTurn);
                emitted = true;
            }
            if (cut) {
                // A turn after the window's last segment is ambiguous: whisper saw
                // the change coming but the new voice is in the next window. Defer
                // it there, where the seam decides the same boundary again --
                // counting it here as well would count one boundary twice.
                if (j == decoded.size()) s.pendingTurn = true;
                else ++systemTurn;      // the run that follows is the new speaker
            }
            i = j;
        }

        // Nothing survived cleanup but a partial is on screen: close it out.
        if (!emitted && s.partialOpen) emitOne(s.bufStart, windowEnd, QString(), systemTurn);
    }

    // Finalize the open window, then keep a short overlap so a word split across
    // the boundary still has context in the next window.
    void closeWindow(int stream) {
        StreamState& s = streams[stream];
        if (s.hadSpeech) {
            emitWindow(stream, true);
            if (tdrz.load() && stream == int(Stream::System)) captureTurnTail(s);
        }

        const size_t keep = std::min(s.buf.size(), size_t(kOverlapSec * kSampleRate));
        const size_t drop = s.buf.size() - keep;
        if (drop > 0) {
            s.buf.erase(s.buf.begin(), s.buf.begin() + qsizetype(drop));
            s.bufStart += double(drop) / kSampleRate;
        }
        s.resetWindow();
    }

    // Energy VAD over 20 ms frames with a slowly adapting noise floor.
    void runVad(StreamState& s, const float* data, size_t count) {
        s.vadTail.insert(s.vadTail.end(), data, data + count);
        size_t offset = 0;
        while (s.vadTail.size() - offset >= size_t(kVadFrame)) {
            double sum = 0.0;
            for (int i = 0; i < kVadFrame; ++i) {
                const double v = double(s.vadTail[offset + size_t(i)]);
                sum += v * v;
            }
            const float rms = float(std::sqrt(sum / kVadFrame));
            const bool voiced = rms > std::max(kVadFloor, s.noiseFloor * kVadFactor);
            if (voiced) {
                s.hadSpeech = true;
                s.speechSec += kFrameSec;
                s.silenceSec = 0.0;
            } else {
                s.silenceSec += kFrameSec;
                s.noiseFloor = 0.95f * s.noiseFloor + 0.05f * rms;
                if (s.noiseFloor < 1e-4f) s.noiseFloor = 1e-4f;
            }
            offset += size_t(kVadFrame);
        }
        if (offset > 0) s.vadTail.erase(s.vadTail.begin(), s.vadTail.begin() + qsizetype(offset));
    }

    void handleAudio(int stream, const QByteArray& bytes, double t0) {
        StreamState& s = streams[stream];
        const size_t count = size_t(bytes.size()) / sizeof(float);
        if (count == 0) return;

        const float* data = reinterpret_cast<const float*>(bytes.constData());

        // The sample counter is authoritative; resync only after a real dropout.
        if (!s.started || std::abs(t0 - s.clock) > kResyncSec) {
            s.started = true;
            s.clock = t0;
            if (s.buf.empty()) s.bufStart = t0;
        }

        s.buf.insert(s.buf.end(), data, data + count);
        s.clock += double(count) / kSampleRate;
        s.sincePartialSec += double(count) / kSampleRate;
        runVad(s, data, count);

        const double windowSec = double(s.buf.size()) / kSampleRate;
        if (!s.hadSpeech) {
            // Nothing but silence: keep the buffer from growing without bound.
            if (windowSec > kIdleTrimSec) {
                const size_t keep = size_t(kOverlapSec * kSampleRate);
                const size_t drop = s.buf.size() - std::min(s.buf.size(), keep);
                s.buf.erase(s.buf.begin(), s.buf.begin() + qsizetype(drop));
                s.bufStart += double(drop) / kSampleRate;
            }
            return;
        }

        if (s.silenceSec >= kSilenceEndSec || s.speechSec >= kSpeechWindowSec ||
            windowSec >= kMaxWindowSec) {
            closeWindow(stream);
        } else if (s.sincePartialSec >= kPartialEverySec) {
            s.sincePartialSec = 0.0;
            emitWindow(stream, false);
        }
    }
};

TranscribeEngine::TranscribeEngine(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>(this)) {
    Impl* impl = d.get();
    impl->worker = std::thread([impl] { impl->run(); });
}

TranscribeEngine::~TranscribeEngine() {
    d->stop();
    d->freeModel();
}

bool TranscribeEngine::isAvailable() const {
    return d->available.load();
}

QString TranscribeEngine::modelPath() const {
    return d->modelPath();
}

void TranscribeEngine::setModelPath(const QString& path) {
    if (d->modelPath() == path) return;
    d->setPath(path);
    emit modelPathChanged();
    Impl* impl = d.get();
    impl->post([impl] {                    // lazy load on the worker
        impl->clearLoadFailure();
        impl->ensureModel();
    });
}

void TranscribeEngine::begin() {
    Impl* impl = d.get();
    impl->post([impl] {
        for (StreamState& s : impl->streams) s.reset();
        impl->systemTurn = 0;
        impl->clearLoadFailure();
        impl->ensureModel();
    });
}

void TranscribeEngine::feed(int stream, QByteArray f32Samples, double t0) {
    if (stream < 0 || stream > 1 || f32Samples.isEmpty()) return;

    const qint64 count = qint64(f32Samples.size()) / qint64(sizeof(float));
    if (d->queuedSamples.load() > qint64(kMaxQueuedSec * kSampleRate)) return;   // backpressure
    d->queuedSamples.fetch_add(count);

    Impl* impl = d.get();
    impl->post([impl, stream, f32Samples, t0, count] {
#ifdef GROMARCH_WITH_WHISPER
        impl->handleAudio(stream, f32Samples, t0);
#else
        Q_UNUSED(stream);
        Q_UNUSED(t0);
#endif
        impl->queuedSamples.fetch_sub(count);
    });
}

void TranscribeEngine::finish() {
    Impl* impl = d.get();
    impl->post([impl] {
#ifdef GROMARCH_WITH_WHISPER
        for (int stream = 0; stream < 2; ++stream) {
            StreamState& s = impl->streams[stream];
            if (s.hadSpeech && !s.buf.empty()) impl->emitWindow(stream, true);
            s.buf.clear();
            s.vadTail.clear();
            s.clearTurnTail();
            s.pendingTurn = false;
            s.resetWindow();
        }
#endif
        impl->emitFinished();
    });
}

} // namespace gromarch
