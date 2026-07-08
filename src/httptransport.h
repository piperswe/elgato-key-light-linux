#pragma once

// Asynchronous HTTP transport for networked Key Lights. Wraps a single
// QNetworkAccessManager; all requests run on the main thread and complete via
// signals, so there is no worker-thread machinery (unlike the Python client,
// which pushed blocking `requests` calls onto a QThreadPool).

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>

#include "keylight.h"

class QNetworkAccessManager;
class QNetworkReply;

namespace elg {

class HttpTransport : public QObject {
    Q_OBJECT
public:
    explicit HttpTransport(QObject* parent = nullptr);

    void getInfo(const QString& id, const KeyLight& light);
    void getLights(const QString& id, const KeyLight& light);
    void putLights(const QString& id, const KeyLight& light, const LightState& state);

    // Abort any in-flight requests (used on shutdown).
    void abortAll();

signals:
    void infoReceived(const QString& id, const QJsonObject& info);
    void stateReceived(const QString& id, const QJsonObject& data);
    void putSucceeded(const QString& id);
    void requestFailed(const QString& id, elg::RequestKind kind);

private:
    void track(QNetworkReply* reply, const QString& id, RequestKind kind);
    void onFinished(QNetworkReply* reply, const QString& id, RequestKind kind);

    QNetworkAccessManager* m_nam;
    QSet<QNetworkReply*> m_pending;
};

}  // namespace elg
