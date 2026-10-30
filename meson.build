project(
  'app-showroom',
  'c',
  version: '0.1.0',
  meson_version: '>= 1.0.0',
  license: 'GPL-3.0-or-later',
  default_options: [
    'warning_level=2',
    'c_std=gnu11',
  ],
)

i18n = import('i18n')
gnome = import('gnome')

app_id = 'io.github.martins.AppShowroom'
resource_base = '/io/github/martins/AppShowroom'

prefix = get_option('prefix')
datadir = prefix / get_option('datadir')
localedir = prefix / get_option('localedir')

cc = meson.get_compiler('c')
add_project_arguments(
  cc.get_supported_arguments([
    '-Wno-unused-parameter',
    '-Wno-missing-field-initializers',
    '-Wno-cast-function-type',
    '-Werror=implicit-function-declaration',
    '-Werror=incompatible-pointer-types',
  ]),
  language: 'c',
)

config_h = configuration_data()
config_h.set_quoted('APPLICATION_ID', app_id)
config_h.set_quoted('RESOURCE_BASE', resource_base)
config_h.set_quoted('PACKAGE_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'app-showroom')
config_h.set_quoted('LOCALEDIR', localedir)
configure_file(output: 'config.h', configuration: config_h)

subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
