#include "display/qemu_display.h"

#include "core/log.h"

#include <atomic>
#include <cstring>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <mutex>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace {
  const Logger kLog("display");

  constexpr const char* kConsolePath = "/org/qemu/Display1/Console_0";
  constexpr const char* kListenerPath = "/org/qemu/Display1/Listener";
  constexpr const char* kClipboardPath = "/org/qemu/Display1/Clipboard";
  constexpr const char* kClipboardInterface = "org.qemu.Display1.Clipboard";
  constexpr const char* kTextMime = "text/plain;charset=utf-8";
  // Both ends of the display bus have this interface; selection 0 is the clipboard.
  constexpr const char* kClipboardXml = R"(<node>
  <interface name="org.qemu.Display1.Clipboard">
    <method name="Register"/>
    <method name="Unregister"/>
    <method name="Grab">
      <arg type="u" name="selection" direction="in"/><arg type="u" name="serial" direction="in"/>
      <arg type="as" name="mimes" direction="in"/>
    </method>
    <method name="Release"><arg type="u" name="selection" direction="in"/></method>
    <method name="Request">
      <arg type="u" name="selection" direction="in"/><arg type="as" name="mimes" direction="in"/>
      <arg type="s" name="reply_mime" direction="out"/><arg type="ay" name="data" direction="out"/>
    </method>
  </interface>
</node>)";
  constexpr const char* kMapInterface = "org.qemu.Display1.Listener.Unix.Map";
  constexpr const char* kDmabuf2Interface = "org.qemu.Display1.Listener.Unix.ScanoutDMABUF2";

  // What this listener implements of QEMU's ui/dbus-display1.xml (QEMU 11.1).
  constexpr const char* kListenerXml = R"(<node>
  <interface name="org.qemu.Display1.Listener">
    <method name="Scanout">
      <arg type="u" name="width" direction="in"/><arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <method name="Update">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <method name="ScanoutDMABUF">
      <arg type="h" name="dmabuf" direction="in"/><arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/><arg type="u" name="stride" direction="in"/>
      <arg type="u" name="fourcc" direction="in"/><arg type="t" name="modifier" direction="in"/>
      <arg type="b" name="y0_top" direction="in"/>
    </method>
    <method name="UpdateDMABUF">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
    </method>
    <method name="Disable"/>
    <method name="MouseSet">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/><arg type="i" name="on" direction="in"/>
    </method>
    <method name="CursorDefine">
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
      <arg type="i" name="hot_x" direction="in"/><arg type="i" name="hot_y" direction="in"/>
      <arg type="ay" name="data" direction="in"/>
    </method>
    <property name="Interfaces" type="as" access="read"/>
  </interface>
  <interface name="org.qemu.Display1.Listener.Unix.Map">
    <method name="ScanoutMap">
      <arg type="h" name="handle" direction="in"/><arg type="u" name="offset" direction="in"/>
      <arg type="u" name="width" direction="in"/><arg type="u" name="height" direction="in"/>
      <arg type="u" name="stride" direction="in"/><arg type="u" name="pixman_format" direction="in"/>
    </method>
    <method name="UpdateMap">
      <arg type="i" name="x" direction="in"/><arg type="i" name="y" direction="in"/>
      <arg type="i" name="width" direction="in"/><arg type="i" name="height" direction="in"/>
    </method>
  </interface>
  <interface name="org.qemu.Display1.Listener.Unix.ScanoutDMABUF2">
    <method name="ScanoutDMABUF2">
      <arg type="ah" name="dmabuf" direction="in"/><arg type="u" name="x" direction="in"/>
      <arg type="u" name="y" direction="in"/><arg type="u" name="width" direction="in"/>
      <arg type="u" name="height" direction="in"/><arg type="au" name="offset" direction="in"/>
      <arg type="au" name="stride" direction="in"/><arg type="u" name="num_planes" direction="in"/>
      <arg type="u" name="fourcc" direction="in"/><arg type="u" name="backing_width" direction="in"/>
      <arg type="u" name="backing_height" direction="in"/><arg type="t" name="modifier" direction="in"/>
      <arg type="b" name="y0_top" direction="in"/>
    </method>
  </interface>
