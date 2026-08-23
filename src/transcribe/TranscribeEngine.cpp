#include "TranscribeEngine.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMetaObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
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

    void resetWindow() {
        speechSec = 0.0;
        silenceSec = 0.0;
        hadSpeech = false;
        sincePartialSec = 0.0;
        partialOpen = false;
    }

    void reset() {
        buf.clear();
        vadTail.clear();
        bufStart = 0.0;
        clock = 0.0;
        started = false;
        noiseFloor = kVadFloor;
        resetWindow();
    }
};

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

// The frozen header carries no data members, so per-instance state lives here.
struct Impl;
std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}
QHash<const TranscribeEngine*, Impl*>& registry() {
    static QHash<const TranscribeEngine*, Impl*> r;
    return r;
}

struct Impl {
    // Signal trampolines; set up by the constructor, invoked on the worker thread.
    std::function<void(const TranscriptSegment&)> emitSegment;
    std::function<void()> emitFinished;
    std::function<void(const QString&)> emitError;
    std::function<void(bool)> emitAvailability;

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

#ifdef GROMARCH_WITH_WHISPER
    std::mutex ctxMutex;
    whisper_context* ctx = nullptr;
    bool loadFailed = false;
#endif

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
        available.store(ok);
        if (emitAvailability) emitAvailability(ok);
        if (!ok && emitError) emitError(failure);
        return ok;
#else
        if (available.exchange(false) && emitAvailability) emitAvailability(false);
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
#ifdef GROMARCH_WITH_WHISPER
        std::lock_guard<std::mutex> lock(ctxMutex);
        if (ctx) whisper_free(ctx);
        ctx = nullptr;
        loadedPath.clear();
        loadFailed = false;
#endif
    }

    // ---- decoding ----------------------------------------------------------

    QString decode(const std::vector<float>& pcm) {
#ifdef GROMARCH_WITH_WHISPER
        if (pcm.empty()) return QString();
        if (!ensureModel()) return QString();

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

        QString text;
        {
            std::lock_guard<std::mutex> lock(ctxMutex);
            if (!ctx) return QString();
            if (whisper_full(ctx, wparams, samples->data(), int(samples->size())) != 0) {
                if (emitError) emitError(QStringLiteral("whisper_full() failed"));
                return QString();
            }
            const int n = whisper_full_n_segments(ctx);
            for (int i = 0; i < n; ++i) {
                const char* piece = whisper_full_get_segment_text(ctx, i);
                if (piece) text += QString::fromUtf8(piece);
            }
        }
        return cleanupText(text);
#else
        Q_UNUSED(pcm);
        return QString();
#endif
    }

    void emitWindow(int stream, bool final) {
        StreamState& s = streams[stream];
        if (s.buf.empty()) return;

        const QString text = decode(s.buf);
        if (text.isEmpty() && !(final && s.partialOpen)) return;

        TranscriptSegment seg;
        seg.stream = stream;
        seg.t0 = s.bufStart;
        seg.t1 = s.bufStart + double(s.buf.size()) / kSampleRate;
        seg.text = text;
        seg.final = final;
        if (!final) s.partialOpen = true;
        if (emitSegment) emitSegment(seg);
    }

    // Finalize the open window, then keep a short overlap so a word split across
    // the boundary still has context in the next window.
    void closeWindow(int stream) {
        StreamState& s = streams[stream];
        if (s.hadSpeech) emitWindow(stream, true);

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

Impl* implFor(const TranscribeEngine* engine) {
    std::lock_guard<std::mutex> lock(registryMutex());
    return registry().value(engine, nullptr);
}

} // namespace

TranscribeEngine::TranscribeEngine(QObject* parent) : QObject(parent) {
    auto* d = new Impl;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().insert(this, d);
    }

    // Marshal everything back onto this object's thread before emitting.
    d->emitSegment = [this](const TranscriptSegment& seg) {
        QMetaObject::invokeMethod(this, [this, seg] { emit segmentReady(seg); }, Qt::QueuedConnection);
    };
    d->emitFinished = [this] {
        QMetaObject::invokeMethod(this, [this] { emit finished(); }, Qt::QueuedConnection);
    };
    d->emitError = [this](const QString& message) {
        QMetaObject::invokeMethod(this, [this, message] { emit error(message); }, Qt::QueuedConnection);
    };
    d->emitAvailability = [this](bool ok) {
        QMetaObject::invokeMethod(this, [this, ok] { emit availabilityChanged(ok); }, Qt::QueuedConnection);
    };

    d->worker = std::thread([d] { d->run(); });
}

TranscribeEngine::~TranscribeEngine() {
    Impl* d = implFor(this);
    if (!d) return;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        registry().remove(this);
    }
    d->stop();
    d->freeModel();
    delete d;
}

bool TranscribeEngine::isAvailable() const {
    Impl* d = implFor(this);
    return d && d->available.load();
}

QString TranscribeEngine::modelPath() const {
    Impl* d = implFor(this);
    return d ? d->modelPath() : QString();
}

void TranscribeEngine::setModelPath(const QString& path) {
    Impl* d = implFor(this);
    if (!d) return;
    if (d->modelPath() == path) return;
    d->setPath(path);
    emit modelPathChanged();
    d->post([d] {                          // lazy load on the worker
        d->clearLoadFailure();
        d->ensureModel();
    });
}

void TranscribeEngine::begin() {
    Impl* d = implFor(this);
    if (!d) return;
    d->post([d] {
        for (StreamState& s : d->streams) s.reset();
        d->clearLoadFailure();
        d->ensureModel();
    });
}

void TranscribeEngine::feed(int stream, QByteArray f32Samples, double t0) {
    Impl* d = implFor(this);
    if (!d || stream < 0 || stream > 1 || f32Samples.isEmpty()) return;

    const qint64 count = qint64(f32Samples.size()) / qint64(sizeof(float));
    if (d->queuedSamples.load() > qint64(kMaxQueuedSec * kSampleRate)) return;   // backpressure
    d->queuedSamples.fetch_add(count);

    d->post([d, stream, f32Samples, t0, count] {
#ifdef GROMARCH_WITH_WHISPER
        d->handleAudio(stream, f32Samples, t0);
#else
        Q_UNUSED(stream);
        Q_UNUSED(t0);
#endif
        d->queuedSamples.fetch_sub(count);
    });
}

void TranscribeEngine::finish() {
    Impl* d = implFor(this);
    if (!d) return;
    d->post([d] {
#ifdef GROMARCH_WITH_WHISPER
        for (int stream = 0; stream < 2; ++stream) {
            StreamState& s = d->streams[stream];
            if (s.hadSpeech && !s.buf.empty()) d->emitWindow(stream, true);
            s.buf.clear();
            s.vadTail.clear();
            s.resetWindow();
        }
#endif
        if (d->emitFinished) d->emitFinished();
    });
}

} // namespace gromarch
