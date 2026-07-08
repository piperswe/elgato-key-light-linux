#include "controlwindow.h"

#include <QApplication>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSlider>
#include <QVBoxLayout>

#include "keylight.h"
#include "lightmanager.h"

namespace elg {

namespace {

constexpr auto kAppName = "Key Lights";

// Theme-aware styling; palette(...) references keep it readable in light and
// dark schemes. Carried over verbatim from the Python app.
constexpr auto kWindowQss = R"(
QFrame#card {
    border: 1px solid palette(mid);
    border-radius: 10px;
    background: palette(base);
}
QLabel#lightName {
    font-weight: bold;
}
QLabel#status {
    font-weight: bold;
}
QPushButton#onToggle {
    padding: 4px 14px;
    border-radius: 10px;
    border: 1px solid palette(mid);
}
QPushButton#onToggle:checked {
    background: palette(highlight);
    color: palette(highlighted-text);
    border: 1px solid palette(highlight);
}
)";

}  // namespace

// -- LabeledSlider -----------------------------------------------------------

LabeledSlider::LabeledSlider(const QString& title, int minimum, int maximum,
                             std::function<QString(int)> fmt, bool inverted,
                             QWidget* parent)
    : QWidget(parent), m_title(title), m_fmt(std::move(fmt)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    m_label = new QLabel(this);
    layout->addWidget(m_label);

    m_slider = new QSlider(Qt::Horizontal, this);
    m_slider->setMinimum(minimum);
    m_slider->setMaximum(maximum);
    m_slider->setInvertedAppearance(inverted);
    layout->addWidget(m_slider);

    connect(m_slider, &QSlider::valueChanged, this,
            [this](int value) { renderLabel(value); });
    renderLabel(m_slider->value());
}

void LabeledSlider::renderLabel(int value) {
    m_label->setText(QStringLiteral("%1: %2").arg(m_title, m_fmt(value)));
}

void LabeledSlider::setValueSilent(int value) {
    {
        QSignalBlocker blocker(m_slider);
        m_slider->setValue(value);
    }
    renderLabel(value);
}

bool LabeledSlider::isHeld() const {
    return m_slider->isSliderDown();
}

// -- LightCard ---------------------------------------------------------------

