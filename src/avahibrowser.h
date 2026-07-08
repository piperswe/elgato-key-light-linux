#pragma once

// mDNS discovery of Elgato Key Lights via the avahi-client C API, driven by the
// Qt event loop through a small custom AvahiPoll bridge (see avahibrowser.cpp).
// Because the bridge runs Avahi's callbacks on the main thread, discovery
// results reach LightManager as ordinary signals with no cross-thread
// marshalling.
//
// IPv4-only, mirroring the original: Elgato devices mis-announce IPv6 and do not
// answer on it.

#include <QObject>
#include <QString>

#include <avahi-client/client.h>
#include <avahi-client/lookup.h>

struct AvahiPoll;
class QTimer;

namespace elg {

struct DiscoveredService {
    QString serviceName;  // unique key (name.type.domain); matched on removal
    QString displayName;  // human instance name, used as the fallback label
    QString address;      // resolved IPv4
    int port = 0;
    QString mac;          // TXT 'id'
    QString model;        // TXT 'md'
};

class AvahiBrowser : public QObject {
    Q_OBJECT
public:
    explicit AvahiBrowser(QObject* parent = nullptr);
    ~AvahiBrowser() override;

    void start();
    void stop();

signals:
    void discovered(const elg::DiscoveredService& service);
    void serviceRemoved(const QString& serviceName);

private:
    // Client lifecycle (the daemon can restart underneath us).
    void createClient();
    void teardownClient();
    void scheduleRetry();

    // avahi C callbacks (static so their address matches the C typedefs).
    static void clientCallback(AvahiClient* client, AvahiClientState state,
                               void* userdata);
    static void browseCallback(AvahiServiceBrowser* browser,
                               AvahiIfIndex interface, AvahiProtocol protocol,
                               AvahiBrowserEvent event, const char* name,
                               const char* type, const char* domain,
                               AvahiLookupResultFlags flags, void* userdata);
    static void resolveCallback(AvahiServiceResolver* resolver,
                                AvahiIfIndex interface, AvahiProtocol protocol,
                                AvahiResolverEvent event, const char* name,
                                const char* type, const char* domain,
                                const char* host, const AvahiAddress* address,
                                uint16_t port, AvahiStringList* txt,
                                AvahiLookupResultFlags flags, void* userdata);

    AvahiPoll* m_poll = nullptr;      // owned by the bridge (freed in dtor)
    AvahiClient* m_client = nullptr;
    AvahiServiceBrowser* m_browser = nullptr;
    QTimer* m_retryTimer = nullptr;
};

}  // namespace elg
