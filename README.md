# Coffret

Virtual machines for the Noctalia desktop: a replacement for GNOME Boxes that aims at the comfort
of VMware Workstation, on QEMU/KVM and libvirt. It draws with Noctalia's UI library, without GTK or
Qt, and follows the Noctalia colour scheme.

Coffret is young. What is there: the library of the machines of the user's own libvirt
(`qemu:///session`), a tab per machine with its summary and its console, power controls, and
machines made from an existing disk image. See [CREDITS.md](CREDITS.md) for what it builds on.

## Build

```sh
meson setup build --buildtype=release
meson compile -C build
```

Dependencies: a C++23 compiler, Meson, wayland-scanner, wayland-protocols, wayland-client,
wayland-cursor, wayland-egl, EGL and GLES, GLib/GIO, Pango, Cairo, HarfBuzz, FreeType, fontconfig,
xkbcommon, tomlplusplus, nlohmann_json, pugixml, libvirt, the stb headers, and for the images
librsvg, libwebp and libjxl.

At run time: libvirt with its QEMU driver, QEMU 9.2 or newer with the D-Bus display, and qemu-img;
passt for the network, swtpm for a TPM, virtiofsd for shared folders.

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
[PARAMETER]` sends one. A machine is named by its name or uuid; without one the shown tab is meant.

| Action | Parameter | |
| --- | --- | --- |
| `open`, `home`, `close-tab` | machine | tabs |
| `start`, `shutdown`, `force-off`, `reset`, `pause`, `resume`, `suspend` | machine | power |
| `remove` | machine | forgets the machine; its disks stay |
| `import` | `image[|name[|cdrom]]` | a machine from a disk image, as a copy |
| `define` | file | a libvirt domain from its XML |
| `search` | text | filters the library |
| `fullscreen`, `grab`, `release` | | the console |
| `key` | Linux key codes, `29+56+111` | pressed in this order, released in reverse |
| `size` | `1280x800` | asks the guest for a resolution |
| `state` | | prints connection, machines, tabs and the console's display path and frame rate |
