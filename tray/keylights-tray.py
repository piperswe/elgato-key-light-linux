#!/usr/bin/env python3
"""KDE system tray controller for Elgato Key Lights.

Left-clicking the tray icon opens a centred control window with a card per
light — an On/Off toggle plus brightness and colour-temperature sliders.
Right-clicking shows a small native menu (Open controls, Toggle all, Refresh,
Quit); middle-clicking toggles all lights.

The controls live in a normal window rather than the tray menu because slider
widgets can't be drawn in a menu exported over Plasma's DBusMenu. Keeping the
menu free of custom widgets lets us register it with ``setContextMenu`` so
Plasma renders and positions it natively — and lets the whole app run on native
Wayland without XWayland/cursor workarounds.
"""

from __future__ import annotations

import signal
import sys
from pathlib import Path
from typing import Callable, Optional

from PySide6.QtCore import QObject, Qt
from PySide6.QtGui import (
    QAction,
    QColor,
    QIcon,
    QPainter,
    QPalette,
    QPixmap,
)
from PySide6.QtWidgets import (
    QApplication,
    QFrame,
    QHBoxLayout,
    QLabel,
    QMenu,
    QPushButton,
    QScrollArea,
    QSlider,
    QSystemTrayIcon,
    QVBoxLayout,
    QWidget,
)

from lights import (
    MIRED_MAX,
    MIRED_MIN,
    KeyLight,
    LightManager,
    kelvin_label,
)

APP_NAME = "Key Lights"
NOTIFY = True

# Themed icon names. On KDE the tray delivers these by name over the
# StatusNotifierItem protocol, so Plasma scales them to fill the panel slot
# (a pixmap-array icon is not scaled up, which made the glyph render tiny) and
# recolours the "-symbolic" variants for light/dark panel contrast.
_ICON_ON = "keylights-tray-symbolic"
_ICON_OFF = "keylights-tray-off-symbolic"

# Fallback assets, tried in order, for environments without the installed theme
# icon (a checkout, or a non-KDE tray). The scalable SVG is preferred; the
# legacy PNG is a last resort.
_HERE = Path(__file__).resolve().parent
_SVG_CANDIDATES = (
    _HERE.parent / "assets" / "keylights-tray.svg",
    Path("/usr/share/icons/hicolor/scalable/apps/keylights-tray.svg"),
)
_PNG_CANDIDATES = (
    _HERE.parent / "assets" / "elgato.png",
    Path("/usr/share/pixmaps/keylights-tray.png"),
)
# Square pixmap size used only for the fallback path; a single large square
# pixmap lets the tray scale it to fill the slot.
_FALLBACK_SIZE = 512

# Opacity of the icon when all lights are off — dims the same glyph.
_OFF_OPACITY = 0.4
# High-contrast monochrome foregrounds for each scheme.
_LIGHT_ON_DARK = QColor(244, 244, 244)
_DARK_ON_LIGHT = QColor(45, 45, 45)


def _first_existing(paths: "tuple[Path, ...]") -> "Optional[Path]":
    return next((p for p in paths if p.is_file()), None)


def load_base_pixmap() -> QPixmap:
    """Load the source glyph for the fallback path (SVG preferred, then PNG)."""
    svg = _first_existing(_SVG_CANDIDATES)
    if svg is not None:
        pm = QIcon(str(svg)).pixmap(_FALLBACK_SIZE, _FALLBACK_SIZE)
        if not pm.isNull():
            return pm
    png = _first_existing(_PNG_CANDIDATES)
    if png is not None:
        pm = QPixmap(str(png))
        if not pm.isNull():
            return pm
    return QIcon.fromTheme("weather-clear").pixmap(_FALLBACK_SIZE, _FALLBACK_SIZE)


def foreground_for_scheme(app: "QApplication") -> QColor:
    """Pick a monochrome colour with strong contrast against the current panel.

    The tray icon sits on the panel, whose colour follows the light/dark theme,
    so we recolour to near-white on dark and near-black on light.
    """
    scheme = None
    try:
        scheme = app.styleHints().colorScheme()
    except (AttributeError, TypeError):
        scheme = None
    if scheme == Qt.ColorScheme.Dark:
        return _LIGHT_ON_DARK
    if scheme == Qt.ColorScheme.Light:
        return _DARK_ON_LIGHT
    # Unknown scheme: infer from the window background lightness.
    bg = app.palette().color(QPalette.ColorRole.Window)
    return _LIGHT_ON_DARK if bg.lightness() < 128 else _DARK_ON_LIGHT


