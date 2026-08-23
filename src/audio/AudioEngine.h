#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include "Types.h"

namespace gromarch {

// Dual-stream PipeWire capture: default mic source + default sink monitor.
// Emits 16 kHz mono f32 frames per stream for transcription, and (when retention
// is enabled) encodes both streams to a stereo Ogg Opus file (L=mic, R=system).
// All PipeWire work happens on an internal thread; signals are queued to the caller.
class AudioEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool recording READ isRecording NOTIFY recordingChanged)
    Q_PROPERTY(float micLevel READ micLevel NOTIFY levelsChanged)
    Q_PROPERTY(float systemLevel READ systemLevel NOTIFY levelsChanged)
public:
    explicit AudioEngine(QObject* parent = nullptr);
    ~AudioEngine() override;

    bool isRecording() const;
    float micLevel() const;     // 0..1, updated ~15 Hz while recording
    float systemLevel() const;

public slots:
    // audioFilePath empty => transcribe-only, nothing written to disk.
    void start(const QString& audioFilePath);
    void stop();

signals:
    void recordingChanged(bool recording);
    void levelsChanged();
    // 16 kHz mono f32 PCM chunk for one stream; t0 = seconds from start().
    void pcmReady(int stream, QByteArray f32Samples, double t0);
    void error(const QString& message);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace gromarch
