# elgato-key-light-linux

A KDE system-tray application for controlling Elgato Key Light, Key Light Air
and Key Light Neo devices on Linux.

Networked lights are discovered automatically over mDNS; a Key Light Neo plugged
in over USB is controlled directly over USB HID, so it works even when it is not
on the network. From the tray you can toggle lights on and off, adjust
brightness and set colour temperature.

It is a native Qt 6 C++ application built with [Meson](https://mesonbuild.com/).

## Installation

Prebuilt packages for Fedora (RPM), Debian/Ubuntu (deb) and Arch
(pkg.tar.zst) are produced by CI on every push; download them from the
workflow run's artifacts.

To build a package yourself, install the build dependencies for your distro
(see below) and run one of:

```bash
make rpm      # Fedora / RPM distros  -> ~/rpmbuild/RPMS/<arch>/
make deb      # Debian / Ubuntu       -> ../keylights-tray_*.deb
make arch     # Arch (run as non-root) -> packaging/arch/*.pkg.tar.zst
```

Each package installs the tray binary, a desktop/autostart entry and a udev
rule that lets your desktop session control a USB-attached Key Light Neo
without root.

### Dependencies

Build: a C++17 compiler, `meson`, `ninja`, and the Qt 6 (Base), Avahi client
and hidapi development packages.

| | Fedora | Debian / Ubuntu | Arch |
|---|---|---|---|
| Qt 6 Base | `qt6-qtbase-devel` | `qt6-base-dev` | `qt6-base` |
| Qt 6 SVG (runtime) | `qt6-qtsvg` | `libqt6svg6` | `qt6-svg` |
| Avahi client | `avahi-devel` | `libavahi-client-dev` | `avahi` |
| hidapi | `hidapi-devel` | `libhidapi-dev` | `hidapi` |

Runtime library dependencies are resolved automatically by each package
manager. The Qt 6 SVG package provides the image-format plugin used to render
the scalable fallback tray icon and is declared as an explicit runtime
dependency.

USB support is optional at build time (Meson `-Dusb=disabled` builds an
HTTP-only binary if hidapi is unavailable).

## Usage

Launch **Key Lights** from the application menu (it also autostarts on login),
then open the controller from its tray icon. Discovered lights each get a card
with an on/off button, a brightness slider and a colour-temperature slider. A
USB-attached Neo is labelled `(USB)`.

## USB support (Key Light Neo)

The Neo speaks the same `/elgato/lights` API as the networked lights, tunnelled
over USB HID (512-byte framed) rather than HTTP. See `src/usbneo.cpp` for the
transport and `src/lightmanager.cpp` for how it is wired into discovery and
control. The protocol was reverse-engineered by Zameer Manji:
<https://zameermanji.com/blog/2026/3/4/elgato-key-light-neo-usb-protocol/>.

USB access needs a udev rule granting the seated user access to the device. The
packages install it; for a development checkout install it manually:

```bash
sudo install -m644 udev/70-keylights-tray.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger -c add -s usb --attr-match=idVendor=0fd9   # or replug the light
```

## Development

```bash
make build    # configure + compile with Meson (build/)
make run      # launch the tray app from this checkout
make probe    # dump the raw USB HID exchange with an attached Neo
make test     # run the unit tests (USB framing codec, unit conversions)
```

The source is organised as: `src/keylight.h` (data model + constants),
`src/lightmanager.*` (device cache and control policy), `src/httptransport.*`
(networked lights over Qt Network), `src/avahibrowser.*` (mDNS discovery over
avahi-client, with a small Qt event-loop bridge), `src/usbneo.*` (Key Light Neo
over USB HID on a worker thread), and `src/trayapp.*` / `src/controlwindow.*`
(the Qt Widgets UI).
