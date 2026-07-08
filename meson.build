project('keylights-tray', 'cpp',
  version: '0.2.0',
  license: 'MIT',
  meson_version: '>=1.1',
  default_options: ['cpp_std=c++17', 'warning_level=2'])

qt6 = import('qt6')
# Svg is intentionally not a link module: QIcon renders the scalable fallback
# icon through a runtime image-format plugin (libqsvg), so nothing here links
# against libQt6Svg. The plugin is declared as a runtime dependency by the
# distro packaging instead.
qt_dep = dependency('qt6', modules: ['Core', 'Gui', 'Widgets', 'Network'])
avahi_dep = dependency('avahi-client')
hidapi_dep = dependency('hidapi-hidraw', required: get_option('usb'))
usb_enabled = hidapi_dep.found()

conf = configuration_data()
conf.set_quoted('APP_VERSION', meson.project_version())
conf.set10('HAVE_USB', usb_enabled)
version_h = configure_file(
  input: 'src/version.h.in',
  output: 'version.h',
  configuration: conf)

sources = files(
  'src/main.cpp',
  'src/lightmanager.cpp',
  'src/httptransport.cpp',
  'src/avahibrowser.cpp',
  'src/trayapp.cpp',
  'src/controlwindow.cpp',
)
moc_headers = files(
  'src/lightmanager.h',
  'src/httptransport.h',
  'src/avahibrowser.h',
  'src/trayapp.h',
  'src/controlwindow.h',
)

deps = [qt_dep, avahi_dep]
if usb_enabled
  sources += files('src/usbneo.cpp')
  moc_headers += files('src/usbneo.h')
  deps += hidapi_dep
endif

moc_files = qt6.preprocess(moc_headers: moc_headers, dependencies: qt_dep)

executable('keylights-tray',
  sources, moc_files, version_h,
  dependencies: deps,
  install: true)

# Unit tests for the USB framing codec and unit conversions (no hardware).
if usb_enabled
  test_moc = qt6.preprocess(moc_headers: files('src/usbneo.h'),
                            dependencies: qt_dep)
  test_framing = executable('test_framing',
    'tests/test_framing.cpp', 'src/usbneo.cpp', test_moc, version_h,
    dependencies: [qt_dep, hidapi_dep],
    build_by_default: false)
  test('framing', test_framing)
endif

# --- data / assets, matching the packaging install contract -----------------

datadir = get_option('datadir')
icondir = datadir / 'icons' / 'hicolor' / 'scalable' / 'apps'

install_data('assets/keylights-tray.svg', install_dir: icondir)
install_data('assets/keylights-tray-symbolic.svg', install_dir: icondir)
install_data('assets/keylights-tray-off-symbolic.svg', install_dir: icondir)
install_data('assets/elgato.png',
  install_dir: datadir / 'pixmaps',
  rename: 'keylights-tray.png')

install_data('data/keylights-tray.desktop',
  install_dir: datadir / 'applications')

autostartdir = get_option('autostartdir')
if autostartdir != ''
  install_data('data/keylights-tray.desktop', install_dir: autostartdir)
endif

udevrulesdir = get_option('udevrulesdir')
if udevrulesdir == ''
  udevrulesdir = get_option('prefix') / 'lib' / 'udev' / 'rules.d'
endif
install_data('udev/70-keylights-tray.rules', install_dir: udevrulesdir)

gnome = import('gnome')
gnome.post_install(gtk_update_icon_cache: true)

summary({
  'USB (Key Light Neo)': usb_enabled,
  'udev rules dir': udevrulesdir,
  'autostart dir': autostartdir != '' ? autostartdir : 'disabled',
}, section: 'Configuration')
