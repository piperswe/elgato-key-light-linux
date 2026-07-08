# elgato-key-light-linux

A KDE system-tray application for controlling Elgato Key Light, Key Light Air
and Key Light Neo devices on Linux.

Networked lights are discovered automatically over mDNS; a Key Light Neo plugged
in over USB is controlled directly over USB HID, so it works even when it is not
on the network. From the tray you can toggle lights on and off, adjust
brightness and set colour temperature.

## Installation

Build and install the RPM (Fedora / other RPM distros):

```bash
make install
```

This installs the tray app, a desktop/autostart entry and a udev rule that lets
your desktop session control a USB-attached Key Light Neo without root.

### Dependencies

Runtime: `python3-pyside6`, `python3-zeroconf`, `python3-requests`,
`python3-hidapi`. These are declared in the RPM spec and pulled in by `dnf`.

## Usage

Launch **Key Lights** from the application menu (it also autostarts on login),
then open the controller from its tray icon. Discovered lights each get a card
with an on/off button, a brightness slider and a colour-temperature slider. A
USB-attached Neo is labelled `(USB)`.

## USB support (Key Light Neo)

The Neo speaks the same `/elgato/lights` API as the networked lights, tunnelled
over USB HID (512-byte framed) rather than HTTP. See `tray/usbneo.py` for the
transport and `tray/lights.py` for how it is wired into discovery and control.
The protocol was reverse-engineered by Zameer Manji:
<https://zameermanji.com/blog/2026/3/4/elgato-key-light-neo-usb-protocol/>.

USB access needs a udev rule granting the seated user access to the device. The
RPM installs it; for a development checkout install it manually:

```bash
sudo install -m644 udev/70-keylights-tray.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger -c add -s usb --attr-match=idVendor=0fd9   # or replug the light
```

## Development

```bash
make run      # launch the tray app from this checkout
make probe    # dump the raw USB HID exchange with an attached Neo
make rpm      # build a binary RPM from the working tree
```

`python3 tray/lights.py` runs a headless discovery smoke test (mDNS + USB) with
no GUI.
