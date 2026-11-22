project('coffret', ['c', 'cpp'],
  version: files('VERSION'),
  meson_version: '>=1.1',
  default_options: [
    'cpp_std=c++23',
    'cpp_extensions=false',
    'warning_level=3',
    'b_ndebug=if-release',
  ],
)

cpp = meson.get_compiler('cpp')

wayland_protocols = dependency('wayland-protocols', version: '>=1.49')
scanner = find_program('wayland-scanner')
protocol_root = wayland_protocols.get_variable('pkgdatadir')
protocol_sources = []
protocol_headers = []
foreach protocol : [
  ['xdg-decoration-unstable-v1', 'unstable/xdg-decoration/xdg-decoration-unstable-v1.xml'],
  ['xdg-foreign-unstable-v2', 'unstable/xdg-foreign/xdg-foreign-unstable-v2.xml'],
  ['primary-selection-unstable-v1', 'unstable/primary-selection/primary-selection-unstable-v1.xml'],
  ['pointer-constraints-unstable-v1', 'unstable/pointer-constraints/pointer-constraints-unstable-v1.xml'],
  ['relative-pointer-unstable-v1', 'unstable/relative-pointer/relative-pointer-unstable-v1.xml'],
  ['keyboard-shortcuts-inhibit-unstable-v1', 'unstable/keyboard-shortcuts-inhibit/keyboard-shortcuts-inhibit-unstable-v1.xml'],
]
  header = custom_target(protocol[0] + '-header',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-client-protocol.h',
    command: [scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
  )
  source = custom_target(protocol[0] + '-source',
    input: protocol_root / protocol[1],
    output: protocol[0] + '-protocol.c',
    command: [scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    depends: header,
  )
  protocol_sources += source
  protocol_headers += header
endforeach

glib = dependency('glib-2.0', version: '>=2.76')
gio = dependency('gio-2.0', version: '>=2.76')
gio_unix = dependency('gio-unix-2.0', version: '>=2.76')
tomlplusplus = dependency('tomlplusplus', version: '>=3.4')
nlohmann_json = dependency('nlohmann_json')
pugixml = dependency('pugixml')
libvirt = dependency('libvirt', version: '>=9.10')
libosinfo = dependency('libosinfo-1.0')
egl = dependency('egl')
glesv2 = dependency('glesv2')

if cpp.get_define('_LIBCPP_VERSION', prefix: '#include <version>') != ''
  add_project_arguments('-fexperimental-library', language: 'cpp')
  add_project_link_arguments('-lc++experimental', language: 'cpp')
endif

if get_option('buildtype') == 'release'
  add_project_arguments('-ffunction-sections', '-fdata-sections', language: 'cpp')
  add_project_link_arguments('-Wl,--gc-sections', '-Wl,--as-needed', language: 'cpp')
else
  add_project_arguments('-D_GLIBCXX_ASSERTIONS', language: 'cpp')
endif

noctalia_ui_build_codecs = true
subdir('lib')

build_config = configuration_data()
build_config.set_quoted('COFFRET_VERSION', meson.project_version())
build_config.set_quoted('COFFRET_BUILD_REVISION', get_option('build_revision'))
build_config.set_quoted('COFFRET_INSTALL_ASSETSDIR', get_option('prefix') / get_option('datadir') / 'coffret/assets')
build_info = configure_file(
  input: 'src/app/build_info.h.in',
  output: 'coffret_build_info.h',
  configuration: build_config,
)
src_inc = include_directories('src')

# What needs neither the toolkit nor a hypervisor: the tests link only this.
core_lib = static_library('coffret-core',
  files(
    'src/display/frame_store.cpp',
    'src/fs/xdg_dirs.cpp',
    'src/input/linux_to_qnum.c',
    'src/snapshot/snapshot_tree.cpp',
    'src/storage/disk_image.cpp',
    'src/vm/clone_plan.cpp',
    'src/vm/domain_xml.cpp',
  ),
  include_directories: src_inc,
  dependencies: [glib, pugixml, nlohmann_json],
)
core_dep = declare_dependency(link_with: core_lib, include_directories: src_inc, dependencies: [glib, pugixml])

subdir('src')

tests_opt = get_option('tests')
if tests_opt.enabled() or (tests_opt.auto() and get_option('buildtype') == 'debug')
  subdir('tests')
endif

install_subdir(noctalia_ui_assets_dir / 'fonts', install_dir: get_option('datadir') / 'coffret/assets')
install_data('assets/templates/colors.toml', install_dir: get_option('datadir') / 'coffret/templates')
install_data('LICENSE', install_dir: get_option('datadir') / 'licenses/coffret')
