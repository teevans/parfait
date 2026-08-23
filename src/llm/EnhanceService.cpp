#include "llm/EnhanceService.h"
#include "llm/Prompts.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QTimer>
#include <QUrl>

namespace gromarch {

namespace {

// Idle/connect budget: if the server sends nothing at all for this long, give up.
constexpr int kConnectTimeoutMs = 30000;
// Total transfer budget as a backstop for a server that dribbles bytes forever.
constexpr int kTransferTimeoutMs = 300000;

enum class Job { None, Enhance, Title };

QString settingsBaseUrl() {
    QSettings s(QStringLiteral("gromarch"), QStringLiteral("gromarch"));
    QString url = s.value(QStringLiteral("llm/baseUrl")).toString().trimmed();
    while (url.endsWith(QLatin1Char('/'))) url.chop(1);
    return url;
}

QString settingsValue(const char* key) {
    QSettings s(QStringLiteral("gromarch"), QStringLiteral("gromarch"));
    return s.value(QString::fromLatin1(key)).toString().trimmed();
}

// Pull {"error":{"message":...}} out of a server body, if that's what it is.
QString errorFromBody(const QByteArray& body) {
    if (body.isEmpty()) return QString();
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonValue err = doc.object().value(QStringLiteral("error"));
        if (err.isObject()) {
            const QString msg = err.toObject().value(QStringLiteral("message")).toString();
            if (!msg.isEmpty()) return msg;
        } else if (err.isString()) {
            return err.toString();
        }
        const QString msg = doc.object().value(QStringLiteral("message")).toString();
        if (!msg.isEmpty()) return msg;
    }
    QString text = QString::fromUtf8(body).trimmed();
    if (text.size() > 400) text = text.left(400) + QStringLiteral("...");
    return text;
}

QString cleanTitle(QString t) {
    t = t.trimmed();
    // Models like to wrap titles in quotes and end them with a period.
    while (t.size() >= 2 &&
           ((t.startsWith(QLatin1Char('"')) && t.endsWith(QLatin1Char('"'))) ||
            (t.startsWith(QLatin1Char('\'')) && t.endsWith(QLatin1Char('\''))) ||
            (t.startsWith(QChar(0x201C)) && t.endsWith(QChar(0x201D))))) {
        t = t.mid(1, t.size() - 2).trimmed();
    }
    while (t.endsWith(QLatin1Char('.')) || t.endsWith(QLatin1Char(';'))) {
        t.chop(1);
        t = t.trimmed();
    }
    return t;
}

} // namespace

struct EnhanceService::Impl {
    QNetworkAccessManager nam;
    QNetworkReply* reply = nullptr;
    QTimer stall;
    QByteArray sseBuffer;
    QString accumulated;
    Job job = Job::None;
    bool busy = false;
    bool cancelled = false;
    bool sawData = false;
    bool configured = false;

    // Re-read at every request start so Settings UI edits apply live.
    QString baseUrl;
    QString apiKey;
    QString model;
    QString fastModel;
};

EnhanceService::EnhanceService(QObject* parent)
    : QObject(parent), d(std::make_unique<Impl>()) {
    d->configured = isConfigured();
    d->stall.setSingleShot(true);
}

EnhanceService::~EnhanceService() {
    if (d->reply) {
        d->reply->disconnect();
        d->reply->abort();
        d->reply->deleteLater();
        d->reply = nullptr;
    }
}

bool EnhanceService::isBusy() const {
    return d->busy;
}

bool EnhanceService::isConfigured() const {
    return !settingsBaseUrl().isEmpty() && !settingsValue("llm/model").isEmpty();
}

void EnhanceService::cancel() {
    Impl* p = d.get();
    if (!p->reply) return;
    p->cancelled = true;
    p->stall.stop();
    QNetworkReply* r = p->reply;
    p->reply = nullptr;
    r->disconnect();
    r->abort();
    r->deleteLater();
    p->job = Job::None;
    if (p->busy) {
        p->busy = false;
        emit busyChanged(false);
    }
}

