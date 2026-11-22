# Coffret

Virtual machines for the Noctalia desktop: a replacement for GNOME Boxes that aims at the comfort
of VMware Workstation, on QEMU/KVM and libvirt. It draws with Noctalia's UI library, without GTK or
Qt, and follows the Noctalia colour scheme.

Coffret is at its very beginning: for now it is the console of one QEMU that was started by hand.
See [CREDITS.md](CREDITS.md) for what it builds on.

## Build

```sh
meson setup build --buildtype=release
meson compile -C build
```

Dependencies: a C++23 compiler, Meson, wayland-scanner, wayland-protocols, wayland-client,
wayland-cursor, wayland-egl, EGL and GLES, GLib/GIO, Pango, Cairo, HarfBuzz, FreeType, fontconfig,
xkbcommon, tomlplusplus, nlohmann_json, the stb headers, and for the images librsvg, libwebp and
libjxl.

## The console of a QEMU started by hand

```sh
qemu-system-x86_64 -enable-kvm -m 4G -display dbus,p2p=yes,gl=on \
  -device virtio-vga-gl -device usb-ehci -device usb-tablet \
  -qmp unix:/tmp/guest.qmp,server=on,wait=off disk.qcow2
COFFRET_ASSETS_DIR=$PWD/lib/assets ./build/src/coffret --qmp /tmp/guest.qmp
```

The guest's picture is taken over without a copy when it renders on the GPU (`gl=on`), and from
shared memory otherwise. Keys reach the guest as the keyboard sends them; the guest follows the
size of the window.

## Actions over D-Bus

A running Coffret takes named actions on `org.freedesktop.Application`; `coffret --action NAME
[PARAMETER]` sends one: `fullscreen`, `state` (prints size, display path and frame rate),
`key <Linux key code>`, `size <width>x<height>`.