LightCard::LightCard(LightManager* manager, const QString& id, QWidget* parent)
    : QFrame(parent), m_manager(manager), m_id(id) {
    setObjectName(QStringLiteral("card"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 12);
    layout->setSpacing(8);

    auto* header = new QHBoxLayout();
    m_name = new QLabel(this);
    m_name->setObjectName(QStringLiteral("lightName"));
    header->addWidget(m_name);
    header->addStretch(1);
    m_onButton = new QPushButton(QStringLiteral("Off"), this);
    m_onButton->setObjectName(QStringLiteral("onToggle"));
    m_onButton->setCheckable(true);
    connect(m_onButton, &QPushButton::toggled, this, [this](bool checked) {
        m_onButton->setText(checked ? QStringLiteral("On") : QStringLiteral("Off"));
        m_manager->setOn(m_id, checked);
    });
    header->addWidget(m_onButton);
    layout->addLayout(header);

    m_brightness = new LabeledSlider(QStringLiteral("Brightness"), 0, 100,
                                     [](int v) { return QStringLiteral("%1 %").arg(v); },
                                     false, this);
    connect(m_brightness->slider(), &QSlider::valueChanged, this,
            [this](int value) { m_manager->setBrightness(m_id, value); });
    connect(m_brightness->slider(), &QSlider::sliderReleased, this,
            [this]() { m_manager->flushPending(m_id); });
    layout->addWidget(m_brightness);

    // Higher Kelvin (cooler/bluer) is fewer mireds, so invert the slider: drag
    // right raises the Kelvin value shown.
    m_temperature = new LabeledSlider(QStringLiteral("Temperature"), kMiredMin,
                                      kMiredMax, [](int v) { return kelvinLabel(v); },
                                      true, this);
    connect(m_temperature->slider(), &QSlider::valueChanged, this,
            [this](int value) { m_manager->setTemperature(m_id, value); });
    connect(m_temperature->slider(), &QSlider::sliderReleased, this,
            [this]() { m_manager->flushPending(m_id); });
    layout->addWidget(m_temperature);
}

void LightCard::sync() {
    const KeyLight* light = m_manager->get(m_id);
    if (!light) {
        return;
    }
    QString name = light->name;
    if (light->transport == Transport::Usb) {
        name += QStringLiteral(" (USB)");
    }
    if (!light->online) {
        name += QStringLiteral(" (offline)");
    }
    m_name->setText(name);

    const bool enabled = light->online && light->state.has_value();
    m_onButton->setEnabled(enabled);
    m_brightness->setEnabled(enabled);
    m_temperature->setEnabled(enabled);

    if (!light->state) {
        return;
    }
    {
        QSignalBlocker blocker(m_onButton);
        m_onButton->setChecked(light->state->on);
        m_onButton->setText(light->state->on ? QStringLiteral("On")
                                             : QStringLiteral("Off"));
    }
    // Don't fight a slider the user is currently dragging.
    if (!m_brightness->isHeld()) {
        m_brightness->setValueSilent(light->state->brightness);
    }
    if (!m_temperature->isHeld()) {
        m_temperature->setValueSilent(light->state->temperature);
    }
}

// -- ControlWindow -----------------------------------------------------------

ControlWindow::ControlWindow(LightManager* manager, const QIcon& icon)
    : m_manager(manager) {
    setWindowTitle(QString::fromUtf8(kAppName));
    setWindowIcon(icon);
    setMinimumWidth(380);
    setStyleSheet(QString::fromUtf8(kWindowQss));

    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 14, 14, 14);
    outer->setSpacing(12);

    auto* header = new QHBoxLayout();
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("status"));
    header->addWidget(m_status);
    header->addStretch(1);
    m_toggleAll = new QPushButton(QStringLiteral("Toggle all"), this);
    connect(m_toggleAll, &QPushButton::clicked, m_manager,
            &LightManager::toggleAll);
    header->addWidget(m_toggleAll);
    outer->addLayout(header);

    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget();
    m_cardsLayout = new QVBoxLayout(body);
    m_cardsLayout->setContentsMargins(0, 0, 0, 0);
    m_cardsLayout->setSpacing(10);
    m_placeholder = new QLabel(QStringLiteral("Searching for lights…"), body);
    m_placeholder->setAlignment(Qt::AlignCenter);
    m_placeholder->setEnabled(false);
    m_cardsLayout->addWidget(m_placeholder);
    m_cardsLayout->addStretch(1);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    connect(m_manager, &LightManager::lightAdded, this,
            &ControlWindow::onLightAdded);
    connect(m_manager, &LightManager::lightUpdated, this,
            &ControlWindow::onLightUpdated);
    connect(m_manager, &LightManager::lightRemoved, this,
            &ControlWindow::onLightRemoved);
    refreshHeader();
}

void ControlWindow::onLightAdded(const QString& id) {
    LightCard* card = m_cards.value(id, nullptr);
    if (!card) {
        card = new LightCard(m_manager, id, this);
        m_cards.insert(id, card);
        // Insert before the trailing stretch.
        m_cardsLayout->insertWidget(m_cardsLayout->count() - 1, card);
    }
    card->sync();
    refreshHeader();
}

void ControlWindow::onLightUpdated(const QString& id) {
    LightCard* card = m_cards.value(id, nullptr);
    if (card) {
        card->sync();
    }
    refreshHeader();
}

void ControlWindow::onLightRemoved(const QString& id) {
    LightCard* card = m_cards.take(id);
    if (card) {
        m_cardsLayout->removeWidget(card);
        card->deleteLater();
    }
    refreshHeader();
}

void ControlWindow::refreshHeader() {
    const auto [on, online] = m_manager->counts();
    m_placeholder->setVisible(m_cards.isEmpty());
    m_toggleAll->setEnabled(online > 0);
    if (online == 0) {
        m_status->setText(QStringLiteral("Searching for lights…"));
    } else {
        m_status->setText(QStringLiteral("%1 of %2 lights on").arg(on).arg(online));
    }
}

void ControlWindow::showCentered() {
    adjustSize();
    QScreen* screen = this->screen();
    if (!screen) {
        screen = QApplication::primaryScreen();
    }
    if (screen) {
        const QPoint center = screen->availableGeometry().center();
        move(center - rect().center());
    }
    show();
    raise();
    activateWindow();
}

void ControlWindow::closeEvent(QCloseEvent* event) {
    // The X button hides the window; the app keeps running in the tray.
    event->ignore();
    hide();
}

}  // namespace elg
