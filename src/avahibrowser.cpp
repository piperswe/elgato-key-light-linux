#include "avahibrowser.h"

#include <memory>

#include <QSocketNotifier>
#include <QTimer>

#include <avahi-common/address.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/timeval.h>
#include <avahi-common/watch.h>

namespace elg {

namespace {

constexpr auto kServiceType = "_elg._tcp";

// --- AvahiPoll → Qt event-loop bridge ---------------------------------------
//
// Implements the AvahiPoll vtable in terms of QSocketNotifier (fd readiness)
// and QTimer (timeouts). This is what avahi-qt does internally; Avahi has no
// released Qt 6 integration, so we carry the ~100 lines here. Every Avahi
// callback consequently runs on the main thread inside the Qt event loop.

}  // namespace

struct AvahiQtWatch {
    AvahiWatchCallback callback = nullptr;
    void* userdata = nullptr;
    int fd = -1;
    AvahiWatchEvent occurred = AvahiWatchEvent(0);
    std::unique_ptr<QSocketNotifier> readNotifier;
    std::unique_ptr<QSocketNotifier> writeNotifier;

    void fire(AvahiWatchEvent event) {
        occurred = event;
        callback(reinterpret_cast<AvahiWatch*>(this), fd, event, userdata);
        occurred = AvahiWatchEvent(0);
    }
};

struct AvahiQtTimeout {
    AvahiTimeoutCallback callback = nullptr;
    void* userdata = nullptr;
    std::unique_ptr<QTimer> timer;

    void arm(const struct timeval* tv) {
        if (!tv) {
            timer->stop();
            return;
        }
        AvahiUsec usec = -avahi_age(tv);  // negative age == time still in future
        if (usec < 0) {
            usec = 0;
        }
        timer->start(static_cast<int>(usec / 1000));
    }
};

namespace {

AvahiWatch* watchNew(const AvahiPoll* api, int fd, AvahiWatchEvent event,
                     AvahiWatchCallback callback, void* userdata) {
    Q_UNUSED(api);
    auto* w = new AvahiQtWatch;
    w->callback = callback;
    w->userdata = userdata;
    w->fd = fd;

    w->readNotifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Read);
    QObject::connect(w->readNotifier.get(), &QSocketNotifier::activated,
                     [w]() { w->fire(AVAHI_WATCH_IN); });
    w->writeNotifier =
        std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Write);
    QObject::connect(w->writeNotifier.get(), &QSocketNotifier::activated,
                     [w]() { w->fire(AVAHI_WATCH_OUT); });

    w->readNotifier->setEnabled((event & AVAHI_WATCH_IN) != 0);
    w->writeNotifier->setEnabled((event & AVAHI_WATCH_OUT) != 0);
    return reinterpret_cast<AvahiWatch*>(w);
}

void watchUpdate(AvahiWatch* watch, AvahiWatchEvent event) {
    auto* w = reinterpret_cast<AvahiQtWatch*>(watch);
    w->readNotifier->setEnabled((event & AVAHI_WATCH_IN) != 0);
    w->writeNotifier->setEnabled((event & AVAHI_WATCH_OUT) != 0);
}

AvahiWatchEvent watchGetEvents(AvahiWatch* watch) {
    return reinterpret_cast<AvahiQtWatch*>(watch)->occurred;
}

void watchFree(AvahiWatch* watch) {
    delete reinterpret_cast<AvahiQtWatch*>(watch);
}

AvahiTimeout* timeoutNew(const AvahiPoll* api, const struct timeval* tv,
                         AvahiTimeoutCallback callback, void* userdata) {
    Q_UNUSED(api);
    auto* t = new AvahiQtTimeout;
    t->callback = callback;
    t->userdata = userdata;
    t->timer = std::make_unique<QTimer>();
    t->timer->setSingleShot(true);
    QObject::connect(t->timer.get(), &QTimer::timeout, [t]() {
        t->callback(reinterpret_cast<AvahiTimeout*>(t), t->userdata);
    });
    t->arm(tv);
    return reinterpret_cast<AvahiTimeout*>(t);
}

void timeoutUpdate(AvahiTimeout* timeout, const struct timeval* tv) {
    reinterpret_cast<AvahiQtTimeout*>(timeout)->arm(tv);
}

void timeoutFree(AvahiTimeout* timeout) {
    delete reinterpret_cast<AvahiQtTimeout*>(timeout);
}

AvahiPoll* qtPollNew() {
    auto* poll = new AvahiPoll;
    poll->userdata = nullptr;
    poll->watch_new = watchNew;
    poll->watch_update = watchUpdate;
    poll->watch_get_events = watchGetEvents;
    poll->watch_free = watchFree;
    poll->timeout_new = timeoutNew;
    poll->timeout_update = timeoutUpdate;
    poll->timeout_free = timeoutFree;
    return poll;
}

