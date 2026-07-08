// KDE system tray controller for Elgato Key Lights.
//
// Left-clicking the tray icon opens a centred control window with a card per
// light — an On/Off toggle plus brightness and colour-temperature sliders.
// Right-clicking shows a small native menu; middle-clicking toggles all lights.
//
// The controls live in a normal window rather than the tray menu because slider
// widgets cannot be drawn in a menu exported over Plasma's DBusMenu. Keeping the
// menu free of custom widgets lets it be registered with setContextMenu so
// Plasma renders and positions it natively — and lets the whole app run on
// native Wayland.

#include <csignal>
#include <cstdio>

#include <QApplication>
#include <QCoreApplication>
#include <QStringList>
#include <QSystemTrayIcon>

#include "keylight.h"
#include "trayapp.h"
#include "version.h"

#if HAVE_USB
#include <QJsonDocument>
#include "usbneo.h"

// Dump the raw USB HID exchange with any attached Key Light Neo (ported from
// usbneo.py's __main__). Runs synchronously: the worker lives on this thread, so
// its result signals fire as direct calls.
static int runProbe() {
    using namespace elg;
    UsbNeoWorker worker;
    QList<QByteArray> paths;
    QObject::connect(&worker, &UsbNeoWorker::enumerated,
                     [&](const QList<QByteArray>& p) { paths = p; });
    QObject::connect(&worker, &UsbNeoWorker::infoReady,
                     [](const QByteArray&, const QJsonObject& info) {
                         printf("  accessory-info: %s\n",
                                QJsonDocument(info).toJson(QJsonDocument::Compact)
                                    .constData());
                     });
    QObject::connect(&worker, &UsbNeoWorker::infoFailed,
                     [](const QByteArray&) { printf("  accessory-info: FAILED\n"); });
    QObject::connect(&worker, &UsbNeoWorker::requestDone,
                     [](const QString&, RequestKind, const QJsonObject& data) {
                         printf("  lights: %s\n",
                                QJsonDocument(data).toJson(QJsonDocument::Compact)
                                    .constData());
                     });
    QObject::connect(&worker, &UsbNeoWorker::requestFailed,
                     [](const QString&, RequestKind) { printf("  lights: FAILED\n"); });

    worker.enumerate();
    printf("Found %lld Neo interface(s).\n", static_cast<long long>(paths.size()));
    for (const QByteArray& path : paths) {
        printf("\n=== %s ===\n", path.constData());
        worker.probeInfo(path);
        worker.request(QString(), path, RequestKind::State, QJsonObject());
    }
    return 0;
}
#endif

int main(int argc, char** argv) {
    // Let Ctrl+C terminate the process when run in the foreground.
    std::signal(SIGINT, SIG_DFL);

    qRegisterMetaType<elg::RequestKind>();

    QStringList args;
    args.reserve(argc);
    for (int i = 0; i < argc; ++i) {
        args << QString::fromLocal8Bit(argv[i]);
    }
    const bool probe = args.contains(QStringLiteral("--probe"));

    if (probe) {
#if HAVE_USB
        QCoreApplication app(argc, argv);
        return runProbe();
#else
        fprintf(stderr, "USB support was not built in.\n");
        return 1;
#endif
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Key Lights"));
    app.setApplicationVersion(QStringLiteral(APP_VERSION));
    app.setDesktopFileName(QStringLiteral("keylights-tray"));
    app.setQuitOnLastWindowClosed(false);

    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        fprintf(stderr, "No system tray available.\n");
        return 1;
    }

    new elg::TrayApp(&app);
    return app.exec();
}
