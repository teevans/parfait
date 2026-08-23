#include "audio/AudioEngine.h"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QtGlobal>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/utils/result.h>

#include <opusenc.h>

namespace gromarch {
namespace {

constexpr uint32_t kRate = 16000;
constexpr uint32_t kChannels = 1;
constexpr size_t kChunkFrames = kRate / 2;              // ~0.5 s per pcmReady chunk
constexpr int kLevelIntervalMs = 66;                    // ~15 Hz
constexpr size_t kMaxSkewFrames = kRate / 2;            // pad a lagging stream beyond 0.5 s
constexpr opus_int32 kBitrate = 32000;

std::once_flag g_pwInit;

float rms(const float* p, size_t n) {
    if (n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; ++i) acc += double(p[i]) * double(p[i]);
    return float(std::clamp(std::sqrt(acc / double(n)), 0.0, 1.0));
}

void atomicMax(std::atomic<float>& slot, float v) {
    float cur = slot.load(std::memory_order_relaxed);
    while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {
    }
}

} // namespace

// ---------------------------------------------------------------------------

class AudioEngineImpl {
public:
    struct StreamCtx {
        AudioEngineImpl* d = nullptr;
        int index = 0;
        pw_stream* stream = nullptr;
        spa_hook listener{};
        std::vector<float> chunk;       // touched only from the PipeWire RT thread
        uint64_t framesEmitted = 0;     // frames already handed to pcmReady (t0 source)
        std::atomic<float> peak{0.0f};  // written by RT thread, drained by the level timer
        std::atomic<float> level{0.0f}; // published value read by micLevel()/systemLevel()
    };

    std::function<void(int, QByteArray, double)> emitPcm;
    std::function<void(const QString&)> emitError;
    std::function<void(bool)> emitRecording;
    std::function<void()> emitLevels;

    bool recording = false;
    pw_thread_loop* loop = nullptr;
    StreamCtx streams[2];
    QTimer* levelTimer = nullptr;

    // Encoder side. encQueue is filled from the RT callbacks and drained (interleaved)
    // by pump() on the object thread, so libopusenc never runs in the RT path.
    std::mutex encMutex;
    std::vector<float> encQueue[2];
    OggOpusEnc* enc = nullptr;

    void feed(StreamCtx& s, const float* samples, size_t n);
    void pump(bool flushAll);
    bool openEncoder(const QString& path, QString* err);
    void closeEncoder();
    void teardownPipeWire();
};

namespace {

void onStreamProcess(void* userdata) {
    auto* s = static_cast<AudioEngineImpl::StreamCtx*>(userdata);
    pw_buffer* b = pw_stream_dequeue_buffer(s->stream);
    if (!b) return;

    spa_buffer* buf = b->buffer;
    if (buf->n_datas > 0 && buf->datas[0].data != nullptr && buf->datas[0].chunk != nullptr) {
        const uint32_t maxsize = buf->datas[0].maxsize;
        const uint32_t offset = std::min(buf->datas[0].chunk->offset, maxsize);
        const uint32_t size = std::min(buf->datas[0].chunk->size, maxsize - offset);
        const auto* base = static_cast<const uint8_t*>(buf->datas[0].data) + offset;
        s->d->feed(*s, reinterpret_cast<const float*>(base), size / sizeof(float));
    }
    pw_stream_queue_buffer(s->stream, b);
}

void onStreamStateChanged(void* userdata, enum pw_stream_state /*old*/, enum pw_stream_state state,
                          const char* message) {
    auto* s = static_cast<AudioEngineImpl::StreamCtx*>(userdata);
    if (state == PW_STREAM_STATE_ERROR && s->d->emitError) {
        s->d->emitError(QStringLiteral("PipeWire %1 stream error: %2")
                            .arg(s->index == 0 ? QStringLiteral("microphone")
                                               : QStringLiteral("system audio"),
                                 QString::fromUtf8(message ? message : "unknown")));
    }
}

const pw_stream_events kStreamEvents = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = onStreamStateChanged,
    .process = onStreamProcess,
};

} // namespace

void AudioEngineImpl::feed(StreamCtx& s, const float* samples, size_t n) {
    if (n == 0) return;
    atomicMax(s.peak, rms(samples, n));

    {
        std::lock_guard<std::mutex> lock(encMutex);
        if (enc) encQueue[s.index].insert(encQueue[s.index].end(), samples, samples + n);
    }

    s.chunk.insert(s.chunk.end(), samples, samples + n);
    size_t consumed = 0;
    while (s.chunk.size() - consumed >= kChunkFrames) {
        const float* p = s.chunk.data() + consumed;
        QByteArray bytes(reinterpret_cast<const char*>(p), qsizetype(kChunkFrames * sizeof(float)));
        if (emitPcm) emitPcm(s.index, std::move(bytes), double(s.framesEmitted) / double(kRate));
        s.framesEmitted += kChunkFrames;
        consumed += kChunkFrames;
    }
    if (consumed > 0) s.chunk.erase(s.chunk.begin(), s.chunk.begin() + qsizetype(consumed));
}

