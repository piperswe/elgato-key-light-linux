#pragma once

// Owns the device cache and all control policy. Every cache mutation happens on
// the main thread: Avahi discovery arrives via main-thread signals (see
// AvahiBrowser), HTTP completes asynchronously on the main thread, and the USB
// worker communicates only through queued signals. This preserves the
// main-thread-only invariant of the original lights.py.

#include <QHash>
#include <QList>
#include <QObject>
#include <QSet>

#include "avahibrowser.h"
#include "keylight.h"
#include "version.h"

class QThread;
class QTimer;

namespace elg {

class HttpTransport;
#if HAVE_USB
class UsbNeoWorker;
#endif

class LightManager : public QObject {
    Q_OBJECT
public:
    explicit LightManager(QObject* parent = nullptr);
    ~LightManager() override;

    void start();
    void stop();

    QList<KeyLight> lights() const;
    const KeyLight* get(const QString& id) const;
    bool anyOn() const;
    std::pair<int, int> counts() const;  // (number on, number online)

public slots:
    void setOn(const QString& id, bool on);
    void setBrightness(const QString& id, int value);
    void setTemperature(const QString& id, int mireds);
    void toggleAll();
    void refreshAll();
    void flushPending(const QString& id);  // e.g. on slider release

signals:
    void lightAdded(const QString& id);
    void lightUpdated(const QString& id);
    void lightRemoved(const QString& id);

    // Cross-thread requests into the USB worker (queued to its thread).
    void usbEnumerateRequested();
    void usbProbeRequested(const QByteArray& path);
    void usbRequestRequested(const QString& id, const QByteArray& path,
                             elg::RequestKind kind, const QJsonObject& body);

private slots:
    // Discovery (main thread).
    void onDiscovered(const elg::DiscoveredService& service);
    void onServiceRemoved(const QString& serviceName);

    // HTTP / USB results.
    void onInfoReceived(const QString& id, const QJsonObject& info);
    void onStateReceived(const QString& id, const QJsonObject& data);
    void onPutSucceeded(const QString& id);
    void onRequestFailed(const QString& id, elg::RequestKind kind);
    void onUsbEnumerated(const QList<QByteArray>& paths);
    void onUsbInfoReady(const QByteArray& path, const QJsonObject& info);
    void onUsbInfoFailed(const QByteArray& path);

    void onRefreshTick();

private:
    // Fetching.
    void fetchInfo(const KeyLight& light);
    void fetchState(KeyLight& light, bool announce);
    void applyState(const QString& id, const QJsonObject& data, bool announce);
    void onIoFail(const QString& id);

    // USB reconciliation.
    void scanUsb();
    void applyUsbInfo(const QByteArray& path, const QJsonObject& info);
    void onUsbRemoved(const QByteArray& path);
    void merge(const QString& keepId, const QString& dropId);

    // Debounce / coalescing for slider-driven PUTs.
    void schedule(const QString& id);
    void flush(const QString& id);

    QHash<QString, KeyLight> m_lights;
    QHash<QString, QString> m_byService;    // mDNS service name -> id
    QHash<QByteArray, QString> m_byUsbPath;  // hidapi path -> id
    QHash<QString, QString> m_bySerial;      // serialNumber -> id (dedup)
    QSet<QByteArray> m_usbPending;           // paths awaiting accessory-info

    HttpTransport* m_http = nullptr;
    AvahiBrowser* m_browser = nullptr;
#if HAVE_USB
    QThread* m_usbThread = nullptr;
    UsbNeoWorker* m_usb = nullptr;
#endif

    QTimer* m_refreshTimer = nullptr;
    int m_refreshTicks = 0;

    QHash<QString, QTimer*> m_debounce;
    QSet<QString> m_inflight;
    QSet<QString> m_dirty;
    QSet<QString> m_announcePending;  // ids whose next state result should announce
};

}  // namespace elg