</node>)";

  // Closes the descriptors of a dmabuf scanout when its last user lets go.
  struct FdSet {
    std::vector<int> fds;
    ~FdSet() {
      for (const int fd : fds) {
        if (fd >= 0) {
          ::close(fd);
        }
      }
    }
  };

  struct Mapping {
    void* address = MAP_FAILED;
    std::size_t length = 0;
    ~Mapping() {
      if (address != MAP_FAILED) {
        ::munmap(address, length);
      }
    }
  };

  GDBusConnection* connectionFor(int fd, const char* guid, GDBusConnectionFlags flags, GError** error) {
    GSocket* socket = g_socket_new_from_fd(fd, error);
    if (!socket) {
      ::close(fd);
      return nullptr;
    }
    GSocketConnection* stream = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);
    GDBusConnection* connection = g_dbus_connection_new_sync(G_IO_STREAM(stream), guid, flags, nullptr, nullptr, error);
    g_object_unref(stream);
    return connection;
  }
} // namespace

struct coffret::QemuDisplay::Impl {
  std::shared_ptr<FrameStore> frames;
  Callbacks callbacks;
  int busFd = -1;

  std::thread thread;
  // The display thread's own context, made and freed by the owner so a wake-up never meets a dead
  // one; the thread iterates it until `stopping`.
  GMainContext* context = nullptr;
  // Set once on the display thread, read by the input calls of the main thread.
  std::atomic<GDBusConnection*> bus{nullptr};
  GDBusConnection* listener = nullptr;
  GDBusNodeInfo* node = nullptr;
  std::atomic<bool> absolute{true};
  std::atomic<bool> stopping{false};
  // Tells callbacks queued for the main thread that the display is gone.
  std::shared_ptr<int> alive = std::make_shared<int>(0);

  // The clipboard: what the host offers, and the count of grabs that both ends share.
  GDBusNodeInfo* clipboardNode = nullptr;
  std::mutex clipboardMutex;
  std::string hostText;
  std::atomic<std::uint32_t> clipboardSerial{0};
  std::atomic<bool> clipboard{false};

  // The copy of the picture for the plain Scanout/Update calls, which carry bytes.
  std::shared_ptr<std::vector<std::uint8_t>> shadow;
  std::uint32_t shadowWidth = 0, shadowHeight = 0, shadowStride = 0, shadowFormat = 0;
  // The size of the picture last reported to the main thread; 0x0 for none.
  std::uint32_t shownWidth = 0, shownHeight = 0;

  void run();
  bool connect(std::string& why);
  void postToMain(std::function<void()> call);
  void fail(const std::string& why);
  void readPointerMode();
  void call(const char* interface, const char* method, GVariant* parameters);