// Interleaves both queues into the stereo file (L=mic, R=system). Streams are aligned by
// sample count; whichever side lags by more than kMaxSkewFrames is padded with silence.
void AudioEngineImpl::pump(bool flushAll) {
    std::vector<float> interleaved;
    {
        std::lock_guard<std::mutex> lock(encMutex);
        if (!enc) return;
        const size_t a = encQueue[0].size();
        const size_t b = encQueue[1].size();
        const size_t lead = std::max(a, b);
        size_t n = std::min(a, b);
        if (flushAll) {
            n = lead;
        } else if (lead - n > kMaxSkewFrames) {
            n = lead - kMaxSkewFrames;
        }
        if (n == 0) return;

        interleaved.resize(n * 2);
        for (size_t i = 0; i < n; ++i) {
            interleaved[i * 2] = i < a ? encQueue[0][i] : 0.0f;
            interleaved[i * 2 + 1] = i < b ? encQueue[1][i] : 0.0f;
        }
        for (int s = 0; s < 2; ++s) {
            const size_t drop = std::min(n, encQueue[s].size());
            encQueue[s].erase(encQueue[s].begin(), encQueue[s].begin() + qsizetype(drop));
        }
    }

    const int frames = int(interleaved.size() / 2);
    std::lock_guard<std::mutex> lock(encMutex);
    if (enc && frames > 0) {
        const int rc = ope_encoder_write_float(enc, interleaved.data(), frames);
        if (rc != OPE_OK && emitError) {
            emitError(QStringLiteral("Opus encode failed: %1").arg(QString::fromUtf8(ope_strerror(rc))));
        }
    }
}

bool AudioEngineImpl::openEncoder(const QString& path, QString* err) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    OggOpusComments* comments = ope_comments_create();
    if (!comments) {
        *err = QStringLiteral("Could not allocate Opus comments");
        return false;
    }
    ope_comments_add(comments, "ENCODER", "gromarch");
    ope_comments_add(comments, "DESCRIPTION", "L=microphone R=system audio");

    int rc = OPE_OK;
    enc = ope_encoder_create_file(path.toUtf8().constData(), comments, int(kRate), 2, 0, &rc);
    ope_comments_destroy(comments);
    if (!enc) {
        *err = QStringLiteral("Could not open %1: %2").arg(path, QString::fromUtf8(ope_strerror(rc)));
        return false;
    }
    ope_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrate));
    ope_encoder_ctl(enc, OPUS_SET_VBR(1));
    return true;
}

void AudioEngineImpl::closeEncoder() {
    OggOpusEnc* e = nullptr;
    {
        std::lock_guard<std::mutex> lock(encMutex);
        e = enc;
        enc = nullptr;
        encQueue[0].clear();
        encQueue[1].clear();
    }
    if (!e) return;
    ope_encoder_drain(e);
    ope_encoder_destroy(e);
}

void AudioEngineImpl::teardownPipeWire() {
    if (loop) pw_thread_loop_stop(loop);
    for (auto& s : streams) {
        if (s.stream) {
            spa_hook_remove(&s.listener);
            pw_stream_destroy(s.stream);
            s.stream = nullptr;
        }
    }
    if (loop) {
        pw_thread_loop_destroy(loop);
        loop = nullptr;
    }
}

// ---------------------------------------------------------------------------

namespace {

std::map<const AudioEngine*, AudioEngineImpl*>& registry() {
    static std::map<const AudioEngine*, AudioEngineImpl*> map;
    return map;
}

std::mutex& registryMutex() {
    static std::mutex m;
    return m;
}

AudioEngineImpl* impl(const AudioEngine* q) {
    std::lock_guard<std::mutex> lock(registryMutex());
    auto it = registry().find(q);
    return it == registry().end() ? nullptr : it->second;
}

} // namespace

AudioEngine::AudioEngine(QObject* parent) : QObject(parent) {
    auto* d = new AudioEngineImpl;
    d->emitPcm = [this](int stream, QByteArray bytes, double t0) {
        emit pcmReady(stream, std::move(bytes), t0);
    };
    d->emitError = [this](const QString& message) { emit error(message); };
    d->emitRecording = [this](bool on) { emit recordingChanged(on); };
    d->emitLevels = [this] { emit levelsChanged(); };

    d->levelTimer = new QTimer(this);
    d->levelTimer->setInterval(kLevelIntervalMs);
    connect(d->levelTimer, &QTimer::timeout, this, [d] {
        for (auto& s : d->streams) s.level.store(s.peak.exchange(0.0f, std::memory_order_relaxed),
                                                 std::memory_order_relaxed);
        d->pump(false);
        if (d->emitLevels) d->emitLevels();
    });

    for (int i = 0; i < 2; ++i) {
        d->streams[i].d = d;
        d->streams[i].index = i;
    }

    std::lock_guard<std::mutex> lock(registryMutex());
    registry()[this] = d;
}

