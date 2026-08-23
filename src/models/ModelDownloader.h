#pragma once
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace parfait {

// In-app whisper.cpp model catalog + downloader.
// Models live in ~/.local/share/parfait/models/ (GenericDataLocation, matching
// TranscribeEngine's default model path). Downloads follow redirects (the
// HuggingFace resolve/ URLs redirect to a CDN), land in a <name>.part file and
// are renamed into place only on success, so a partial file never looks usable.
// The active model is persisted to QSettings("parfait","parfait") under
// transcribe/modelPath; main.cpp wires activeModelPathChanged into
// TranscribeEngine::setModelPath so switching applies live.
// Exposed to QML as the context property `Models`.
class ModelDownloader : public QObject {
    Q_OBJECT
    // One map per catalog entry: name, label, sizeMB, url, path, downloaded,
    // downloading, active, progress. Re-emitted whenever any of that changes.
    Q_PROPERTY(QVariantList models READ models NOTIFY modelsChanged)
    Q_PROPERTY(bool busy READ isBusy NOTIFY busyChanged)
    Q_PROPERTY(QString activeModelPath READ activeModelPath NOTIFY activeModelPathChanged)
    Q_PROPERTY(QString modelsDir READ modelsDir CONSTANT)
public:
    explicit ModelDownloader(QObject* parent = nullptr);
    ~ModelDownloader() override;

    QVariantList models() const;
    bool isBusy() const;                       // at least one download in flight
    QString activeModelPath() const;           // absolute path, empty if unset
    QString modelsDir() const;                 // ~/.local/share/parfait/models

    Q_INVOKABLE void download(const QString& name);
    Q_INVOKABLE void cancel(const QString& name);
    Q_INVOKABLE void remove(const QString& name);   // refuses the active model
    Q_INVOKABLE void setActive(const QString& name);
    Q_INVOKABLE void refresh();                     // re-stat the models dir

signals:
    void modelsChanged();
    void busyChanged(bool busy);
    // Fine-grained 0..1 progress for one model; modelsChanged only fires on
    // state transitions so a download does not rebuild the whole list per chunk.
    void progressChanged(const QString& name, qreal progress);
    void activeModelPathChanged(const QString& path);
    void downloadFinished(const QString& name);
    void error(const QString& message);

private:
    struct Entry {
        QString name;
        QString label;
        int sizeMB = 0;
        QString url;
    };
    struct Job {
        QNetworkReply* reply = nullptr;
        QFile* file = nullptr;
        qreal progress = 0.0;
    };

    const Entry* entryFor(const QString& name) const;
    QString pathFor(const QString& name) const;
    bool isDownloaded(const QString& name) const;
    void updateBusy();
    void finishJob(const QString& name, bool keepPartial);

    QVector<Entry> m_catalog;
    QHash<QString, Job> m_jobs;
    QNetworkAccessManager* m_nam = nullptr;
    QString m_activePath;
    bool m_busy = false;
};

} // namespace parfait
