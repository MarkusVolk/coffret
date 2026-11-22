#include "osinfo/install_media.h"

#include <mutex>

// libosinfo's headers do not declare themselves as C.
extern "C" {
#include <osinfo/osinfo.h>
}

namespace {
  OsinfoDb* database() {
    static OsinfoDb* db = nullptr;
    static std::once_flag once;
    std::call_once(once, [] {
      OsinfoLoader* loader = osinfo_loader_new();
      GError* error = nullptr;
      osinfo_loader_process_default_path(loader, &error);
      if (error) {
        g_error_free(error);
      } else {
        db = OSINFO_DB(g_object_ref(osinfo_loader_get_db(loader)));
      }
      g_object_unref(loader);
    });
    return db;
  }
} // namespace

std::optional<coffret::InstallMedia> coffret::detectInstallMedia(const std::string& isoPath) {
  static std::mutex mutex;
  const std::lock_guard lock(mutex);
  OsinfoDb* db = database();
  if (!db) {
    return std::nullopt;
  }
  GError* error = nullptr;
  OsinfoMedia* media = osinfo_media_create_from_location(isoPath.c_str(), nullptr, &error);
  if (!media) {
    g_clear_error(&error);
    return std::nullopt;
  }
  std::optional<InstallMedia> result;
  OsinfoOs* os = nullptr;
  if (osinfo_db_identify_media(db, media)) {
    g_object_get(media, "os", &os, nullptr);
  }
  if (os) {
    InstallMedia found;
    const char* id = osinfo_entity_get_id(OSINFO_ENTITY(os));
    const char* name = osinfo_product_get_name(OSINFO_PRODUCT(os));
    const char* family = osinfo_os_get_family(os);
    found.id = id ? id : "";
    found.name = name ? name : "";
    const std::string_view kind = family ? family : "";
    found.kind = kind == "linux" ? GuestKind::Linux : kind == "winnt" ? GuestKind::Windows : GuestKind::Other;
    OsinfoResourcesList* list = osinfo_os_get_recommended_resources(os);
    const int count = list ? osinfo_list_get_length(OSINFO_LIST(list)) : 0;
    for (int index = 0; index < count; ++index) {
      auto* resources = OSINFO_RESOURCES(osinfo_list_get_nth(OSINFO_LIST(list), index));
      const char* architecture = osinfo_resources_get_architecture(resources);
      const std::string_view arch = architecture ? architecture : "";
      if (arch != "x86_64" && arch != "all") {
        continue;
      }
      if (const gint64 ram = osinfo_resources_get_ram(resources); ram > 0) {
        found.memoryMiB = static_cast<unsigned>(ram >> 20);
      }
      if (const gint64 storage = osinfo_resources_get_storage(resources); storage > 0) {
        found.diskBytes = static_cast<std::uint64_t>(storage);
      }
      if (const int cpus = osinfo_resources_get_n_cpus(resources); cpus > 0) {
        found.cpus = static_cast<unsigned>(cpus);
      }
    }
    if (list) {
      g_object_unref(list);
    }
    g_object_unref(os);
    result = std::move(found);
  }
  g_object_unref(media);
  return result;
}