AudioEngine::~AudioEngine() {
    stop();
    AudioEngineImpl* d = nullptr;
    {
        std::lock_guard<std::mutex> lock(registryMutex());
        auto it = registry().find(this);
        if (it != registry().end()) {
            d = it->second;
            registry().erase(it);
        }
    }
    delete d;
}

bool AudioEngine::isRecording() const {
    auto* d = impl(this);
    return d && d->recording;
}

float AudioEngine::micLevel() const {
    auto* d = impl(this);
    return d ? d->streams[0].level.load(std::memory_order_relaxed) : 0.0f;
}

float AudioEngine::systemLevel() const {
    auto* d = impl(this);
    return d ? d->streams[1].level.load(std::memory_order_relaxed) : 0.0f;
}

void AudioEngine::start(const QString& audioFilePath) {
    auto* d = impl(this);
    if (!d) return;
    if (d->recording) {
        qWarning("AudioEngine::start() called while already recording - ignored");
        return;
    }

    for (auto& s : d->streams) {
        s.chunk.clear();
        s.framesEmitted = 0;
        s.peak.store(0.0f, std::memory_order_relaxed);
        s.level.store(0.0f, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(d->encMutex);
        d->encQueue[0].clear();
        d->encQueue[1].clear();
    }

    if (!audioFilePath.isEmpty()) {
        QString err;
        if (!d->openEncoder(audioFilePath, &err)) {
            emit error(err);
            return; // transcription still needs the streams, but the caller asked for a file
        }
    }

    std::call_once(g_pwInit, [] { pw_init(nullptr, nullptr); });

    d->loop = pw_thread_loop_new("gromarch-audio", nullptr);
    if (!d->loop) {
        d->closeEncoder();
        emit error(QStringLiteral("Could not create the PipeWire thread loop"));
        return;
    }

    for (int i = 0; i < 2; ++i) {
        const bool sink = (i == 1);
        pw_properties* props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_ROLE, "Communication",
            PW_KEY_NODE_NAME, sink ? "gromarch-system" : "gromarch-mic",
            nullptr);
        if (!props) {
            d->teardownPipeWire();
            d->closeEncoder();
            emit error(QStringLiteral("Out of memory creating PipeWire stream properties"));
            return;
        }
        // Capture what the sink is playing back (the remote meeting participants).
        if (sink) pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

        d->streams[i].stream = pw_stream_new_simple(pw_thread_loop_get_loop(d->loop),
                                                    sink ? "gromarch system audio" : "gromarch microphone",
                                                    props, &kStreamEvents, &d->streams[i]);
        if (!d->streams[i].stream) {
            d->teardownPipeWire();
            d->closeEncoder();
            emit error(QStringLiteral("Could not create the PipeWire capture stream"));
            return;
        }

        uint8_t podBuffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kRate;
        info.channels = kChannels;
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};

        const int rc = pw_stream_connect(
            d->streams[i].stream, PW_DIRECTION_INPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                         PW_STREAM_FLAG_RT_PROCESS),
            params, 1);
        if (rc < 0) {
            d->teardownPipeWire();
            d->closeEncoder();
            emit error(QStringLiteral("Could not connect the %1 stream: %2")
                           .arg(sink ? QStringLiteral("system audio") : QStringLiteral("microphone"),
                                QString::fromUtf8(spa_strerror(rc))));
            return;
        }
    }

    if (pw_thread_loop_start(d->loop) < 0) {
        d->teardownPipeWire();
        d->closeEncoder();
        emit error(QStringLiteral("Could not start the PipeWire thread loop"));
        return;
    }

    d->recording = true;
    d->levelTimer->start();
    emit recordingChanged(true);
}

void AudioEngine::stop() {
    auto* d = impl(this);
    if (!d || !d->recording) return;

    d->recording = false;
    d->levelTimer->stop();
    d->teardownPipeWire(); // joins the RT thread; stream state is ours again afterwards

    for (auto& s : d->streams) {
        if (!s.chunk.empty()) {
            QByteArray bytes(reinterpret_cast<const char*>(s.chunk.data()),
                             qsizetype(s.chunk.size() * sizeof(float)));
            emit pcmReady(s.index, bytes, double(s.framesEmitted) / double(kRate));
            s.framesEmitted += s.chunk.size();
            s.chunk.clear();
        }
        s.peak.store(0.0f, std::memory_order_relaxed);
        s.level.store(0.0f, std::memory_order_relaxed);
    }

    d->pump(true);
    d->closeEncoder();

    emit levelsChanged();
    emit recordingChanged(false);
}

} // namespace gromarch