void EnhanceService::enhance(const QString& cuesMd, const QString& transcriptText,
                             const QString& templateId) {
    Impl* p = d.get();

    cancel();   // one job at a time — a new enhance supersedes the running one

    p->baseUrl = settingsBaseUrl();
    p->apiKey = settingsValue("llm/apiKey");
    p->model = settingsValue("llm/model");
    p->fastModel = settingsValue("llm/fastModel");
    const bool cfg = !p->baseUrl.isEmpty() && !p->model.isEmpty();
    if (cfg != p->configured) {
        p->configured = cfg;
        emit configuredChanged(cfg);
    }
    if (!cfg) {
        emit error(tr("LLM endpoint is not configured — set a base URL and model in Settings."));
        return;
    }

    QJsonArray messages;
    QJsonObject sys;
    sys.insert(QStringLiteral("role"), QStringLiteral("system"));
    sys.insert(QStringLiteral("content"), prompts::systemPrompt(templateId));
    messages.append(sys);
    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), prompts::userMessage(cuesMd, transcriptText));
    messages.append(user);

    QJsonObject body;
    body.insert(QStringLiteral("model"), p->model);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), true);
    body.insert(QStringLiteral("temperature"), 0.4);

    p->job = Job::Enhance;
    p->accumulated.clear();
    p->sseBuffer.clear();
    p->cancelled = false;
    p->sawData = false;

    QNetworkRequest req{QUrl(p->baseUrl + QStringLiteral("/chat/completions"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    req.setRawHeader("Accept", "text/event-stream");
    if (!p->apiKey.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + p->apiKey.toUtf8());
    req.setTransferTimeout(kTransferTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    QNetworkReply* r = p->nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    p->reply = r;

    // Nothing received within the connect budget => treat as a timeout.
    p->stall.disconnect();
    p->stall.start(kConnectTimeoutMs);
    connect(&p->stall, &QTimer::timeout, this, [this, p, r] {
        if (p->reply != r || p->sawData) return;
        p->cancelled = true;
        p->reply = nullptr;
        r->disconnect();
        r->abort();
        r->deleteLater();
        p->job = Job::None;
        if (p->busy) {
            p->busy = false;
            emit busyChanged(false);
        }
        emit error(tr("The LLM endpoint did not respond within 30 seconds."));
    });

    connect(r, &QNetworkReply::readyRead, this, [this, p, r] {
        if (p->reply != r) return;
        p->sawData = true;
        p->stall.stop();
        p->sseBuffer.append(r->readAll());

        // SSE events are line-oriented; keep any trailing partial line buffered.
        int nl;
        while ((nl = p->sseBuffer.indexOf('\n')) >= 0) {
            QByteArray line = p->sseBuffer.left(nl);
            p->sseBuffer.remove(0, nl + 1);
            if (line.endsWith('\r')) line.chop(1);
            if (line.isEmpty() || line.startsWith(':')) continue;
            if (!line.startsWith("data:")) continue;

            const QByteArray payload = line.mid(5).trimmed();
            if (payload.isEmpty()) continue;
            if (payload == "[DONE]") continue;

            const QJsonObject obj = QJsonDocument::fromJson(payload).object();
            if (obj.contains(QStringLiteral("error"))) {
                const QString msg = errorFromBody(payload);
                p->cancelled = true;      // suppress the finished-handler error
                p->reply = nullptr;
                r->disconnect();
                r->abort();
                r->deleteLater();
                p->job = Job::None;
                if (p->busy) {
                    p->busy = false;
                    emit busyChanged(false);
                }
                emit error(msg.isEmpty() ? tr("The LLM endpoint returned an error.") : msg);
                return;
            }

            const QJsonArray choices = obj.value(QStringLiteral("choices")).toArray();
            if (choices.isEmpty()) continue;
            const QJsonObject choice = choices.at(0).toObject();
            QString delta = choice.value(QStringLiteral("delta"))
                                .toObject()
                                .value(QStringLiteral("content"))
                                .toString();
            if (delta.isEmpty()) {
                // Some servers replay the whole message on the final chunk.
                delta = choice.value(QStringLiteral("message"))
                            .toObject()
                            .value(QStringLiteral("content"))
                            .toString();
                if (delta.isEmpty()) continue;
            }
            p->accumulated += delta;
            emit enhanceDelta(delta);
        }
    });

    connect(r, &QNetworkReply::finished, this, [this, p, r] {
        if (p->reply != r) { r->deleteLater(); return; }
        p->stall.stop();
        p->reply = nullptr;
        const QNetworkReply::NetworkError err = r->error();
        const QByteArray tail = r->readAll();
        const int status =
            r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString netMsg = r->errorString();
        r->deleteLater();

        p->job = Job::None;
        if (p->busy) {
            p->busy = false;
            emit busyChanged(false);
        }
        if (p->cancelled) return;

        if (err != QNetworkReply::NoError) {
            QString msg = errorFromBody(tail);
            if (msg.isEmpty()) msg = netMsg;
            if (status > 0) msg = tr("HTTP %1: %2").arg(status).arg(msg);
            emit error(msg);
            return;
        }
        if (p->accumulated.isEmpty()) {
            emit error(tr("The LLM endpoint returned an empty response."));
            return;
        }
        emit enhanceFinished(p->accumulated);
    });

    p->busy = true;
    emit busyChanged(true);
}

void EnhanceService::suggestTitle(const QString& transcriptText) {
    Impl* p = d.get();
    if (transcriptText.trimmed().isEmpty()) return;

    const QString baseUrl = settingsBaseUrl();
    const QString apiKey = settingsValue("llm/apiKey");
    const QString model = settingsValue("llm/model");
    const QString fast = settingsValue("llm/fastModel");
    if (baseUrl.isEmpty() || model.isEmpty()) {
        emit error(tr("LLM endpoint is not configured — set a base URL and model in Settings."));
        return;
    }

    QJsonArray messages;
    QJsonObject sys;
    sys.insert(QStringLiteral("role"), QStringLiteral("system"));
    sys.insert(QStringLiteral("content"), prompts::titleSystem());
    messages.append(sys);
    QJsonObject user;
    user.insert(QStringLiteral("role"), QStringLiteral("user"));
    user.insert(QStringLiteral("content"), prompts::titleUserMessage(transcriptText));
    messages.append(user);

    QJsonObject body;
    body.insert(QStringLiteral("model"), fast.isEmpty() ? model : fast);
    body.insert(QStringLiteral("messages"), messages);
    body.insert(QStringLiteral("stream"), false);
    body.insert(QStringLiteral("temperature"), 0.3);
    body.insert(QStringLiteral("max_tokens"), 32);

    QNetworkRequest req{QUrl(baseUrl + QStringLiteral("/chat/completions"))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!apiKey.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + apiKey.toUtf8());
    req.setTransferTimeout(kConnectTimeoutMs);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);

    // Titles are a side job: they never touch busy state or the enhance reply slot.
    QNetworkReply* r = p->nam.post(req, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(r, &QNetworkReply::finished, this, [this, r] {
        const QNetworkReply::NetworkError err = r->error();
        const QByteArray payload = r->readAll();
        const QString netMsg = r->errorString();
        r->deleteLater();

        if (err == QNetworkReply::OperationCanceledError) return;
        if (err != QNetworkReply::NoError) {
            QString msg = errorFromBody(payload);
            emit error(msg.isEmpty() ? netMsg : msg);
            return;
        }
        const QJsonArray choices =
            QJsonDocument::fromJson(payload).object().value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) return;
        const QJsonObject choice = choices.at(0).toObject();
        QString text = choice.value(QStringLiteral("message"))
                           .toObject()
                           .value(QStringLiteral("content"))
                           .toString();
        if (text.isEmpty())
            text = choice.value(QStringLiteral("text")).toString();
        text = cleanTitle(text.section(QLatin1Char('\n'), 0, 0));
        if (!text.isEmpty()) emit titleReady(text);
    });
}

} // namespace gromarch
