#pragma once

// The tray presence: a QSystemTrayIcon with a native context menu, an icon that
// tracks whether any light is on (and follows the light/dark panel scheme), and
// a "Found <name>" notification when a light first comes online. Left-click
// toggles the control window; middle-click toggles all lights; the right-click
// menu is rendered by the desktop (Plasma) from the QMenu we register.

#include <QIcon>
#include <QObject>

class QApplication;
class QMenu;
class QSystemTrayIcon;

namespace elg {

class LightManager;
class ControlWindow;

class TrayApp : public QObject {
    Q_OBJECT
public:
    explicit TrayApp(QApplication* app);
    ~TrayApp() override;

private slots:
    void onSchemeChanged();
    void onLightAdded(const QString& id);
    void updateIcon();

private:
    void onActivated(int reason);
    void notifyOnline(const QString& id);
    void quit();

    QApplication* m_app;
    QIcon m_iconOn;
    QIcon m_iconOff;
    LightManager* m_manager;
    ControlWindow* m_window;
    QSystemTrayIcon* m_tray;
    QMenu* m_menu;
};

}  // namespace elg