  void registerClipboard();
  void grab();
  void clipboardCall(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation);
  static void onClipboardCall(
      GDBusConnection*, const char*, const char*, const char*, const char* method, GVariant* parameters,
      GDBusMethodInvocation* invocation, gpointer data
  ) {
    static_cast<Impl*>(data)->clipboardCall(method, parameters, invocation);
  }
  void methodCall(const char* interface, const char* method, GVariant* parameters, GDBusMethodInvocation* invocation);
  static void onMethodCall(
      GDBusConnection*, const char*, const char*, const char* interface, const char* method, GVariant* parameters,
      GDBusMethodInvocation* invocation, gpointer data
  ) {
    static_cast<Impl*>(data)->methodCall(interface, method, parameters, invocation);
  }
  static GVariant* onGetProperty(GDBusConnection*, const char*, const char*, const char*, const char* property, GError**, gpointer) {
    if (std::strcmp(property, "Interfaces") == 0) {
      const char* const interfaces[] = {kMapInterface, kDmabuf2Interface, nullptr};
      return g_variant_new_strv(interfaces, -1);
    }
    return nullptr;
  }
  static void onPropertiesChanged(
      GDBusConnection*, const char*, const char*, const char*, const char*, GVariant* parameters, gpointer data
  ) {
    auto* self = static_cast<Impl*>(data);
    const char* interface = nullptr;
    GVariant* changed = nullptr;
    g_variant_get(parameters, "(&s@a{sv}@as)", &interface, &changed, nullptr);
    if (std::strcmp(interface, "org.qemu.Display1.Mouse") == 0) {
      gboolean value = FALSE;
      if (g_variant_lookup(changed, "IsAbsolute", "b", &value)) {
        self->absolute = value != FALSE;
        self->postToMain([self] {
          if (self->callbacks.pointerMode) {
            self->callbacks.pointerMode(self->absolute);
          }
        });
      }
    }
    g_variant_unref(changed);
  }
  static void onClosed(GDBusConnection*, gboolean, GError* error, gpointer data) {
    auto* self = static_cast<Impl*>(data);
    if (!self->stopping) {
      self->fail(error ? error->message : "QEMU closed the display connection");
    }
  }
};

void coffret::QemuDisplay::Impl::postToMain(std::function<void()> call) {
  struct Posted {
    std::weak_ptr<int> alive;
    std::function<void()> call;
  };
  g_main_context_invoke_full(
      g_main_context_default(), G_PRIORITY_DEFAULT,
      [](gpointer data) -> gboolean {
        auto* posted = static_cast<Posted*>(data);
        if (!posted->alive.expired()) {
          posted->call();
        }
        return G_SOURCE_REMOVE;
      },
      new Posted{alive, std::move(call)}, [](gpointer data) { delete static_cast<Posted*>(data); }
  );
}

void coffret::QemuDisplay::Impl::fail(const std::string& why) {
  kLog.warn("{}", why);
  frames->disable();
  postToMain([this, why] {
    if (callbacks.closed) {
      callbacks.closed(why);
    }
  });
}

void coffret::QemuDisplay::Impl::readPointerMode() {
  GVariant* reply = g_dbus_connection_call_sync(
      bus, nullptr, kConsolePath, "org.freedesktop.DBus.Properties", "Get",
      g_variant_new("(ss)", "org.qemu.Display1.Mouse", "IsAbsolute"), G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000,
      nullptr, nullptr
  );
  if (!reply) {
    return;
  }
  GVariant* value = nullptr;
  g_variant_get(reply, "(v)", &value);
  absolute = g_variant_get_boolean(value) != FALSE;
  g_variant_unref(value);
  g_variant_unref(reply);
}

