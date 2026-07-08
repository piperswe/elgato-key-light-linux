#include "lightmanager.h"

#include <QJsonArray>
#include <QThread>
#include <QTimer>

#include "httptransport.h"
#if HAVE_USB
#include "usbneo.h"
#endif

namespace elg {

LightManager::LightManager(QObject* parent) : QObject(parent) {
    m_http = new HttpTransport(this);
    connect(m_http, &HttpTransport::infoReceived, this,
            &LightManager::onInfoReceived);
    connect(m_http, &HttpTransport::stateReceived, this,
            &LightManager::onStateReceived);
    connect(m_http, &HttpTransport::putSucceeded, this,
            &LightManager::onPutSucceeded);
    connect(m_http, &HttpTransport::requestFailed, this,
            &LightManager::onRequestFailed);

    m_browser = new AvahiBrowser(this);
    connect(m_browser, &AvahiBrowser::discovered, this,
            &LightManager::onDiscovered);
    connect(m_browser, &AvahiBrowser::serviceRemoved, this,
            &LightManager::onServiceRemoved);

#if HAVE_USB
    m_usbThread = new QThread(this);
    m_usb = new UsbNeoWorker;  // no parent: moved to its own thread
    m_usb->moveToThread(m_usbThread);
    // The worker is deleted deterministically in stop() after the thread's
    // event loop has stopped (a finished -> deleteLater connection would not
    // run then, leaking it and skipping hid_exit()).

    connect(this, &LightManager::usbEnumerateRequested, m_usb,
            &UsbNeoWorker::enumerate);
    connect(this, &LightManager::usbProbeRequested, m_usb,
            &UsbNeoWorker::probeInfo);
    connect(this, &LightManager::usbRequestRequested, m_usb,
            &UsbNeoWorker::request);

    connect(m_usb, &UsbNeoWorker::enumerated, this,
            &LightManager::onUsbEnumerated);
    connect(m_usb, &UsbNeoWorker::infoReady, this, &LightManager::onUsbInfoReady);
    connect(m_usb, &UsbNeoWorker::infoFailed, this,
            &LightManager::onUsbInfoFailed);
    connect(m_usb, &UsbNeoWorker::requestDone, this,
            [this](const QString& id, RequestKind kind, const QJsonObject& data) {
                if (kind == RequestKind::Put) {
                    onPutSucceeded(id);
                } else {
                    onStateReceived(id, data);
                }
            });
    connect(m_usb, &UsbNeoWorker::requestFailed, this,
            &LightManager::onRequestFailed);
    m_usbThread->start();
#endif

    m_refreshTimer = new QTimer(this);
    m_refreshTimer->setInterval(kRefreshMs);
    connect(m_refreshTimer, &QTimer::timeout, this, &LightManager::onRefreshTick);
}

LightManager::~LightManager() {
    stop();
}

// -- lifecycle ---------------------------------------------------------------

void LightManager::start() {
    m_browser->start();
    scanUsb();
    m_refreshTimer->start();
}

void LightManager::stop() {
    m_refreshTimer->stop();
    if (m_browser) {
        m_browser->stop();
    }
    if (m_http) {
        m_http->abortAll();
    }
#if HAVE_USB
    if (m_usbThread && m_usbThread->isRunning()) {
        m_usbThread->quit();
        m_usbThread->wait(2500);
    }
    // Delete the worker deterministically. The finished -> deleteLater wiring
    // cannot run once the thread's event loop has stopped, so it would leak the
    // worker and skip hid_exit(); with the thread now stopped it is safe to
    // delete directly from the main thread.
    if (m_usb) {
        delete m_usb;
        m_usb = nullptr;
    }
#endif
}

// -- accessors ---------------------------------------------------------------

QList<KeyLight> LightManager::lights() const {
    return m_lights.values();
}

const KeyLight* LightManager::get(const QString& id) const {
    auto it = m_lights.constFind(id);
    return it == m_lights.constEnd() ? nullptr : &it.value();
}

bool LightManager::anyOn() const {
    for (const KeyLight& l : m_lights) {
        if (l.online && l.state && l.state->on) {
            return true;
        }
    }
    return false;
}

std::pair<int, int> LightManager::counts() const {
    int on = 0;
    int online = 0;
    for (const KeyLight& l : m_lights) {
        if (l.online && l.state) {
            ++online;
            if (l.state->on) {
                ++on;
            }
        }
    }
    return {on, online};
}

// -- discovery (main thread) -------------------------------------------------

void LightManager::onDiscovered(const DiscoveredService& service) {
    const QString id =
        service.mac.isEmpty() ? service.displayName : normalizeMac(service.mac);
    m_byService.insert(service.serviceName, id);

    auto it = m_lights.find(id);
    if (it == m_lights.end()) {
        KeyLight light;
        light.id = id;
        light.name = service.displayName;
        light.address = service.address;
        // Avahi may report port 0 when the SRV record lacks one; keep the
        // default so the base URL stays valid (parity with the Python client).
        light.port = service.port > 0 ? service.port : kDefaultPort;
        light.product = service.model;
        m_lights.insert(id, light);
        fetchInfo(m_lights[id]);
        fetchState(m_lights[id], /*announce=*/true);
    } else {
        // Known light (possibly a new IP after a DHCP change).
        KeyLight& light = it.value();
        light.address = service.address;
        light.port = service.port > 0 ? service.port : kDefaultPort;
        fetchState(light, /*announce=*/!light.online);
    }
}

void LightManager::onServiceRemoved(const QString& serviceName) {
    const QString id = m_byService.take(serviceName);
    if (id.isEmpty()) {
        return;
    }
    auto it = m_lights.find(id);
    if (it != m_lights.end() && it.value().online) {
        it.value().online = false;
        emit lightUpdated(id);
    }
}

// -- fetching ----------------------------------------------------------------

void LightManager::fetchInfo(const KeyLight& light) {
    // Accessory-info over the network only; the USB path uses probeInfo.
    m_http->getInfo(light.id, light);
}

void LightManager::fetchState(KeyLight& light, bool announce) {
    if (announce) {
        m_announcePending.insert(light.id);
    }
    if (light.transport == Transport::Usb) {
#if HAVE_USB
        emit usbRequestRequested(light.id, light.usbPath, RequestKind::State,
                                 QJsonObject());
#endif
    } else {
        m_http->getLights(light.id, light);
    }
}

void LightManager::onInfoReceived(const QString& id, const QJsonObject& info) {
    auto it = m_lights.find(id);
    if (it == m_lights.end()) {
        return;
    }
    KeyLight& light = it.value();
    QString name = info.value(QStringLiteral("displayName")).toString();
    if (name.isEmpty()) {
        name = info.value(QStringLiteral("productName")).toString();
    }
    if (!name.isEmpty()) {
        light.name = name;
    }
    const QString serial = info.value(QStringLiteral("serialNumber")).toString();
    if (!serial.isEmpty()) {
        light.serial = serial;
        auto other = m_bySerial.constFind(serial);
        if (other != m_bySerial.constEnd() && other.value() != id) {
            // A USB entry for this device arrived first under a different id;
            // fold this network entry into it so USB stays the transport.
            merge(other.value(), id);
            return;
        }
        m_bySerial.insert(serial, id);
    }
    emit lightUpdated(id);
}

void LightManager::onStateReceived(const QString& id, const QJsonObject& data) {
    applyState(id, data, m_announcePending.remove(id));
}

void LightManager::applyState(const QString& id, const QJsonObject& data,
                              bool announce) {
    auto it = m_lights.find(id);
    if (it == m_lights.end()) {
        return;
    }
    KeyLight& light = it.value();
    const QJsonArray lights = data.value(QStringLiteral("lights")).toArray();
    light.numLights = data.value(QStringLiteral("numberOfLights"))
                          .toInt(lights.isEmpty() ? 1 : lights.size());
    const bool wasOnline = light.online;
    light.failCount = 0;
    light.online = true;
    if (!lights.isEmpty()) {
        light.state = LightState::fromJson(lights.first().toObject());
    }
    if (announce && !wasOnline) {
        emit lightAdded(id);
    } else {
        emit lightUpdated(id);
    }
}

void LightManager::onIoFail(const QString& id) {
    auto it = m_lights.find(id);
    if (it == m_lights.end()) {
        return;
    }
    KeyLight& light = it.value();
    ++light.failCount;
    if (light.online && light.failCount >= kFailLimit) {
        light.online = false;
        emit lightUpdated(id);
    }
}

void LightManager::onPutSucceeded(const QString& id) {
    m_inflight.remove(id);
    auto it = m_lights.find(id);
    if (it != m_lights.end()) {
        KeyLight& light = it.value();
        light.failCount = 0;
        if (!light.online) {
            light.online = true;
        }
    }
    emit lightUpdated(id);
    if (m_dirty.remove(id)) {
        flush(id);
    }
}

void LightManager::onRequestFailed(const QString& id, RequestKind kind) {
    if (kind == RequestKind::Put) {
        m_inflight.remove(id);
        m_dirty.remove(id);
        onIoFail(id);
    } else if (kind == RequestKind::State) {
        onIoFail(id);
    }
    // Info failures are ignored: the light keeps its fallback name.
}

// -- USB reconciliation ------------------------------------------------------

void LightManager::scanUsb() {
#if HAVE_USB
    emit usbEnumerateRequested();
#endif
}

void LightManager::onUsbEnumerated(const QList<QByteArray>& paths) {
    const QSet<QByteArray> present(paths.begin(), paths.end());
    for (const QByteArray& path : paths) {
        if (!m_byUsbPath.contains(path) && !m_usbPending.contains(path)) {
            m_usbPending.insert(path);
            emit usbProbeRequested(path);
        }
    }
    const QList<QByteArray> known = m_byUsbPath.keys();
    for (const QByteArray& path : known) {
        if (!present.contains(path)) {
            onUsbRemoved(path);
        }
    }
}

void LightManager::onUsbInfoReady(const QByteArray& path,
                                  const QJsonObject& info) {
    applyUsbInfo(path, info);
}

void LightManager::onUsbInfoFailed(const QByteArray& path) {
    // Path stays out of the cache; the next scan retries it.
    m_usbPending.remove(path);
}

void LightManager::applyUsbInfo(const QByteArray& path, const QJsonObject& info) {
    m_usbPending.remove(path);
    const QString serial = info.value(QStringLiteral("serialNumber")).toString();
    const QString mac =
        normalizeMac(info.value(QStringLiteral("macAddress")).toString());
    QString id = !mac.isEmpty()
                     ? mac
                     : (!serial.isEmpty()
                            ? serial
                            : QStringLiteral("usb:") + QString::fromUtf8(path));
    QString name = info.value(QStringLiteral("displayName")).toString();
    if (name.isEmpty()) {
        name = info.value(QStringLiteral("productName")).toString();
    }
    if (name.isEmpty()) {
        name = id;
    }

    bool existing = m_lights.contains(id);
    // A device already known under a serial-based id (no MAC) matches here.
    if (!existing && !serial.isEmpty()) {
        auto bySerial = m_bySerial.constFind(serial);
        if (bySerial != m_bySerial.constEnd() && m_lights.contains(bySerial.value())) {
            id = bySerial.value();
            existing = true;
        }
    }

    m_byUsbPath.insert(path, id);
    if (!serial.isEmpty()) {
        m_bySerial.insert(serial, id);
    }

    if (existing) {
        // Adopt USB as the transport, since it needs no network.
        KeyLight& light = m_lights[id];
        light.usbPath = path;
        light.transport = Transport::Usb;
        light.name = name;
        if (!serial.isEmpty()) {
            light.serial = serial;
        }
        fetchState(light, /*announce=*/false);
        emit lightUpdated(id);
        return;
    }

    KeyLight light;
    light.id = id;
    light.name = name;
    light.product = info.value(QStringLiteral("productName")).toString();
    light.transport = Transport::Usb;
    light.usbPath = path;
    light.serial = serial;
    m_lights.insert(id, light);
    fetchState(m_lights[id], /*announce=*/true);
}

void LightManager::onUsbRemoved(const QByteArray& path) {
    const QString id = m_byUsbPath.take(path);
    m_usbPending.remove(path);
    if (id.isEmpty()) {
        return;
    }
    auto it = m_lights.find(id);
    if (it == m_lights.end()) {
        return;
    }
    KeyLight& light = it.value();
    light.usbPath.clear();
    if (!light.address.isEmpty()) {
        // Still reachable over the network: fall back to HTTP.
        light.transport = Transport::Http;
        emit lightUpdated(id);
    } else if (light.online) {
        light.online = false;
        emit lightUpdated(id);
    }
}

void LightManager::merge(const QString& keepId, const QString& dropId) {
    auto keepIt = m_lights.find(keepId);
    auto dropIt = m_lights.find(dropId);
    if (keepIt == m_lights.end() || dropIt == m_lights.end() || keepId == dropId) {
        return;
    }
    KeyLight& keep = keepIt.value();
    KeyLight& drop = dropIt.value();
    if (keep.address.isEmpty() && !drop.address.isEmpty()) {
        keep.address = drop.address;
        keep.port = drop.port;
    }
    if (keep.usbPath.isEmpty() && !drop.usbPath.isEmpty()) {
        keep.usbPath = drop.usbPath;
        keep.transport = Transport::Usb;
    }
    if (keep.serial.isEmpty() && !drop.serial.isEmpty()) {
        keep.serial = drop.serial;
    }
    for (auto* mapping : {&m_byService, &m_bySerial}) {
        for (auto it = mapping->begin(); it != mapping->end(); ++it) {
            if (it.value() == dropId) {
                it.value() = keepId;
            }
        }
    }
    for (auto it = m_byUsbPath.begin(); it != m_byUsbPath.end(); ++it) {
        if (it.value() == dropId) {
            it.value() = keepId;
        }
    }
    m_lights.remove(dropId);
    emit lightRemoved(dropId);
    emit lightUpdated(keepId);
}

// -- refresh -----------------------------------------------------------------

void LightManager::refreshAll() {
    for (KeyLight& light : m_lights) {
        if (m_inflight.contains(light.id)) {
            continue;
        }
        fetchState(light, /*announce=*/false);
    }
}

void LightManager::onRefreshTick() {
    ++m_refreshTicks;
    scanUsb();
    for (KeyLight& light : m_lights) {
        if (m_inflight.contains(light.id)) {
            continue;
        }
        // Offline lights are probed less often to avoid needless timeouts.
        if (!light.online && (m_refreshTicks % 3) != 0) {
            continue;
        }
        fetchState(light, /*announce=*/false);
    }
}

// -- user intents (main thread only) -----------------------------------------

void LightManager::setOn(const QString& id, bool on) {
    auto it = m_lights.find(id);
    if (it == m_lights.end() || !it.value().state) {
        return;
    }
    it.value().state->on = on;
    flush(id);
}

void LightManager::setBrightness(const QString& id, int value) {
    auto it = m_lights.find(id);
    if (it == m_lights.end() || !it.value().state) {
        return;
    }
    it.value().state->brightness = clampInt(value, 0, 100);
    schedule(id);
}

void LightManager::setTemperature(const QString& id, int mireds) {
    auto it = m_lights.find(id);
    if (it == m_lights.end() || !it.value().state) {
        return;
    }
    it.value().state->temperature = clampInt(mireds, kMiredMin, kMiredMax);
    schedule(id);
}

void LightManager::toggleAll() {
    const bool target = !anyOn();
    for (KeyLight& light : m_lights) {
        if (light.online && light.state) {
            light.state->on = target;
            flush(light.id);
        }
    }
}

void LightManager::flushPending(const QString& id) {
    auto timer = m_debounce.value(id, nullptr);
    if (timer && timer->isActive()) {
        timer->stop();
    }
    flush(id);
}

// -- debounce / coalescing ---------------------------------------------------

void LightManager::schedule(const QString& id) {
    QTimer* timer = m_debounce.value(id, nullptr);
    if (!timer) {
        timer = new QTimer(this);
        timer->setSingleShot(true);
        timer->setInterval(kDebounceMs);
        connect(timer, &QTimer::timeout, this, [this, id]() { flush(id); });
        m_debounce.insert(id, timer);
    }
    timer->start();
}

void LightManager::flush(const QString& id) {
    auto it = m_lights.find(id);
    if (it == m_lights.end() || !it.value().state) {
        return;
    }
    if (m_inflight.contains(id)) {
        // A PUT is already running; remember to resend the latest state.
        m_dirty.insert(id);
        return;
    }
    m_inflight.insert(id);
    KeyLight& light = it.value();
    const LightState snapshot = *light.state;  // send-time snapshot
    if (light.transport == Transport::Usb) {
#if HAVE_USB
        emit usbRequestRequested(id, light.usbPath, RequestKind::Put,
                                 buildLightsBody(light, snapshot));
#endif
    } else {
        m_http->putLights(id, light, snapshot);
    }
}

}  // namespace elg