def _recolor(src: QPixmap, color: QColor) -> QPixmap:
    """Recolour a pixmap to a flat colour, using its alpha channel as the mask."""
    out = QPixmap(src.size())
    out.fill(Qt.GlobalColor.transparent)
    p = QPainter(out)
    p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)
    p.drawPixmap(0, 0, src)
    p.setCompositionMode(QPainter.CompositionMode.CompositionMode_SourceIn)
    p.fillRect(out.rect(), color)
    p.end()
    return out


def _dim(src: QPixmap, opacity: float) -> QPixmap:
    """Return a copy of src scaled down in opacity."""
    out = QPixmap(src.size())
    out.fill(Qt.GlobalColor.transparent)
    p = QPainter(out)
    p.setOpacity(opacity)
    p.drawPixmap(0, 0, src)
    p.end()
    return out


def _fallback_icon(app: "QApplication", dim: bool) -> QIcon:
    """Recolour the source glyph into a single large square pixmap.

    Used only when the themed icon is unavailable. One large square pixmap
    (rather than a graded size set) lets the tray scale it to fill its slot.
    """
    base = load_base_pixmap().scaled(
        _FALLBACK_SIZE,
        _FALLBACK_SIZE,
        Qt.AspectRatioMode.KeepAspectRatio,
        Qt.TransformationMode.SmoothTransformation,
    )
    tinted = _recolor(base, foreground_for_scheme(app))
    if dim:
        tinted = _dim(tinted, _OFF_OPACITY)
    icon = QIcon()
    icon.addPixmap(tinted)
    return icon


def build_icons(app: "QApplication") -> tuple[QIcon, QIcon]:
    """Return (on_icon, off_icon), preferring the recolourable themed icons.

    On KDE these resolve by name so Plasma sizes and tints them; elsewhere we
    fall back to a locally recoloured pixmap.
    """
    on_icon = QIcon.fromTheme(_ICON_ON)
    if on_icon.isNull():
        on_icon = _fallback_icon(app, dim=False)
    off_icon = QIcon.fromTheme(_ICON_OFF)
    if off_icon.isNull():
        off_icon = _fallback_icon(app, dim=True)
    return on_icon, off_icon


# Light, theme-aware styling for the control window. palette(...) references
# keep it readable in both light and dark schemes.
_WINDOW_QSS = """
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
"""


class LabeledSlider(QWidget):
    """A title/value label above a horizontal slider."""

    def __init__(
        self,
        title: str,
        minimum: int,
        maximum: int,
        fmt: Callable[[int], str],
        inverted: bool = False,
        parent: Optional[QWidget] = None,
    ):
        super().__init__(parent)
        self._title = title
        self._fmt = fmt

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(2)

        self._label = QLabel()
        layout.addWidget(self._label)

        self.slider = QSlider(Qt.Orientation.Horizontal)
        self.slider.setMinimum(minimum)
        self.slider.setMaximum(maximum)
        self.slider.setInvertedAppearance(inverted)
        layout.addWidget(self.slider)

        self.slider.valueChanged.connect(self._on_value_changed)
        self._render_label(self.slider.value())

    def _render_label(self, value: int):
        self._label.setText(f"{self._title}: {self._fmt(value)}")

    def _on_value_changed(self, value: int):
        self._render_label(value)

    def set_value_silent(self, value: int):
        blocked = self.slider.blockSignals(True)
        self.slider.setValue(value)
        self.slider.blockSignals(blocked)
        self._render_label(value)

    def is_held(self) -> bool:
        return self.slider.isSliderDown()