bool coffret::QemuDisplay::Impl::connect(std::string& why) {
  GError* error = nullptr;
  GDBusConnection* connection = connectionFor(std::exchange(busFd, -1), nullptr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT, &error);
  if (!connection) {
    why = std::string("cannot reach QEMU's display: ") + error->message;
    g_error_free(error);
    return false;
  }
  bus = connection;
  g_signal_connect(connection, "closed", G_CALLBACK(onClosed), this);
  g_dbus_connection_signal_subscribe(
      connection, nullptr, "org.freedesktop.DBus.Properties", "PropertiesChanged", kConsolePath, nullptr,
      G_DBUS_SIGNAL_FLAGS_NONE, onPropertiesChanged, this, nullptr
  );
  readPointerMode();

  // QEMU talks to the listener over a connection of its own: one end of a socket pair goes to
  // QEMU, which is the D-Bus server on it although this side has the objects.
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
    why = "cannot create a socket pair";
    return false;
  }
  GUnixFDList* fds = g_unix_fd_list_new();
  const int index = g_unix_fd_list_append(fds, pair[1], &error);
  ::close(pair[1]);
  if (index < 0) {
    why = error->message;
    g_error_free(error);
    g_object_unref(fds);
    ::close(pair[0]);
    return false;
  }
  // QEMU answers first and then waits for this side's handshake on the new connection: the call
  // must be on its way before the handshake blocks here.
  g_dbus_connection_call_with_unix_fd_list(
      connection, nullptr, kConsolePath, "org.qemu.Display1.Console", "RegisterListener", g_variant_new("(h)", index), nullptr,
      G_DBUS_CALL_FLAGS_NONE, -1, fds, nullptr, nullptr, nullptr
  );
  g_object_unref(fds);
  g_dbus_connection_flush_sync(connection, nullptr, nullptr);

  listener = connectionFor(
      pair[0], nullptr,
      static_cast<GDBusConnectionFlags>(
          G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_DELAY_MESSAGE_PROCESSING
      ),
      &error
  );
  if (!listener) {
    why = std::string("QEMU did not connect to the listener: ") + error->message;
    g_error_free(error);
    return false;
  }
  node = g_dbus_node_info_new_for_xml(kListenerXml, &error);
  if (!node) {
    why = error->message;
    g_error_free(error);
    return false;
  }
  static const GDBusInterfaceVTable vtable = {
      .method_call = onMethodCall, .get_property = onGetProperty, .set_property = nullptr, .padding = {}
  };
  for (GDBusInterfaceInfo** interface = node->interfaces; *interface; ++interface) {
    if (g_dbus_connection_register_object(listener, kListenerPath, *interface, &vtable, this, nullptr, &error) == 0) {
      why = error->message;
      g_error_free(error);
      return false;
    }
  }
  g_dbus_connection_start_message_processing(listener);
  registerClipboard();
  return true;
}

void coffret::QemuDisplay::Impl::registerClipboard() {
  clipboardNode = g_dbus_node_info_new_for_xml(kClipboardXml, nullptr);
  static const GDBusInterfaceVTable vtable = {.method_call = onClipboardCall, .get_property = nullptr, .set_property = nullptr, .padding = {}};
  if (!clipboardNode
      || g_dbus_connection_register_object(bus, kClipboardPath, clipboardNode->interfaces[0], &vtable, this, nullptr, nullptr) == 0) {
    return;
  }
  // QEMU looks at this side's object before it answers, so the answer cannot be waited for here.
  // The call fails for a machine without the clipboard channel: then there is nothing to share.
  g_dbus_connection_call(
      bus, nullptr, kClipboardPath, kClipboardInterface, "Register", nullptr, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer data) {
        auto* self = static_cast<Impl*>(data);
        GError* error = nullptr;
        GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
        if (!reply) {
          kLog.debug("no clipboard with this machine: {}", error->message);
          g_error_free(error);
          return;
        }
        g_variant_unref(reply);
        self->clipboard = true;
        // What the host copied before the guest was there.
        std::string text;
        {
          const std::lock_guard lock(self->clipboardMutex);
          text = self->hostText;
        }
        if (!text.empty()) {
          self->grab();
        }
      },
      this
  );
}

void coffret::QemuDisplay::Impl::grab() {
  const char* const mimes[] = {kTextMime, nullptr};
  g_dbus_connection_call(
      bus, nullptr, kClipboardPath, kClipboardInterface, "Grab", g_variant_new("(uu^as)", 0U, ++clipboardSerial, mimes), nullptr,
      G_DBUS_CALL_FLAGS_NONE, -1, nullptr,
      [](GObject* source, GAsyncResult* result, gpointer) {
        GError* error = nullptr;
        if (GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error)) {
          g_variant_unref(reply);
          kLog.debug("the host's clipboard is offered to the guest");
        } else {
          kLog.debug("the guest's side did not take the clipboard: {}", error->message);
          g_error_free(error);
        }
      },
      nullptr
  );
}

