#include "models/ModelDownloader.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QStandardPaths>
#include <QUrl>
#include <QVariantMap>

namespace parfait {

namespace {

// A model file smaller than this is a leftover/partial, not something whisper
// could ever load — treat it as "not downloaded".
constexpr qint64 kMinModelBytes = 10ll * 1024 * 1024;

const QString kWhisperCpp =
    QStringLiteral("https://huggingface.co/ggerganov/whisper.cpp/resolve/main/");

QString settingsModelPath() {
    QSettings s(QStringLiteral("parfait"), QStringLiteral("parfait"));
    return s.value(QStringLiteral("transcribe/modelPath")).toString().trimmed();
}

} // namespace

ModelDownloader::ModelDownloader(QObject* parent) : QObject(parent) {
    m_catalog = {
        {QStringLiteral("ggml-base.en.bin"), QStringLiteral("Fast, lower accuracy"), 148,
         kWhisperCpp + QStringLiteral("ggml-base.en.bin")},
        {QStringLiteral("ggml-small.en.bin"), QStringLiteral("Better accuracy"), 488,
         kWhisperCpp + QStringLiteral("ggml-small.en.bin")},
        {QStringLiteral("ggml-small.en-tdrz.bin"), QStringLiteral("Speaker turns (recommended)"), 488,
         QStringLiteral("https://huggingface.co/akashmjn/tinydiarize-whisper.cpp/resolve/main/"
                        "ggml-small.en-tdrz.bin")},
        {QStringLiteral("ggml-large-v3-turbo.bin"), QStringLiteral("Best accuracy, needs GPU/fast CPU"),
         1600, kWhisperCpp + QStringLiteral("ggml-large-v3-turbo.bin")},
    };
    m_nam = new QNetworkAccessManager(this);
    m_activePath = settingsModelPath();
}

ModelDownloader::~ModelDownloader() {
    const QStringList names = m_jobs.keys();
    for (const QString& name : names) finishJob(name, false);
}

QString ModelDownloader::modelsDir() const {
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(QStringLiteral("parfait/models"));
}

QString ModelDownloader::pathFor(const QString& name) const {
    return QDir(modelsDir()).filePath(name);
}

const ModelDownloader::Entry* ModelDownloader::entryFor(const QString& name) const {
    for (const Entry& e : m_catalog)
        if (e.name == name) return &e;
    return nullptr;
}

bool ModelDownloader::isDownloaded(const QString& name) const {
    const QFileInfo fi(pathFor(name));
    return fi.exists() && fi.size() > kMinModelBytes;
}

QVariantList ModelDownloader::models() const {
    QVariantList out;
    out.reserve(m_catalog.size());
    for (const Entry& e : m_catalog) {
        const QString path = pathFor(e.name);
        const auto job = m_jobs.constFind(e.name);
        QVariantMap m;
        m[QStringLiteral("name")] = e.name;
        m[QStringLiteral("label")] = e.label;
        m[QStringLiteral("sizeMB")] = e.sizeMB;
        m[QStringLiteral("url")] = e.url;
        m[QStringLiteral("path")] = path;
        m[QStringLiteral("downloaded")] = isDownloaded(e.name);
        m[QStringLiteral("downloading")] = job != m_jobs.constEnd();
        m[QStringLiteral("progress")] = job != m_jobs.constEnd() ? job->progress : 0.0;
        m[QStringLiteral("active")] = !m_activePath.isEmpty()
                                      && QFileInfo(m_activePath) == QFileInfo(path);
        out.append(m);
    }
    return out;
}

bool ModelDownloader::isBusy() const { return m_busy; }

QString ModelDownloader::activeModelPath() const { return m_activePath; }

void ModelDownloader::refresh() { emit modelsChanged(); }

void ModelDownloader::updateBusy() {
    const bool busy = !m_jobs.isEmpty();
    if (busy == m_busy) return;
    m_busy = busy;
    emit busyChanged(m_busy);
}

// Tears down the reply/file for a job. keepPartial is only true while a
// successful download is being renamed into place.
void ModelDownloader::finishJob(const QString& name, bool keepPartial) {
    const auto it = m_jobs.find(name);
    if (it == m_jobs.end()) return;
    Job job = *it;
    m_jobs.erase(it);
    if (job.reply) {
        job.reply->disconnect(this);
        if (job.reply->isRunning()) job.reply->abort();
        job.reply->deleteLater();
    }
    if (job.file) {
        const QString partPath = job.file->fileName();
        job.file->close();
        delete job.file;
        if (!keepPartial) QFile::remove(partPath);
    }
    updateBusy();
}

void ModelDownloader::download(const QString& name) {
    const Entry* e = entryFor(name);
    if (!e) {
        emit error(tr("Unknown model %1").arg(name));
        return;
    }
    if (m_jobs.contains(name)) return;
    if (isDownloaded(name)) {
        emit modelsChanged();
        return;
    }

    QDir().mkpath(modelsDir());
    const QString partPath = pathFor(name) + QStringLiteral(".part");
    auto* file = new QFile(partPath);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit error(tr("Cannot write %1: %2").arg(partPath, file->errorString()));
        delete file;
        return;
    }

