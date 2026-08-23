#pragma once
#include <QObject>
#include <QString>
#include <memory>
#include "Types.h"

namespace parfait {

// Streaming transcription over two PCM streams (mic + system) via whisper.cpp.
// Windowed decoding with VAD gating; emits partial segments that are replaced
// until finalized. Runs on an internal worker thread.
// When built without PARFAIT_WITH_WHISPER, acts as a stub that emits nothing
// and reports ready(false, ...).
class TranscribeEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool available READ isAvailable NOTIFY availabilityChanged)
    Q_PROPERTY(QString modelPath READ modelPath WRITE setModelPath NOTIFY modelPathChanged)
public:
    explicit TranscribeEngine(QObject* parent = nullptr);
    ~TranscribeEngine() override;

    bool isAvailable() const;         // model loaded and ready
    QString modelPath() const;
    void setModelPath(const QString& path);   // ggml/gguf model file

public slots:
    void begin();                     // reset stream state for a new meeting
    void feed(int stream, QByteArray f32Samples, double t0);  // connect to AudioEngine::pcmReady
    void finish();                    // flush pending audio, finalize all segments

signals:
    void availabilityChanged(bool available);
    void modelPathChanged();
    // Partial segments carry final=false and update the previous partial for
    // that stream; final=true segments are stable and should be persisted.
    void segmentReady(parfait::TranscriptSegment segment);
    void finished();                  // all segments final after finish()
    void error(const QString& message);

private:
    struct Impl;                      // worker thread, whisper context, window state
    std::unique_ptr<Impl> d;
};

} // namespace parfait