void coffret::QemuDisplay::Impl::clipboardCall(const char* method, GVariant* parameters, GDBusMethodInvocation* invocation) {
  const std::string_view name(method);
  if (name == "Grab") {
    guint32 selection = 0, serial = 0;
    GVariant* mimes = nullptr;
    g_variant_get(parameters, "(uu@as)", &selection, &serial, &mimes);
    gsize count = 0;
    const char** list = g_variant_get_strv(mimes, &count);
    bool text = false;
    for (gsize index = 0; index < count; ++index) {
      text = text || std::string_view(list[index]) == kTextMime;
    }
    g_free(list);
    g_variant_unref(mimes);
    g_dbus_method_invocation_return_value(invocation, nullptr);
    kLog.debug("the guest grabs selection {} (serial {}, text {})", selection, serial, text);
    if (selection != 0 || !text) {
      return;
    }
    clipboardSerial = serial;
    // The guest has copied something: fetch it right away, as text is small and the host's
    // clipboard then has it whether or not the guest is still there when it is pasted.
    const char* const wanted[] = {kTextMime, nullptr};
    g_dbus_connection_call(
        bus, nullptr, kClipboardPath, kClipboardInterface, "Request", g_variant_new("(u^as)", 0U, wanted), G_VARIANT_TYPE("(say)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
        [](GObject* source, GAsyncResult* result, gpointer data) {
          auto* self = static_cast<Impl*>(data);
          GError* error = nullptr;
          GVariant* reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error);
          if (!reply) {
            kLog.debug("the guest's clipboard cannot be read: {}", error ? error->message : "no reply");
            g_clear_error(&error);
            return;
          }
          const char* mime = nullptr;
          GVariant* bytes = nullptr;
          g_variant_get(reply, "(&s@ay)", &mime, &bytes);
          gsize size = 0;
          const auto* data8 = static_cast<const char*>(g_variant_get_fixed_array(bytes, &size, 1));
          std::string text(data8, size);
          g_variant_unref(bytes);
          g_variant_unref(reply);
          self->postToMain([self, text = std::move(text)] {
            if (self->callbacks.guestClipboard) {
              self->callbacks.guestClipboard(text);
            }
          });
        },
        this
    );
  } else if (name == "Request") {
    kLog.debug("the guest pastes the host's clipboard");
    guint32 selection = 0;
    g_variant_get(parameters, "(u@as)", &selection, nullptr);
    std::string text;
    {
      const std::lock_guard lock(clipboardMutex);
      text = hostText;
    }
    if (selection != 0) {
      g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_FAILED, "nothing in this selection");
      return;
    }
    g_dbus_method_invocation_return_value(
        invocation,
        g_variant_new("(s@ay)", kTextMime, g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, text.data(), text.size(), 1))
    );
  } else {
    // Register, Unregister, Release: nothing to do on this side.
    g_dbus_method_invocation_return_value(invocation, nullptr);
  }
}

void coffret::QemuDisplay::Impl::run() {
  g_main_context_push_thread_default(context);
  std::string why;
  if (connect(why)) {
    postToMain([this] {
      if (callbacks.ready) {
        callbacks.ready();
      }
      if (callbacks.pointerMode) {
        callbacks.pointerMode(absolute);
      }
    });
    // A wake-up sent before the iteration sleeps is not lost, so the destructor cannot miss it.
    while (!stopping) {
      g_main_context_iteration(context, TRUE);
    }
  } else {
    fail(why);
  }
  if (listener) {
    g_dbus_connection_close_sync(listener, nullptr, nullptr);
    g_object_unref(listener);
  }
  if (GDBusConnection* connection = bus.exchange(nullptr)) {
    g_dbus_connection_close_sync(connection, nullptr, nullptr);
    g_object_unref(connection);
  }
  if (node) {
    g_dbus_node_info_unref(node);
  }
  if (clipboardNode) {
    g_dbus_node_info_unref(clipboardNode);
  }
  if (busFd >= 0) {
    ::close(busFd);
  }
  g_main_context_pop_thread_default(context);
}

