Name:           keylights-tray
Version:        0.2.0
Release:        1%{?dist}
Summary:        KDE system tray controller for Elgato Key Lights

License:        MIT
URL:            https://github.com/nsahq/elgato-key-light-linux
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  meson
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  qt6-qtbase-devel
BuildRequires:  avahi-devel
BuildRequires:  hidapi-devel
BuildRequires:  systemd-rpm-macros
BuildRequires:  desktop-file-utils

# qt6-qtsvg ships the runtime SVG image-format plugin QIcon uses for the
# scalable fallback icon; it is loaded at runtime, not linked, so it must be
# required explicitly.
Requires:       qt6-qtsvg
Requires:       hicolor-icon-theme

%description
A system tray application for controlling Elgato Key Light, Key Light Air and
Key Light Neo devices on Linux. Discovers networked lights over mDNS and the
USB-attached Key Light Neo over HID, and lets you toggle them, adjust
brightness and set colour temperature from a control window opened from the
KDE Plasma system tray.

%prep
%autosetup

%build
%meson -Dudevrulesdir=%{_udevrulesdir}
%meson_build

%install
%meson_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/%{name}.desktop

%post
# Apply the new udev rule to an already-plugged Neo without a replug.
udevadm control --reload &>/dev/null || :
udevadm trigger -c add -s usb --attr-match=idVendor=0fd9 &>/dev/null || :

%files
%license LICENSE
%doc README.md
%{_bindir}/%{name}
%{_udevrulesdir}/70-keylights-tray.rules
%{_datadir}/icons/hicolor/scalable/apps/%{name}.svg
%{_datadir}/icons/hicolor/scalable/apps/%{name}-symbolic.svg
%{_datadir}/icons/hicolor/scalable/apps/%{name}-off-symbolic.svg
%{_datadir}/pixmaps/%{name}.png
%{_datadir}/applications/%{name}.desktop
%{_sysconfdir}/xdg/autostart/%{name}.desktop

%changelog
* Wed Jul 08 2026 Piper McCorkle <piper.mccorkle@grafana.com> - 0.2.0-1
- Rewrite as a native Qt 6 C++ application built with Meson.
- mDNS discovery via avahi-client; USB HID via hidapi; HTTP via Qt Network.

* Wed Jul 08 2026 Piper McCorkle <piper.mccorkle@grafana.com> - 0.1.0-1
- Initial tray application.
- Add USB HID support for the Key Light Neo, including a udev rule for
  unprivileged access.
