Name:           keylights-tray
Version:        0.1.0
Release:        1%{?dist}
Summary:        KDE system tray controller for Elgato Key Lights

License:        MIT
URL:            https://github.com/nsahq/elgato-key-light-linux
Source0:        %{name}-%{version}.tar.gz

BuildArch:      noarch

BuildRequires:  make
BuildRequires:  systemd-rpm-macros

Requires:       python3-pyside6
Requires:       python3-zeroconf
Requires:       python3-requests
Requires:       python3-hidapi
Requires:       hicolor-icon-theme

%description
A system tray application for controlling Elgato Key Light, Key Light Air and
Key Light Neo devices on Linux. Discovers networked lights over mDNS and the
USB-attached Key Light Neo over HID, and lets you toggle them, adjust
brightness and set colour temperature from a control window opened from the
KDE Plasma system tray.

%prep
%setup -q

%install
# Application code.
install -Dm755 tray/keylights-tray.py %{buildroot}%{_datadir}/%{name}/keylights-tray.py
install -Dm644 tray/lights.py          %{buildroot}%{_datadir}/%{name}/lights.py
install -Dm644 tray/usbneo.py          %{buildroot}%{_datadir}/%{name}/usbneo.py

# udev rule granting the seated user access to the Key Light Neo over USB HID.
install -Dm644 udev/70-keylights-tray.rules \
    %{buildroot}%{_udevrulesdir}/70-keylights-tray.rules

# Launcher on PATH. Keeps lights.py importable: Python prepends the real
# script's directory to sys.path, so the sibling module resolves.
install -d %{buildroot}%{_bindir}
cat > %{buildroot}%{_bindir}/%{name} <<'EOF'
#!/bin/bash
exec %{_datadir}/%{name}/keylights-tray.py "$@"
EOF
chmod 0755 %{buildroot}%{_bindir}/%{name}

# Themed icons. Installing the scalable SVGs into the hicolor theme lets the
# desktop entry's "Icon=keylights-tray" resolve, and lets the tray request them
# by name so Plasma scales them to fill the panel slot and recolours the
# "-symbolic" variants for light/dark contrast.
install -Dm644 assets/keylights-tray.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
install -Dm644 assets/keylights-tray-symbolic.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{name}-symbolic.svg
install -Dm644 assets/keylights-tray-off-symbolic.svg \
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/%{name}-off-symbolic.svg

# Legacy raster fallback for non-theme icon lookups.
install -Dm644 assets/elgato.png %{buildroot}%{_datadir}/pixmaps/%{name}.png

# Desktop entry: application menu and per-session autostart.
install -Dm644 tray/keylights-tray.desktop %{buildroot}%{_datadir}/applications/%{name}.desktop
install -Dm644 tray/keylights-tray.desktop %{buildroot}%{_sysconfdir}/xdg/autostart/%{name}.desktop

%post
touch --no-create %{_datadir}/icons/hicolor &>/dev/null || :
# Apply the new udev rule to an already-plugged Neo without a replug.
udevadm control --reload &>/dev/null || :
udevadm trigger -c add -s usb --attr-match=idVendor=0fd9 &>/dev/null || :

%postun
if [ $1 -eq 0 ] ; then
    touch --no-create %{_datadir}/icons/hicolor &>/dev/null
    gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :
fi

%posttrans
gtk-update-icon-cache %{_datadir}/icons/hicolor &>/dev/null || :

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%{_datadir}/%{name}/
%{_udevrulesdir}/70-keylights-tray.rules
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
%{_datadir}/icons/hicolor/scalable/apps/%{name}-symbolic.svg
%{_datadir}/icons/hicolor/scalable/apps/%{name}-off-symbolic.svg
%{_datadir}/pixmaps/%{name}.png
%{_datadir}/applications/%{name}.desktop
%{_sysconfdir}/xdg/autostart/%{name}.desktop

%changelog
* Wed Jul 08 2026 Piper McCorkle <piper.mccorkle@grafana.com> - 0.1.0-1
- Initial tray application.
- Add USB HID support for the Key Light Neo, including a udev rule for
  unprivileged access.