void coffret::QemuDisplay::Impl::methodCall(
    const char* interface, const char* method, GVariant* parameters, GDBusMethodInvocation* invocation
) {
  const auto done = [invocation] { g_dbus_method_invocation_return_value(invocation, nullptr); };
  const auto refuse = [invocation](const char* message) {
    g_dbus_method_invocation_return_error_literal(invocation, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, message);
  };
  GUnixFDList* fdList = g_dbus_message_get_unix_fd_list(g_dbus_method_invocation_get_message(invocation));
  const auto takeFd = [fdList](gint32 handle) { return fdList ? g_unix_fd_list_get(fdList, handle, nullptr) : -1; };
  const std::string_view name(method);
  const auto shown = [this](std::uint32_t width, std::uint32_t height) {
    if (width == shownWidth && height == shownHeight) {
      return;
    }
    shownWidth = width;
    shownHeight = height;
    postToMain([this, width, height] {
      if (callbacks.scanout) {
        callbacks.scanout(width, height);
      }
    });
  };

  if (name == "UpdateDMABUF" || name == "UpdateMap") {
    gint32 x = 0, y = 0, w = 0, h = 0;
    g_variant_get(parameters, "(iiii)", &x, &y, &w, &h);
    frames->damage({x, y, w, h});
    done();
  } else if (name == "ScanoutDMABUF") {
    gint32 handle = 0;
    guint32 width = 0, height = 0, stride = 0, fourcc = 0;
    guint64 modifier = 0;
    gboolean y0Top = FALSE;
    g_variant_get(parameters, "(huuuutb)", &handle, &width, &height, &stride, &fourcc, &modifier, &y0Top);
    const int fd = takeFd(handle);
    if (fd < 0) {
      refuse("no dmabuf descriptor");
      return;
    }
    auto keep = std::make_shared<FdSet>();
    keep->fds.push_back(fd);
    Scanout scanout;
    scanout.width = scanout.backingWidth = width;
    scanout.height = scanout.backingHeight = height;
    scanout.planes.push_back({fd, 0, stride});
    scanout.fourcc = fourcc;
    scanout.modifier = modifier;
    scanout.y0Top = y0Top != FALSE;
    scanout.keep = std::move(keep);
    frames->setDmabuf(std::move(scanout));
    shown(width, height);
    done();
  } else if (name == "ScanoutDMABUF2") {
    GVariant* handles = nullptr;
    GVariant* offsets = nullptr;
    GVariant* strides = nullptr;
    guint32 x = 0, y = 0, width = 0, height = 0, planes = 0, fourcc = 0, backingWidth = 0, backingHeight = 0;
    guint64 modifier = 0;
    gboolean y0Top = FALSE;
    g_variant_get(
        parameters, "(@ahuuuu@au@auuuuutb)", &handles, &x, &y, &width, &height, &offsets, &strides, &planes, &fourcc,
        &backingWidth, &backingHeight, &modifier, &y0Top
    );
    gsize handleCount = 0, offsetCount = 0, strideCount = 0;
    const auto* handleArray = static_cast<const gint32*>(g_variant_get_fixed_array(handles, &handleCount, sizeof(gint32)));
    const auto* offsetArray = static_cast<const guint32*>(g_variant_get_fixed_array(offsets, &offsetCount, sizeof(guint32)));
    const auto* strideArray = static_cast<const guint32*>(g_variant_get_fixed_array(strides, &strideCount, sizeof(guint32)));
    auto keep = std::make_shared<FdSet>();
    Scanout scanout;
    bool ok = planes > 0 && planes <= 4 && handleCount >= planes && offsetCount >= planes && strideCount >= planes;
    for (guint32 i = 0; ok && i < planes; ++i) {
      const int fd = takeFd(handleArray[i]);
      ok = fd >= 0;
      keep->fds.push_back(fd);
      scanout.planes.push_back({fd, offsetArray[i], strideArray[i]});
    }
    g_variant_unref(handles);
    g_variant_unref(offsets);
    g_variant_unref(strides);
    if (!ok) {
      refuse("bad dmabuf planes");
      return;
    }
    scanout.x = x;
    scanout.y = y;
    scanout.width = width;
    scanout.height = height;
    scanout.backingWidth = backingWidth;
    scanout.backingHeight = backingHeight;
    scanout.fourcc = fourcc;
    scanout.modifier = modifier;
    scanout.y0Top = y0Top != FALSE;
    scanout.keep = std::move(keep);
    frames->setDmabuf(std::move(scanout));
    shown(width, height);
    done();
  } else if (name == "ScanoutMap") {
    gint32 handle = 0;
    guint32 offset = 0, width = 0, height = 0, stride = 0, format = 0;
    g_variant_get(parameters, "(huuuuu)", &handle, &offset, &width, &height, &stride, &format);
    const int fd = takeFd(handle);
    auto mapping = std::make_shared<Mapping>();
    mapping->length = static_cast<std::size_t>(offset) + static_cast<std::size_t>(stride) * height;
    if (fd >= 0 && mapping->length > 0) {
      mapping->address = ::mmap(nullptr, mapping->length, PROT_READ, MAP_SHARED, fd, 0);
    }
    if (fd >= 0) {
      ::close(fd);
    }
    if (mapping->address == MAP_FAILED) {
      refuse("cannot map the shared picture");
      return;
    }
    const auto* pixels = static_cast<const std::uint8_t*>(mapping->address) + offset;
    frames->setPixels(pixels, width, height, stride, format, std::move(mapping));
    shown(width, height);
    done();
  } else if (name == "Scanout") {
    guint32 width = 0, height = 0, stride = 0, format = 0;
    GVariant* data = nullptr;
    g_variant_get(parameters, "(uuuu@ay)", &width, &height, &stride, &format, &data);
    gsize size = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(data, &size, 1));
    if (size < static_cast<gsize>(stride) * height) {
      g_variant_unref(data);
      refuse("short picture");
      return;
    }
    shadow = std::make_shared<std::vector<std::uint8_t>>(bytes, bytes + static_cast<gsize>(stride) * height);
    g_variant_unref(data);
    shadowWidth = width;
    shadowHeight = height;
    shadowStride = stride;
    shadowFormat = format;
    frames->setPixels(shadow->data(), width, height, stride, format, shadow);
    shown(width, height);
    done();
  } else if (name == "Update") {
    gint32 x = 0, y = 0, w = 0, h = 0;
    guint32 stride = 0, format = 0;
    GVariant* data = nullptr;
    g_variant_get(parameters, "(iiiiuu@ay)", &x, &y, &w, &h, &stride, &format, &data);
    gsize size = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(data, &size, 1));
    const bool fits = shadow && format == shadowFormat && x >= 0 && y >= 0 && w > 0 && h > 0
                   && static_cast<guint32>(x + w) <= shadowWidth && static_cast<guint32>(y + h) <= shadowHeight
                   && size >= static_cast<gsize>(stride) * (h - 1) + static_cast<gsize>(w) * 4;
    if (fits) {
      for (gint32 row = 0; row < h; ++row) {
        std::memcpy(
            shadow->data() + static_cast<gsize>(y + row) * shadowStride + static_cast<gsize>(x) * 4,
            bytes + static_cast<gsize>(row) * stride, static_cast<gsize>(w) * 4
        );
      }
      frames->damage({x, y, w, h});
    }
    g_variant_unref(data);
    done();
  } else if (name == "Disable") {
    shadow.reset();
    frames->disable();
    shown(0, 0);
    done();
  } else if (name == "MouseSet") {
    gint32 x = 0, y = 0, on = 0;
    g_variant_get(parameters, "(iii)", &x, &y, &on);
    frames->moveCursor(x, y, on != 0);
    done();
  } else if (name == "CursorDefine") {
    gint32 width = 0, height = 0, hotX = 0, hotY = 0;
    GVariant* data = nullptr;
    g_variant_get(parameters, "(iiii@ay)", &width, &height, &hotX, &hotY, &data);
    gsize size = 0;
    const auto* bytes = static_cast<const std::uint8_t*>(g_variant_get_fixed_array(data, &size, 1));
    if (width > 0 && height > 0 && width <= 512 && height <= 512 && size >= static_cast<gsize>(width) * height * 4) {
      frames->setCursor(width, height, hotX, hotY, std::vector<std::uint8_t>(bytes, bytes + static_cast<gsize>(width) * height * 4));
    }
    g_variant_unref(data);
    done();
  } else {
    g_dbus_method_invocation_return_error(
        invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "%s.%s is not implemented", interface, method
    );
  }
}

