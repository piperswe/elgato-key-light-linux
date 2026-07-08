#pragma once

// Data model for the tray app: plain value types plus the device constants and
// unit conversions shared across the discovery, transport and UI layers. No
// QObject or widget code lives here so it can be reasoned about (and reused)
// independently, mirroring the split in the original lights.py.

#include <optional>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaType>
#include <QString>
#include <QUrl>
#include <cmath>

namespace elg {

// Which request a transport result belongs to, so the manager can route it.
// Shared by the HTTP and USB transports (the USB worker emits it across a
// thread boundary, hence the metatype registration below).
enum class RequestKind { Info, State, Put };

constexpr int kDefaultPort = 9123;

// HTTP endpoints (see keylights.sh).
inline constexpr auto kEpLights = "/elgato/lights";
inline constexpr auto kEpInfo = "/elgato/accessory-info";

// Elgato colour temperature is stored in mireds. 143..344 mireds spans roughly
// 7000K..2900K, the range the hardware accepts.
constexpr int kMiredMin = 143;
constexpr int kMiredMax = 344;

// Overall HTTP transfer budget, milliseconds. QNetworkAccessManager has a single
// timeout rather than the separate connect/read pair of the Python client; 2500
// approximates the old (1.5, 2.0) connect/read timeouts.
constexpr int kHttpTimeoutMs = 2500;

// Consecutive I/O failures before a light is considered offline.
constexpr int kFailLimit = 3;

// Trailing debounce window for slider drags, milliseconds.
constexpr int kDebounceMs = 150;

// Periodic state refresh, milliseconds.
constexpr int kRefreshMs = 5000;

enum class Transport { Http, Usb };

inline int clampInt(int value, int low, int high) {
    return std::max(low, std::min(high, value));
}

// Convert mireds to Kelvin, rounded to the nearest 50K for a tidy label.
inline int miredToKelvin(int mired) {
    return static_cast<int>(std::lround(1000000.0 / mired / 50.0)) * 50;
}

inline QString kelvinLabel(int mired) {
    return QStringLiteral("%1 K").arg(miredToKelvin(mired));
}

// Canonicalise a MAC to the mDNS TXT 'id' form (upper-case). The accessory-info
// macAddress and the mDNS id record already share the 3C:6A:9D:.. format, so
// this only guards against case differences so both discovery paths derive one
// identity for the same device.
inline QString normalizeMac(const QString& raw) {
    return raw.trimmed().toUpper();
}

struct LightState {
    bool on = false;
    int brightness = 0;            // 0-100
    int temperature = kMiredMin;   // mireds, kMiredMin..kMiredMax

    static LightState fromJson(const QJsonObject& light) {
        LightState s;
        s.on = light.value(QStringLiteral("on")).toInt(0) != 0;
        s.brightness = light.value(QStringLiteral("brightness")).toInt(0);
        s.temperature = light.value(QStringLiteral("temperature")).toInt(kMiredMin);
        return s;
    }
};

struct KeyLight {
    QString id;         // MAC (mDNS TXT 'id' / accessory-info macAddress); stable identity
    QString name;       // displayName from accessory-info, falls back to mDNS name
    QString address;    // IPv4 (empty for a USB-only light)
    int port = kDefaultPort;
    QString product;    // model from TXT 'md'
    QString serial;     // accessory-info serialNumber; dedup key across transports
    bool online = false;
    int numLights = 1;
    std::optional<LightState> state;
    int failCount = 0;
    Transport transport = Transport::Http;
    QByteArray usbPath; // hidapi device path when USB-reachable (empty otherwise)

    QUrl baseUrl() const {
        QUrl url;
        url.setScheme(QStringLiteral("http"));
        url.setHost(address);
        url.setPort(port);
        return url;
    }
};

// Build the /elgato/lights PUT body: the same single light state repeated
// numberOfLights times, matching the hardware's expected shape.
inline QJsonObject buildLightsBody(const KeyLight& light, const LightState& state) {
    QJsonObject one{
        {QStringLiteral("on"), state.on ? 1 : 0},
        {QStringLiteral("brightness"), clampInt(state.brightness, 0, 100)},
        {QStringLiteral("temperature"),
         clampInt(state.temperature, kMiredMin, kMiredMax)},
    };
    const int count = std::max(1, light.numLights);
    QJsonArray lights;
    for (int i = 0; i < count; ++i) {
        lights.append(one);
    }
    return QJsonObject{
        {QStringLiteral("numberOfLights"), light.numLights},
        {QStringLiteral("lights"), lights},
    };
}

}  // namespace elg

Q_DECLARE_METATYPE(elg::RequestKind)
