# Credits

Coffret is built on the UI library of [Noctalia](https://github.com/noctalia-dev/noctalia) and
borrows application plumbing from [Luciole](https://github.com/noctalia-dev/luciole) by way of
[Plume](https://github.com/MarkusVolk/plume) and [Etal](https://github.com/MarkusVolk/etal).
Noctalia and Luciole are MIT licensed, Copyright (c) 2026 noctalia-dev.

- `lib/` is an unchanged copy of Noctalia's `lib/` with `third_party/wuffs/` for the codec pack,
  refreshed with `tools/sync-lib.sh`. Changes to it belong upstream.
- From Luciole, adapted: `src/app/main_loop.*`, `src/app/palette_watcher.*`, `src/event_loop/`,
  `src/fs/directory_watch.*`, `src/fs/gio_ptr.h`, `src/fs/xdg_dirs.*`,
  `src/platform/desktop_integration.*`, `src/platform/launch_activation.*`, `src/ui/palette_file.*`.
- `src/input/linux_to_qnum.c` is generated from
  [keycodemapdb](https://gitlab.com/keycodemap/keycodemapdb), used under its BSD license
  (`third_party/keycodemapdb/LICENSE.BSD`).
- The display code implements QEMU's `org.qemu.Display1` D-Bus interface as documented in QEMU's
  `ui/dbus-display1.xml`. [libmks](https://gitlab.gnome.org/GNOME/libmks) and
  [qemu-display](https://gitlab.com/marcandre.lureau/qemu-display) showed how such a client is
  put together; no code is taken from either.
- `lib/assets/fonts/` carries the Tabler icon font; its license is
  `lib/assets/fonts/tabler-icons-license.txt`. Wuffs is under `lib/third_party/wuffs/`, Apache-2.0
  or MIT.
