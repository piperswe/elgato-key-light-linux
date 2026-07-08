#include "httptransport.h"

#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

namespace elg {

HttpTransport::HttpTransport(QObject* parent)
    : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

static QNetworkRequest makeRequest(const KeyLight& light, const char* endpoint) {
    QUrl url = light.baseUrl();
    url.setPath(QString::fromLatin1(endpoint));
    QNetworkRequest req(url);
    req.setTransferTimeout(kHttpTimeoutMs);
    return req;
}

void HttpTransport::getInfo(const QString& id, const KeyLight& light) {
    track(m_nam->get(makeRequest(light, kEpInfo)), id, RequestKind::Info);
}

void HttpTransport::getLights(const QString& id, const KeyLight& light) {
    track(m_nam->get(makeRequest(light, kEpLights)), id, RequestKind::State);
}

void HttpTransport::putLights(const QString& id, const KeyLight& light,
                              const LightState& state) {
    QJsonObject body = buildLightsBody(light, state);

    QNetworkRequest req = makeRequest(light, kEpLights);
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  QStringLiteral("application/json"));
    QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    track(m_nam->put(req, payload), id, RequestKind::Put);
}

void HttpTransport::track(QNetworkReply* reply, const QString& id,
                          RequestKind kind) {
    m_pending.insert(reply);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, id, kind]() { onFinished(reply, id, kind); });
}

void HttpTransport::onFinished(QNetworkReply* reply, const QString& id,
                               RequestKind kind) {
    m_pending.remove(reply);
    reply->deleteLater();

    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const bool ok =
        reply->error() == QNetworkReply::NoError && status >= 200 && status < 300;
    if (!ok) {
        emit requestFailed(id, kind);
        return;
    }

    if (kind == RequestKind::Put) {
        emit putSucceeded(id);
        return;
    }

    const QByteArray body = reply->readAll();
    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        emit requestFailed(id, kind);
        return;
    }
    if (kind == RequestKind::Info) {
        emit infoReceived(id, doc.object());
    } else {
        emit stateReceived(id, doc.object());
    }
}

void HttpTransport::abortAll() {
    const auto pending = m_pending;
    for (QNetworkReply* reply : pending) {
        reply->abort();
    }
}

}  // namespace elg