class LightCard(QFrame):
    """One light's controls: name + On/Off toggle, brightness, temperature."""

    def __init__(self, manager: LightManager, light_id: str, parent: Optional[QWidget] = None):
        super().__init__(parent)
        self._manager = manager
        self._light_id = light_id
        self.setObjectName("card")

        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 10, 12, 12)
        layout.setSpacing(8)

        header = QHBoxLayout()
        self._name = QLabel()
        self._name.setObjectName("lightName")
        header.addWidget(self._name)
        header.addStretch(1)
        self.on_button = QPushButton("Off")
        self.on_button.setObjectName("onToggle")
        self.on_button.setCheckable(True)
        self.on_button.toggled.connect(self._on_toggled)
        header.addWidget(self.on_button)
        layout.addLayout(header)

        self.brightness = LabeledSlider("Brightness", 0, 100, lambda v: f"{v} %")
        self.brightness.slider.valueChanged.connect(self._on_brightness)
        self.brightness.slider.sliderReleased.connect(
            lambda: self._manager.flush_pending(self._light_id)
        )
        layout.addWidget(self.brightness)

        # Higher Kelvin (cooler/bluer) is fewer mireds, so invert the slider so
        # dragging right raises the Kelvin value shown.
        self.temperature = LabeledSlider(
            "Temperature", MIRED_MIN, MIRED_MAX, kelvin_label, inverted=True
        )
        self.temperature.slider.valueChanged.connect(self._on_temperature)
        self.temperature.slider.sliderReleased.connect(
            lambda: self._manager.flush_pending(self._light_id)
        )
        layout.addWidget(self.temperature)

    def _light(self) -> Optional[KeyLight]:
        return self._manager.get(self._light_id)

    def _on_toggled(self, checked: bool):
        self.on_button.setText("On" if checked else "Off")
        self._manager.set_on(self._light_id, checked)

    def _on_brightness(self, value: int):
        self._manager.set_brightness(self._light_id, value)

    def _on_temperature(self, value: int):
        self._manager.set_temperature(self._light_id, value)

    def sync(self):
        """Reflect cache state into the widgets without emitting change signals."""
        light = self._light()
        if light is None:
            return
        name = light.name
        if light.transport == "usb":
            name += " (USB)"
        if not light.online:
            name += " (offline)"
        self._name.setText(name)

        controls_enabled = light.online and light.state is not None
        self.on_button.setEnabled(controls_enabled)
        self.brightness.setEnabled(controls_enabled)
        self.temperature.setEnabled(controls_enabled)

        if light.state is None:
            return
        blocked = self.on_button.blockSignals(True)
        self.on_button.setChecked(light.state.on)
        self.on_button.setText("On" if light.state.on else "Off")
        self.on_button.blockSignals(blocked)

        # Don't fight a slider the user is currently dragging.
        if not self.brightness.is_held():
            self.brightness.set_value_silent(light.state.brightness)
        if not self.temperature.is_held():
            self.temperature.set_value_silent(light.state.temperature)


class ControlWindow(QWidget):
    """Centred window holding one card per discovered light."""

    def __init__(self, manager: LightManager, icon: QIcon):
        super().__init__()
        self._manager = manager
        self._cards: dict[str, LightCard] = {}

        self.setWindowTitle(APP_NAME)
        self.setWindowIcon(icon)
        self.setMinimumWidth(380)
        self.setStyleSheet(_WINDOW_QSS)

        outer = QVBoxLayout(self)
        outer.setContentsMargins(14, 14, 14, 14)
        outer.setSpacing(12)

        header = QHBoxLayout()
        self._status = QLabel()
        self._status.setObjectName("status")
        header.addWidget(self._status)
        header.addStretch(1)
        self._toggle_all = QPushButton("Toggle all")
        self._toggle_all.clicked.connect(self._manager.toggle_all)
        header.addWidget(self._toggle_all)
        outer.addLayout(header)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.Shape.NoFrame)
        body = QWidget()
        self._cards_layout = QVBoxLayout(body)
        self._cards_layout.setContentsMargins(0, 0, 0, 0)
        self._cards_layout.setSpacing(10)
        self._placeholder = QLabel("Searching for lights…")
        self._placeholder.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._placeholder.setEnabled(False)
        self._cards_layout.addWidget(self._placeholder)
        self._cards_layout.addStretch(1)
        scroll.setWidget(body)
        outer.addWidget(scroll, 1)

        self._manager.light_added.connect(self._on_light_added)
        self._manager.light_updated.connect(self._on_light_updated)
        self._manager.light_removed.connect(self._on_light_removed)
        self._refresh_header()

    # -- manager signal handlers --

    def _on_light_added(self, light_id: str):
        card = self._cards.get(light_id)
        if card is None:
            card = LightCard(self._manager, light_id, self)
            self._cards[light_id] = card
            # Insert before the trailing stretch.
            self._cards_layout.insertWidget(self._cards_layout.count() - 1, card)
        card.sync()
        self._refresh_header()

    def _on_light_updated(self, light_id: str):
        card = self._cards.get(light_id)
        if card is not None:
            card.sync()
        self._refresh_header()

    def _on_light_removed(self, light_id: str):
        card = self._cards.pop(light_id, None)
        if card is not None:
            self._cards_layout.removeWidget(card)
            card.deleteLater()
        self._refresh_header()

    def _refresh_header(self):
        on, online = self._manager.counts()
        self._placeholder.setVisible(not self._cards)
        self._toggle_all.setEnabled(online > 0)
        if online == 0:
            self._status.setText("Searching for lights…")
        else:
            self._status.setText(f"{on} of {online} lights on")

    # -- window behaviour --

    def show_centered(self):
        self.adjustSize()
        screen = self.screen() or QApplication.primaryScreen()
        if screen is not None:
            center = screen.availableGeometry().center()
            self.move(center - self.rect().center())
        self.show()
        self.raise_()
        self.activateWindow()

    def closeEvent(self, event):
        # The X button hides the window; the app keeps running in the tray.
        event.ignore()
        self.hide()


