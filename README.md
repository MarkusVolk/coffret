# Coffret

Virtual machines for the Noctalia desktop: a replacement for GNOME Boxes that aims at the comfort
of VMware Workstation, on QEMU/KVM and libvirt. It draws with Noctalia's UI library, without GTK or
Qt, and follows the Noctalia colour scheme.

What is there: the library of the machines of the user's own libvirt (`qemu:///session`); a tab
per machine with its console or its page; power controls; new machines from an installation medium
(recognised through libosinfo, and installed without anybody at it where libosinfo can answer the
installer) or from a disk image such as a cloud image or a Yocto `.wic`;
hardware settings, shared folders, port forwards; snapshots as a tree; full and linked clones;
the clipboard; USB devices of the host. See [CREDITS.md](CREDITS.md) for what it builds on.

Things to know:

- With 3D acceleration QEMU cannot save a running machine, so such a machine is neither suspended
  nor snapshotted while it runs; snapshots are taken while it is off.
- Clipboard and the guest following the window's size need `spice-vdagent` in the guest. With
  GNOME on Wayland in the guest, text goes from the guest to the host only.
- An unattended installation gives the guest the keyboard layout, language and time zone of this
  computer, a user and a password. coffret boots the installer's kernel with libosinfo's answers
  in its initrd and boots the disk once the installer is done; for now this works for media whose
  answers go into the initrd, such as Debian's.
- A shared folder is mounted in the guest with `mount -t virtiofs <name> /mnt`. Whoever writes
  in the guest, the files belong to the user on the host; in the guest they belong to its first
  user (id 1000).
- A USB device is given to a machine through `pkexec` and `coffret-usb-acl`, which lets the user
  open that one device; the polkit action asks nobody in the active session. It needs `setfacl`.

## Build

```sh
meson setup build --buildtype=release
meson compile -C build
```

Dependencies: a C++23 compiler, Meson, wayland-scanner, wayland-protocols, wayland-client,
wayland-cursor, wayland-egl, EGL and GLES, GLib/GIO, Pango, Cairo, HarfBuzz, FreeType, fontconfig,
xkbcommon, tomlplusplus, nlohmann_json, pugixml, libvirt, libosinfo, the stb headers, and for the images
librsvg, libwebp and libjxl.

At run time: libvirt with its QEMU driver, QEMU 9.2 or newer with the D-Bus display, and qemu-img;
passt for the network, swtpm for a TPM, virtiofsd for shared folders, xorriso for unattended
installations.

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
| `open`, `home`, `close-tab`, `details` | machine | tabs; `details` shows the page of a running machine |
| `start`, `shutdown`, `force-off`, `reset`, `pause`, `resume`, `suspend` | machine | power |
| `new` | file | the page for a new machine, with an ISO or a disk image |
| `unattended` | `on`, `off` | installs the machine of that page without anybody at it, or not |
| `user`, `password` | text | the user an unattended installation makes |
| `create`, `create-in-place` | | makes the machine of that page; in place: a disk image is not copied |
| `finish-install` | machine | boots a machine from its disk whose unattended installation coffret did not see end |
| `import` | `image[|name[|cdrom]]` | a machine from a disk image, as a copy |
| `define` | file | a libvirt domain from its XML |
| `set` | `memory=4096`, `cpus=4`, `3d=on`, `sound=off`, `microphone=on`, `forward=tcp:2222:22`, `share=/path:name` | one setting of the shown machine |
| `snapshot`, `revert`, `delete-snapshot` | name | snapshots of the shown machine |
| `clone`, `clone-linked` | name | a copy of the shown machine |
| `delete`, `forget` | machine | removes it with its own folder, or only out of the library |
| `usb`, `usb-off` | `vendor:product` | gives a USB device to the shown machine or takes it back |
| `clipboard` | text | as if the host had copied the text |
| `search` | text | filters the library |
| `fullscreen`, `grab`, `release` | | the console |
| `key` | Linux key codes, `29+56+111` | pressed in this order, released in reverse |
| `click` | `x,y` | a click at a place of the guest's screen |
| `size` | `1280x800` | asks the guest for a resolution |
| `shot` | file | the guest's picture as a PPM file |
| `state` | | prints connection, machines, tabs and the console's display path and frame rate |
