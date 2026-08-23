#pragma once
#include <QObject>
#include <QString>

namespace gromarch {

// Client for any OpenAI-compatible /chat/completions endpoint (streaming SSE).
// Configured via Settings (base URL, API key, model). One job at a time.
class EnhanceService : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(bool configured READ isConfigured NOTIFY configuredChanged)
public:
    explicit EnhanceService(QObject* parent = nullptr);
    ~EnhanceService() override;

    bool isBusy() const;
    bool isConfigured() const;   // base URL + model set

public slots:
    // Merge the user's rough cues with the transcript into a polished Markdown
    // note shaped by templateId. Streams deltas via enhanceDelta, then
    // enhanceFinished with the full text.
    void enhance(const QString& cuesMd, const QString& transcriptText,
                 const QString& templateId);
    // Short title suggestion from the transcript (fast model if configured).
    void suggestTitle(const QString& transcriptText);
    void cancel();

signals:
    void busyChanged(bool busy);
    void configuredChanged(bool configured);
    void enhanceDelta(const QString& textDelta);
    void enhanceFinished(const QString& fullMarkdown);
    void titleReady(const QString& title);
    void error(const QString& message);
};

} // namespace gromarch