    QNetworkRequest req{QUrl(e->url)};
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(10);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("parfait"));

    QNetworkReply* reply = m_nam->get(req);
    Job job;
    job.reply = reply;
    job.file = file;
    m_jobs.insert(name, job);
    updateBusy();
    emit modelsChanged();

    connect(reply, &QNetworkReply::readyRead, this, [this, name] {
        const auto it = m_jobs.find(name);
        if (it == m_jobs.end() || !it->file || !it->reply) return;
        if (it->file->write(it->reply->readAll()) < 0) {
            const QString msg = it->file->errorString();
            finishJob(name, false);
            emit modelsChanged();
            emit error(tr("Write failed for %1: %2").arg(name, msg));
        }
    });

    connect(reply, &QNetworkReply::downloadProgress, this,
            [this, name](qint64 received, qint64 total) {
                const auto it = m_jobs.find(name);
                if (it == m_jobs.end()) return;
                // Fall back to the catalog size while the server withholds
                // Content-Length (chunked transfer through the CDN).
                const Entry* e = entryFor(name);
                const qint64 denom = total > 0 ? total
                                    : (e ? qint64(e->sizeMB) * 1024 * 1024 : 0);
                const qreal p = denom > 0 ? qBound(0.0, qreal(received) / qreal(denom), 1.0) : 0.0;
                it->progress = p;
                emit progressChanged(name, p);
            });

    connect(reply, &QNetworkReply::finished, this, [this, name, reply] {
        const auto it = m_jobs.find(name);
        if (it == m_jobs.end()) return;   // cancelled/torn down already
        const QNetworkReply::NetworkError err = reply->error();
        const QString errText = reply->errorString();
        if (it->file && reply->bytesAvailable() > 0) it->file->write(reply->readAll());
        const QString partPath = it->file ? it->file->fileName() : QString();
        const qint64 got = it->file ? it->file->size() : 0;

        if (err != QNetworkReply::NoError) {
            finishJob(name, false);
            emit modelsChanged();
            if (err != QNetworkReply::OperationCanceledError)
                emit error(tr("Download of %1 failed: %2").arg(name, errText));
            return;
        }
        if (got <= kMinModelBytes) {
            finishJob(name, false);
            emit modelsChanged();
            emit error(tr("Download of %1 was truncated (%2 bytes)").arg(name).arg(got));
            return;
        }

        finishJob(name, true);   // closes the file, keeps the .part
        const QString finalPath = pathFor(name);
        QFile::remove(finalPath);
        if (!QFile::rename(partPath, finalPath)) {
            QFile::remove(partPath);
            emit modelsChanged();
            emit error(tr("Could not move %1 into place").arg(name));
            return;
        }
        emit modelsChanged();
        emit downloadFinished(name);
    });
}

void ModelDownloader::cancel(const QString& name) {
    if (!m_jobs.contains(name)) return;
    finishJob(name, false);
    emit modelsChanged();
}

void ModelDownloader::remove(const QString& name) {
    if (!entryFor(name)) return;
    const QString path = pathFor(name);
    if (!m_activePath.isEmpty() && QFileInfo(m_activePath) == QFileInfo(path)) {
        emit error(tr("%1 is the active model — pick another one first").arg(name));
        return;
    }
    if (m_jobs.contains(name)) finishJob(name, false);
    if (QFile::exists(path) && !QFile::remove(path))
        emit error(tr("Could not delete %1").arg(name));
    emit modelsChanged();
}

void ModelDownloader::setActive(const QString& name) {
    if (!entryFor(name)) {
        emit error(tr("Unknown model %1").arg(name));
        return;
    }
    if (!isDownloaded(name)) {
        emit error(tr("%1 is not downloaded yet").arg(name));
        return;
    }
    const QString path = pathFor(name);
    if (m_activePath == path) return;
    m_activePath = path;
    QSettings s(QStringLiteral("parfait"), QStringLiteral("parfait"));
    s.setValue(QStringLiteral("transcribe/modelPath"), path);
    s.sync();
    emit activeModelPathChanged(path);
    emit modelsChanged();
}

} // namespace parfait
