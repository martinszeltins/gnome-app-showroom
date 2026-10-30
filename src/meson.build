blueprints = custom_target('blueprints',
  input: files(
    'sr-app-card.blp',
    'sr-details-dialog.blp',
    'sr-window.blp',
  ),
  output: '.',
  command: [
    find_program('blueprint-compiler'),
    'batch-compile',
    '@OUTPUT@',
    '@CURRENT_SOURCE_DIR@',
    '@INPUT@',
  ],
)

resources = gnome.compile_resources('sr-resources',
  'showroom.gresource.xml',
  c_name: 'sr',
  dependencies: blueprints,
)

sources = files(
  'main.c',
  'sr-app.c',
  'sr-app-card.c',
  'sr-app-scanner.c',
  'sr-appimage.c',
  'sr-application.c',
  'sr-bundle-probe.c',
  'sr-catalog.c',
  'sr-details-dialog.c',
  'sr-dpkg-index.c',
  'sr-host.c',
  'sr-package.c',
  'sr-package-scanner.c',
  'sr-standalone-scanner.c',
  'sr-utils.c',
  'sr-window.c',
)

deps = [
  dependency('gio-unix-2.0', version: '>= 2.84'),
  dependency('gtk4', version: '>= 4.18'),
  dependency('libadwaita-1', version: '>= 1.7'),
  dependency('json-glib-1.0'),
  dependency('libarchive'),
]

executable('app-showroom',
  sources,
  resources,
  dependencies: deps,
  include_directories: include_directories('..'),
  install: true,
)