void coffret::QemuDisplay::Impl::call(const char* interface, const char* method, GVariant* parameters) {
  GDBusConnection* connection = bus;
  if (!connection) {
    g_variant_unref(g_variant_ref_sink(parameters));
    return;
  }
  // No reply is wanted: a key that QEMU refuses is nothing the user can act on.
  g_dbus_connection_call(
      connection, nullptr, kConsolePath, interface, method, parameters, nullptr, G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr,
      nullptr
  );
}

coffret::QemuDisplay::QemuDisplay(int busFd, std::shared_ptr<FrameStore> frames, Callbacks callbacks)
    : m_impl(std::make_unique<Impl>()) {
  m_impl->frames = std::move(frames);
  m_impl->callbacks = std::move(callbacks);
  m_impl->busFd = busFd;
  m_impl->context = g_main_context_new();
  m_impl->thread = std::thread([impl = m_impl.get()] { impl->run(); });
}

coffret::QemuDisplay::~QemuDisplay() {
  m_impl->alive.reset();
  m_impl->stopping = true;
  g_main_context_wakeup(m_impl->context);
  if (m_impl->thread.joinable()) {
    m_impl->thread.join();
  }
  g_main_context_unref(m_impl->context);
}

void coffret::QemuDisplay::offerClipboard(std::string text) {
  if (!m_impl->bus) {
    return;
  }
  {
    const std::lock_guard lock(m_impl->clipboardMutex);
    if (m_impl->hostText == text) {
      return;
    }
    m_impl->hostText = std::move(text);
  }
  // Before the clipboard is registered the text waits for that.
  if (m_impl->clipboard) {
    m_impl->grab();
  }
}