class TrayApp(QObject):
    def __init__(self, app: QApplication):
        super().__init__(app)
        self._app = app
        self._icon_on, self._icon_off = build_icons(app)

        self._manager = LightManager(self)
        self._manager.light_added.connect(self._on_light_added)
        self._manager.light_updated.connect(self._update_icon)

        self._window = ControlWindow(self._manager, self._icon_on)

        self.tray = QSystemTrayIcon(self._icon_off, self)
        self.tray.setToolTip(APP_NAME)
        self.tray.activated.connect(self._on_activated)

        # Rebuild the icon when the user switches between light and dark themes.
        # (An app-wide event filter would also catch palette changes, but
        # filtering every application event from Python corrupts the heap when
        # events are posted from the discovery/HTTP threads — so rely on this
        # targeted signal instead.)
        try:
            app.styleHints().colorSchemeChanged.connect(self._on_scheme_changed)
        except (AttributeError, TypeError):
            pass

        self.menu = QMenu()
        open_action = QAction("Open controls", self.menu)
        open_action.triggered.connect(self._window.show_centered)
        self.menu.addAction(open_action)
        self.menu.addSeparator()
        toggle_all = QAction("Toggle all lights", self.menu)
        toggle_all.triggered.connect(self._manager.toggle_all)
        self.menu.addAction(toggle_all)
        refresh = QAction("Refresh", self.menu)
        refresh.triggered.connect(self._manager.refresh_all)
        self.menu.addAction(refresh)
        self.menu.addSeparator()
        quit_action = QAction("Quit", self.menu)
        quit_action.triggered.connect(self._quit)
        self.menu.addAction(quit_action)
        self.tray.setContextMenu(self.menu)

        self.tray.show()
        self._manager.start()
        self._update_icon()

    def _on_scheme_changed(self, _scheme=None):
        self._icon_on, self._icon_off = build_icons(self._app)
        self._window.setWindowIcon(self._icon_on)
        self._update_icon()

    # -- manager signal handlers --

    def _on_light_added(self, light_id: str):
        self._update_icon()
        self._notify_online(light_id)

    # -- tray behaviour --

    def _on_activated(self, reason: QSystemTrayIcon.ActivationReason):
        if reason in (
            QSystemTrayIcon.ActivationReason.Trigger,
            QSystemTrayIcon.ActivationReason.DoubleClick,
        ):
            # Left-click toggles the control window (right-click is handled by
            # Plasma via the context menu we registered).
            if self._window.isVisible():
                if self._window.isActiveWindow():
                    self._window.hide()
                else:
                    self._window.raise_()
                    self._window.activateWindow()
            else:
                self._window.show_centered()
        elif reason == QSystemTrayIcon.ActivationReason.MiddleClick:
            # Middle-click stays a quick toggle shortcut.
            self._manager.toggle_all()

    def _update_icon(self, *_):
        on, online = self._manager.counts()
        any_on = self._manager.any_on()
        self.tray.setIcon(self._icon_on if any_on else self._icon_off)
        if online == 0:
            self.tray.setToolTip(f"{APP_NAME} — searching…")
        else:
            self.tray.setToolTip(f"{APP_NAME} — {on} of {online} on")

    def _notify_online(self, light_id: str):
        if not NOTIFY:
            return
        light = self._manager.get(light_id)
        if light is not None:
            self.tray.showMessage(
                APP_NAME, f"Found {light.name}", self._icon_on, 2000
            )

    def _quit(self):
        self._manager.stop()
        self._app.quit()


def main() -> int:
    # Let Ctrl+C terminate the process when run in the foreground.
    signal.signal(signal.SIGINT, signal.SIG_DFL)

    app = QApplication(sys.argv)
    app.setApplicationName(APP_NAME)
    app.setDesktopFileName("keylights-tray")
    app.setQuitOnLastWindowClosed(False)

    if not QSystemTrayIcon.isSystemTrayAvailable():
        print("No system tray available.", file=sys.stderr)
        return 1

    TrayApp(app)
    return app.exec()


if __name__ == "__main__":
    raise SystemExit(main())
