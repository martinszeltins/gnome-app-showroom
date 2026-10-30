desktop_file = i18n.merge_file(
  input: configure_file(
    input: 'io.github.martins.AppShowroom.desktop.in.in',
    output: 'io.github.martins.AppShowroom.desktop.in',
    configuration: {'APP_ID': app_id},
  ),
  output: app_id + '.desktop',
  type: 'desktop',
  po_dir: '../po',
  install: true,
  install_dir: datadir / 'applications',
)

desktop_utils = find_program('desktop-file-validate', required: false)
if desktop_utils.found()
  test('validate-desktop', desktop_utils, args: [desktop_file])
endif

metainfo_file = i18n.merge_file(
  input: configure_file(
    input: 'io.github.martins.AppShowroom.metainfo.xml.in.in',
    output: 'io.github.martins.AppShowroom.metainfo.xml.in',
    configuration: {'APP_ID': app_id},
  ),
  output: app_id + '.metainfo.xml',
  po_dir: '../po',
  install: true,
  install_dir: datadir / 'metainfo',
)

appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli,
    args: ['validate', '--no-net', '--explain', metainfo_file])
endif

install_data(app_id + '.gschema.xml',
  install_dir: datadir / 'glib-2.0' / 'schemas',
)

compile_schemas = find_program('glib-compile-schemas', required: false)
if compile_schemas.found()
  test('validate-schema', compile_schemas,
    args: ['--strict', '--dry-run', meson.current_source_dir()])
endif

subdir('icons')