void coffret::QemuDisplay::keyPress(std::uint32_t qnum) {
  m_impl->call("org.qemu.Display1.Keyboard", "Press", g_variant_new("(u)", qnum));
}

void coffret::QemuDisplay::keyRelease(std::uint32_t qnum) {
  m_impl->call("org.qemu.Display1.Keyboard", "Release", g_variant_new("(u)", qnum));
}

void coffret::QemuDisplay::pointerPress(std::uint32_t button) {
  m_impl->call("org.qemu.Display1.Mouse", "Press", g_variant_new("(u)", button));
}

void coffret::QemuDisplay::pointerRelease(std::uint32_t button) {
  m_impl->call("org.qemu.Display1.Mouse", "Release", g_variant_new("(u)", button));
}

void coffret::QemuDisplay::pointerPosition(std::uint32_t x, std::uint32_t y) {
  m_impl->call("org.qemu.Display1.Mouse", "SetAbsPosition", g_variant_new("(uu)", x, y));
}

void coffret::QemuDisplay::pointerMotion(std::int32_t dx, std::int32_t dy) {
  m_impl->call("org.qemu.Display1.Mouse", "RelMotion", g_variant_new("(ii)", dx, dy));
}

void coffret::QemuDisplay::requestSize(std::uint32_t width, std::uint32_t height, std::uint16_t widthMm, std::uint16_t heightMm) {
  m_impl->call(
      "org.qemu.Display1.Console", "SetUIInfo", g_variant_new("(qqiiuu)", widthMm, heightMm, 0, 0, width, height)
  );
}

bool coffret::QemuDisplay::pointerAbsolute() const noexcept { return m_impl->absolute; }