QString txtValue(AvahiStringList* txt, const char* key) {
    AvahiStringList* rec = avahi_string_list_find(txt, key);
    if (!rec) {
        return QString();
    }
    char* k = nullptr;
    char* v = nullptr;
    size_t n = 0;
    if (avahi_string_list_get_pair(rec, &k, &v, &n) < 0) {
        return QString();
    }
    QString result = v ? QString::fromUtf8(v, static_cast<int>(n)) : QString();
    avahi_free(k);
    avahi_free(v);
    return result;
}

}  // namespace

// --- AvahiBrowser ------------------------------------------------------------

AvahiBrowser::AvahiBrowser(QObject* parent) : QObject(parent) {
    m_poll = qtPollNew();
    m_retryTimer = new QTimer(this);
    m_retryTimer->setSingleShot(true);
    m_retryTimer->setInterval(2000);
    connect(m_retryTimer, &QTimer::timeout, this, &AvahiBrowser::createClient);
}

AvahiBrowser::~AvahiBrowser() {
    teardownClient();
    delete m_poll;
}

void AvahiBrowser::start() {
    createClient();
}

void AvahiBrowser::stop() {
    m_retryTimer->stop();
    teardownClient();
}

void AvahiBrowser::createClient() {
    if (m_client) {
        return;
    }
    int error = 0;
    // AVAHI_CLIENT_NO_FAIL tolerates the daemon not being up yet; the client
    // callback drives browser creation once it reaches the running state.
    m_client = avahi_client_new(m_poll, AVAHI_CLIENT_NO_FAIL, clientCallback, this,
                                &error);
    if (!m_client) {
        scheduleRetry();
    }
}

void AvahiBrowser::teardownClient() {
    if (m_browser) {
        avahi_service_browser_free(m_browser);
        m_browser = nullptr;
    }
    if (m_client) {
        avahi_client_free(m_client);
        m_client = nullptr;
    }
}

void AvahiBrowser::scheduleRetry() {
    teardownClient();
    if (!m_retryTimer->isActive()) {
        m_retryTimer->start();
    }
}

void AvahiBrowser::clientCallback(AvahiClient* client, AvahiClientState state,
                                  void* userdata) {
    auto* self = static_cast<AvahiBrowser*>(userdata);
    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            if (!self->m_browser) {
                self->m_browser = avahi_service_browser_new(
                    client, AVAHI_IF_UNSPEC, AVAHI_PROTO_INET, kServiceType,
                    nullptr, AvahiLookupFlags(0), browseCallback, self);
            }
            break;
        case AVAHI_CLIENT_FAILURE:
            // Daemon died or connection lost: rebuild on a timer.
            self->scheduleRetry();
            break;
        default:
            break;
    }
}

void AvahiBrowser::browseCallback(AvahiServiceBrowser* browser,
                                  AvahiIfIndex interface, AvahiProtocol protocol,
                                  AvahiBrowserEvent event, const char* name,
                                  const char* type, const char* domain,
                                  AvahiLookupResultFlags flags, void* userdata) {
    Q_UNUSED(browser);
    Q_UNUSED(flags);
    auto* self = static_cast<AvahiBrowser*>(userdata);
    switch (event) {
        case AVAHI_BROWSER_NEW:
            // One-shot resolve forced to IPv4; the resolver frees itself in the
            // callback, matching the Python get_service_info() semantics.
            avahi_service_resolver_new(self->m_client, interface, protocol, name,
                                       type, domain, AVAHI_PROTO_INET,
                                       AvahiLookupFlags(0), resolveCallback, self);
            break;
        case AVAHI_BROWSER_REMOVE:
            emit self->serviceRemoved(QString::fromUtf8(name));
            break;
        case AVAHI_BROWSER_FAILURE:
            self->scheduleRetry();
            break;
        default:
            break;
    }
}

void AvahiBrowser::resolveCallback(AvahiServiceResolver* resolver,
                                   AvahiIfIndex interface, AvahiProtocol protocol,
                                   AvahiResolverEvent event, const char* name,
                                   const char* type, const char* domain,
                                   const char* host, const AvahiAddress* address,
                                   uint16_t port, AvahiStringList* txt,
                                   AvahiLookupResultFlags flags, void* userdata) {
    Q_UNUSED(interface);
    Q_UNUSED(protocol);
    Q_UNUSED(type);
    Q_UNUSED(domain);
    Q_UNUSED(host);
    Q_UNUSED(flags);
    auto* self = static_cast<AvahiBrowser*>(userdata);

    if (event == AVAHI_RESOLVER_FOUND && address) {
        char addr[AVAHI_ADDRESS_STR_MAX];
        avahi_address_snprint(addr, sizeof(addr), address);
        DiscoveredService svc;
        svc.serviceName = QString::fromUtf8(name);
        svc.address = QString::fromUtf8(addr);
        svc.port = port;
        svc.mac = txtValue(txt, "id");
        svc.model = txtValue(txt, "md");
        emit self->discovered(svc);
    }
    // Either way this resolver is done.
    avahi_service_resolver_free(resolver);
}

}  // namespace elg
