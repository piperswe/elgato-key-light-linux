#include "trayapp.h"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QStyleHints>
#include <QSystemTrayIcon>

#include "controlwindow.h"
#include "keylight.h"
#include "lightmanager.h"

namespace elg {

namespace {

constexpr auto kAppName = "Key Lights";

// Themed icon names. On KDE the tray delivers these by name over the
// StatusNotifierItem protocol, so Plasma scales them to fill the panel slot and
// recolours the "-symbolic" variants for light/dark contrast.
constexpr auto kIconOn = "keylights-tray-symbolic";
constexpr auto kIconOff = "keylights-tray-off-symbolic";

constexpr int kFallbackSize = 512;
constexpr qreal kOffOpacity = 0.4;
const QColor kLightOnDark(244, 244, 244);
const QColor kDarkOnLight(45, 45, 45);

QString firstExisting(const QStringList& paths) {
    for (const QString& p : paths) {
        if (QFileInfo::exists(p)) {
            return p;
        }
    }
    return QString();
}

// Fallback assets, tried in order, for environments without the installed theme
// icon (a checkout, or a non-KDE tray). The scalable SVG is preferred; the
// legacy PNG is a last resort.
QPixmap loadBasePixmap() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString svg = firstExisting({
        appDir + QStringLiteral("/../assets/keylights-tray.svg"),
        QStringLiteral("/usr/share/icons/hicolor/scalable/apps/keylights-tray.svg"),
    });
    if (!svg.isEmpty()) {
        QPixmap pm = QIcon(svg).pixmap(kFallbackSize, kFallbackSize);
        if (!pm.isNull()) {
            return pm;
        }
    }
    const QString png = firstExisting({
        appDir + QStringLiteral("/../assets/elgato.png"),
        QStringLiteral("/usr/share/pixmaps/keylights-tray.png"),
    });
    if (!png.isEmpty()) {
        QPixmap pm(png);
        if (!pm.isNull()) {
            return pm;
        }
    }
    return QIcon::fromTheme(QStringLiteral("weather-clear"))
        .pixmap(kFallbackSize, kFallbackSize);
}

// Pick a monochrome colour with strong contrast against the current panel.
QColor foregroundForScheme(QApplication* app) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const Qt::ColorScheme scheme = app->styleHints()->colorScheme();
    if (scheme == Qt::ColorScheme::Dark) {
        return kLightOnDark;
    }
    if (scheme == Qt::ColorScheme::Light) {
        return kDarkOnLight;
    }
#endif
    // Unknown scheme (or Qt < 6.5): infer from window background lightness.
    const QColor bg = app->palette().color(QPalette::Window);
    return bg.lightness() < 128 ? kLightOnDark : kDarkOnLight;
}

// Recolour a pixmap to a flat colour, using its alpha channel as the mask.
QPixmap recolor(const QPixmap& src, const QColor& color) {
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.drawPixmap(0, 0, src);
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(out.rect(), color);
    p.end();
    return out;
}

QPixmap dim(const QPixmap& src, qreal opacity) {
    QPixmap out(src.size());
    out.fill(Qt::transparent);
    QPainter p(&out);
    p.setOpacity(opacity);
    p.drawPixmap(0, 0, src);
    p.end();
    return out;
}

QIcon fallbackIcon(QApplication* app, bool dimmed) {
    QPixmap base = loadBasePixmap().scaled(kFallbackSize, kFallbackSize,
                                           Qt::KeepAspectRatio,
                                           Qt::SmoothTransformation);
    QPixmap tinted = recolor(base, foregroundForScheme(app));
    if (dimmed) {
        tinted = dim(tinted, kOffOpacity);
    }
    QIcon icon;
    icon.addPixmap(tinted);
    return icon;
}

// Return (on, off) icons, preferring the recolourable themed icons.
std::pair<QIcon, QIcon> buildIcons(QApplication* app) {
    QIcon on = QIcon::fromTheme(QString::fromUtf8(kIconOn));
    if (on.isNull()) {
        on = fallbackIcon(app, false);
    }
    QIcon off = QIcon::fromTheme(QString::fromUtf8(kIconOff));
    if (off.isNull()) {
        off = fallbackIcon(app, true);
    }
    return {on, off};
}

}  // namespace

