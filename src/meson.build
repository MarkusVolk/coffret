app_sources = files(
  'app/application.cpp',
  'app/console_window.cpp',
  'app/instance_service.cpp',
  'app/main_loop.cpp',
  'app/palette_watcher.cpp',
  'display/console_node.cpp',
  'display/qemu_display.cpp',
  'event_loop/glib_poll_source.cpp',
  'fs/directory_watch.cpp',
  'input/input_grab.cpp',
  'input/raw_keyboard.cpp',
  'main.cpp',
  'platform/desktop_integration.cpp',
  'platform/launch_activation.cpp',
  'qemu/qmp_attach.cpp',
  'ui/palette_file.cpp',
)

executable('coffret',
  app_sources + protocol_sources + protocol_headers + [build_info],
  include_directories: src_inc,
  dependencies: [
    core_dep, noctalia_ui_dep, noctalia_ui_codecs_dep, gio, gio_unix, tomlplusplus, nlohmann_json, egl, glesv2,
    dependency('wayland-client'), dependency('wayland-cursor'), dependency('threads'),
  ],
  install: true,
)
