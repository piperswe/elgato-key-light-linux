#pragma once

// The control window: one card per discovered light, each with an On/Off toggle
// plus brightness and colour-temperature sliders. Widgets live here (never in
// the tray menu), so the menu can be exported natively over DBusMenu on Plasma.

#include <functional>

#include <QFrame>
#include <QHash>
#include <QIcon>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;
class QSlider;
class QVBoxLayout;

namespace elg {

class LightManager;

// A title/value label above a horizontal slider.
class LabeledSlider : public QWidget {
    Q_OBJECT
public:
    LabeledSlider(const QString& title, int minimum, int maximum,
                  std::function<QString(int)> fmt, bool inverted = false,
                  QWidget* parent = nullptr);

    QSlider* slider() const { return m_slider; }
    void setValueSilent(int value);
    bool isHeld() const;

private:
    void renderLabel(int value);

    QString m_title;
    std::function<QString(int)> m_fmt;
    QLabel* m_label;
    QSlider* m_slider;
};

// One light's controls: name + On/Off toggle, brightness, temperature.
class LightCard : public QFrame {
    Q_OBJECT
public:
    LightCard(LightManager* manager, const QString& id, QWidget* parent = nullptr);
    void sync();  // reflect cache state into the widgets without emitting changes

private:
    LightManager* m_manager;
    QString m_id;
    QLabel* m_name;
    QPushButton* m_onButton;
    LabeledSlider* m_brightness;
    LabeledSlider* m_temperature;
};

// Centred window holding one card per discovered light.
class ControlWindow : public QWidget {
    Q_OBJECT
public:
    ControlWindow(LightManager* manager, const QIcon& icon);
    void showCentered();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void onLightAdded(const QString& id);
    void onLightUpdated(const QString& id);
    void onLightRemoved(const QString& id);

private:
    void refreshHeader();

    LightManager* m_manager;
    QHash<QString, LightCard*> m_cards;
    QVBoxLayout* m_cardsLayout;
    QLabel* m_status;
    QLabel* m_placeholder;
    QPushButton* m_toggleAll;
};

}  // namespace elg