TrayApp::TrayApp(QApplication* app) : QObject(app), m_app(app) {
    std::tie(m_iconOn, m_iconOff) = buildIcons(app);

    m_manager = new LightManager(this);
    connect(m_manager, &LightManager::lightAdded, this, &TrayApp::onLightAdded);
    connect(m_manager, &LightManager::lightUpdated, this, &TrayApp::updateIcon);

    m_window = new ControlWindow(m_manager, m_iconOn);

    m_tray = new QSystemTrayIcon(m_iconOff, this);
    m_tray->setToolTip(QString::fromUtf8(kAppName));
    connect(m_tray, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                onActivated(static_cast<int>(reason));
            });

    // Rebuild the icon when the user switches between light and dark themes.
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(app->styleHints(), &QStyleHints::colorSchemeChanged, this,
            &TrayApp::onSchemeChanged);
#endif

    m_menu = new QMenu();
    auto* openAction = new QAction(QStringLiteral("Open controls"), m_menu);
    connect(openAction, &QAction::triggered, m_window,
            &ControlWindow::showCentered);
    m_menu->addAction(openAction);
    m_menu->addSeparator();
    auto* toggleAll = new QAction(QStringLiteral("Toggle all lights"), m_menu);
    connect(toggleAll, &QAction::triggered, m_manager, &LightManager::toggleAll);
    m_menu->addAction(toggleAll);
    auto* refresh = new QAction(QStringLiteral("Refresh"), m_menu);
    connect(refresh, &QAction::triggered, m_manager, &LightManager::refreshAll);
    m_menu->addAction(refresh);
    m_menu->addSeparator();
    auto* quitAction = new QAction(QStringLiteral("Quit"), m_menu);
    connect(quitAction, &QAction::triggered, this, &TrayApp::quit);
    m_menu->addAction(quitAction);
    m_tray->setContextMenu(m_menu);

    m_tray->show();
    m_manager->start();
    updateIcon();
}

TrayApp::~TrayApp() {
    delete m_window;
    delete m_menu;
}

void TrayApp::onSchemeChanged() {
    std::tie(m_iconOn, m_iconOff) = buildIcons(m_app);
    m_window->setWindowIcon(m_iconOn);
    updateIcon();
}

void TrayApp::onLightAdded(const QString& id) {
    updateIcon();
    notifyOnline(id);
}

void TrayApp::onActivated(int reason) {
    if (reason == QSystemTrayIcon::Trigger ||
        reason == QSystemTrayIcon::DoubleClick) {
        // Left-click toggles the control window (right-click is handled by the
        // desktop via the context menu we registered).
        if (m_window->isVisible()) {
            if (m_window->isActiveWindow()) {
                m_window->hide();
            } else {
                m_window->raise();
                m_window->activateWindow();
            }
        } else {
            m_window->showCentered();
        }
    } else if (reason == QSystemTrayIcon::MiddleClick) {
        m_manager->toggleAll();
    }
}

void TrayApp::updateIcon() {
    const auto [on, online] = m_manager->counts();
    m_tray->setIcon(m_manager->anyOn() ? m_iconOn : m_iconOff);
    if (online == 0) {
        m_tray->setToolTip(QStringLiteral("%1 — searching…")
                               .arg(QString::fromUtf8(kAppName)));
    } else {
        m_tray->setToolTip(QStringLiteral("%1 — %2 of %3 on")
                               .arg(QString::fromUtf8(kAppName))
                               .arg(on)
                               .arg(online));
    }
}

void TrayApp::notifyOnline(const QString& id) {
    const KeyLight* light = m_manager->get(id);
    if (light) {
        m_tray->showMessage(QString::fromUtf8(kAppName),
                            QStringLiteral("Found %1").arg(light->name), m_iconOn,
                            2000);
    }
}

void TrayApp::quit() {
    m_manager->stop();
    m_app->quit();
}

}  // namespace elg
