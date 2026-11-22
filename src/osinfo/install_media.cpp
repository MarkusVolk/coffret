#include "osinfo/install_media.h"

#include "osinfo/answer_file.h"
#include "osinfo/windows_answers.h"
#include "storage/process.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <fstream>
#include <gio/gio.h>
#include <mutex>
#include <pwd.h>
#include <unistd.h>

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
  std::mutex& databaseMutex() {
    static std::mutex mutex;
    return mutex;
  }

  // The installer's answer file that can go into the initrd, the desktop flavour before the
  // minimal one; null for a medium none fits. Holds a reference.
  OsinfoInstallScript* initrdScript(OsinfoMedia* media, OsinfoOs* os) {
    if (!osinfo_media_get_kernel_path(media) || !osinfo_media_get_initrd_path(media)) {
      return nullptr;
    }
    OsinfoInstallScriptList* list = osinfo_media_get_install_script_list(media);
    if (!list || osinfo_list_get_length(OSINFO_LIST(list)) == 0) {
      g_clear_object(&list);
      list = osinfo_os_get_install_script_list(os);
    }
    OsinfoInstallScript* found = nullptr;
    const int count = list ? osinfo_list_get_length(OSINFO_LIST(list)) : 0;
    for (const char* profile : {"desktop", "jeos"}) {
      for (int index = 0; index < count && !found; ++index) {
        auto* script = OSINFO_INSTALL_SCRIPT(osinfo_list_get_nth(OSINFO_LIST(list), index));
        const char* own = osinfo_install_script_get_profile(script);
        if (own && std::string_view(own) == profile
            && (osinfo_install_script_get_injection_methods(script) & OSINFO_INSTALL_SCRIPT_INJECTION_METHOD_INITRD)) {
          found = OSINFO_INSTALL_SCRIPT(g_object_ref(script));
        }
      }
    }
    g_clear_object(&list);
    return found;
  }

  // Windows 10 or 11 for amd64: the edition coffret installs and the language of the image, e.g.
  // "en-GB"; nullopt for another system.
  struct WindowsMedium {
    std::string edition;
    std::string language;
    // Volume licensing media (CCSA_...) take other keys than retail ones (CCCOMA_...).
    bool volume = false;
  };
  std::optional<WindowsMedium> windowsMedium(OsinfoMedia* media, OsinfoOs* os) {
    const char* shortId = osinfo_product_get_short_id(OSINFO_PRODUCT(os));
    const char* arch = osinfo_media_get_architecture(media);
    const std::string_view id = shortId ? shortId : "";
    if ((id != "win11" && id != "win10") || (arch && std::string_view(arch) != "x86_64")) {
      return std::nullopt;
    }
    const char* volumeId = osinfo_media_get_volume_id(media);
    WindowsMedium found{
        .edition = id == "win11" ? "Windows 11 Pro" : "Windows 10 Pro",
        .language = "en-US",
        .volume = volumeId && std::string_view(volumeId).find("CCSA") != std::string_view::npos,
    };
    GList* languages = osinfo_media_get_languages(media);
    if (languages && languages->data) {
      found.language = static_cast<const char*>(languages->data);
      std::replace(found.language.begin(), found.language.end(), '_', '-');
    }
    g_list_free(languages);
    return found;
  }

  // The medium as libosinfo knows it, with its system; both null when it does not. Holds
  // references.
  std::pair<OsinfoMedia*, OsinfoOs*> identify(OsinfoDb* db, const std::string& isoPath) {
    GError* error = nullptr;
    OsinfoMedia* media = osinfo_media_create_from_location(isoPath.c_str(), nullptr, &error);
    if (!media) {
      g_clear_error(&error);
      return {nullptr, nullptr};
    }
    OsinfoOs* os = nullptr;
    if (osinfo_db_identify_media(db, media)) {
      g_object_get(media, "os", &os, nullptr);
    }
    if (!os) {
      g_object_unref(media);
      return {nullptr, nullptr};
    }
    return {media, os};
  }

  std::string localeProperty(const char* name) {
    GDBusConnection* bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, nullptr);
    if (!bus) {
      return {};
    }
    GError* error = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(
        bus, "org.freedesktop.locale1", "/org/freedesktop/locale1",
        "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.freedesktop.locale1", name),
        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &error
    );
    g_object_unref(bus);
    if (!reply) {
      g_clear_error(&error);
      return {};
    }
    GVariant* value = nullptr;
    g_variant_get(reply, "(v)", &value);
    std::string text = g_variant_is_of_type(value, G_VARIANT_TYPE_STRING) ? g_variant_get_string(value, nullptr) : "";
    g_variant_unref(value);
    g_variant_unref(reply);
    return text;
  }
} // namespace

coffret::GuestSetup coffret::hostGuestSetup() {
  GuestSetup setup;
  if (const passwd* entry = ::getpwuid(::getuid())) {
    setup.user = entry->pw_name ? entry->pw_name : "";
    const std::string_view gecos = entry->pw_gecos ? entry->pw_gecos : "";
    setup.realName = std::string(gecos.substr(0, gecos.find(',')));
  }
  setup.keyboard = firstKeyboardLayout(localeProperty("X11Layout"), localeProperty("VConsoleKeymap"));
  const char* lang = std::getenv("LANG");
  setup.language = localeLanguage(lang ? lang : "");
  std::error_code error;
  const std::string zone = std::filesystem::read_symlink("/etc/localtime", error).string();
  const auto at = zone.find("zoneinfo/");
  setup.timezone = at == std::string::npos ? "UTC" : zone.substr(at + 9);
  return setup;
}

namespace {
  std::expected<coffret::UnattendedFiles, std::string>
  prepareWindows(const WindowsMedium& medium, const coffret::GuestSetup& setup, const std::string& folder) {
    namespace fs = std::filesystem;
    coffret::WindowsAnswers answers;
    answers.user = setup.user;
    answers.realName = setup.realName;
    answers.password = setup.password;
    answers.computerName = coffret::windowsComputerName(setup.hostname);
    answers.keyboard = setup.keyboard;
    answers.language = setup.language;
    answers.timezone = setup.timezone;
    answers.imageLanguage = medium.language;
    answers.edition = medium.edition;
    answers.productKey = coffret::windowsSetupKey(medium.edition, medium.volume);
    const fs::path directory = fs::path(folder) / "answers";
    std::error_code error;
    fs::create_directories(directory, error);
    std::ofstream(directory / "autounattend.xml") << coffret::windowsAnswerFile(answers);
    const fs::path disk = fs::path(folder) / "answers.iso";
    // Setup looks for autounattend.xml at the root of every drive.
    if (const auto made = coffret::runCommand(
            {"xorriso", "-as", "mkisofs", "-quiet", "-J", "-r", "-V", "OEMDRV", "-o", disk.string(), directory.string()}
        );
        !made) {
      return std::unexpected(std::format("The disc with the answers cannot be made: {}", made.error()));
    }
    fs::remove_all(directory, error);
    return coffret::UnattendedFiles{.boot = {}, .answerDisk = disk.string()};
  }
} // namespace

std::expected<coffret::UnattendedFiles, std::string>
coffret::prepareUnattended(const std::string& isoPath, const GuestSetup& setup, const std::string& folder) {
  namespace fs = std::filesystem;
  std::string answers, cmdline, filename, kernelPath, initrdPath;
  std::optional<WindowsMedium> windows;
  {
    const std::lock_guard lock(databaseMutex());
    OsinfoDb* db = database();
    if (!db) {
      return std::unexpected(std::string("The libosinfo database cannot be read."));
    }
    auto [media, os] = identify(db, isoPath);
    if (!media) {
      return std::unexpected(std::format("{} is no medium libosinfo knows.", isoPath));
    }
    windows = windowsMedium(media, os);
    OsinfoInstallScript* script = windows ? nullptr : initrdScript(media, os);
    if (!script) {
      g_object_unref(os);
      g_object_unref(media);
      if (windows) {
        return prepareWindows(*windows, setup, folder);
      }
      return std::unexpected(std::string("This medium cannot be installed without somebody at it."));
    }
    osinfo_install_script_set_preferred_injection_method(script, OSINFO_INSTALL_SCRIPT_INJECTION_METHOD_INITRD);
    OsinfoInstallConfig* config = osinfo_install_config_new("http://coffret/install-config");
    osinfo_install_config_set_l10n_keyboard(config, setup.keyboard.c_str());
    osinfo_install_config_set_l10n_language(config, setup.language.c_str());
    osinfo_install_config_set_l10n_timezone(config, setup.timezone.c_str());
    osinfo_install_config_set_user_login(config, setup.user.c_str());
    osinfo_install_config_set_user_realname(config, (setup.realName.empty() ? setup.user : setup.realName).c_str());
    osinfo_install_config_set_user_password(config, setup.password.c_str());
    // The installers want a root password too; the user's own is the one they know.
    osinfo_install_config_set_admin_password(config, setup.password.c_str());
    osinfo_install_config_set_hostname(config, setup.hostname.c_str());
    GError* error = nullptr;
    gchar* text = osinfo_install_script_generate_for_media(script, media, config, nullptr, &error);
    gchar* line = osinfo_install_script_generate_command_line_for_media(script, media, config);
    if (text) {
      answers = text;
      cmdline = line ? line : "";
      const char* expected = osinfo_install_script_get_expected_filename(script);
      filename = expected ? expected : "";
      kernelPath = osinfo_media_get_kernel_path(media);
      initrdPath = osinfo_media_get_initrd_path(media);
    }
    g_free(text);
    g_free(line);
    g_object_unref(config);
    g_object_unref(script);
    g_object_unref(os);
    g_object_unref(media);
    if (error) {
      std::string message = std::format("The installer's answers cannot be written: {}", error->message);
      g_error_free(error);
      return std::unexpected(std::move(message));
    }
  }
  if (answers.empty() || filename.empty()) {
    return std::unexpected(std::string("libosinfo wrote no answers for this medium."));
  }
  const fs::path kernel = fs::path(folder) / "install-kernel";
  const fs::path initrd = fs::path(folder) / "install-initrd";
  if (const auto extracted = runCommand(
          {"xorriso", "-osirrox", "on", "-indev", isoPath, "-extract", "/" + kernelPath, kernel.string(), "-extract",
           "/" + initrdPath, initrd.string()}
      );
      !extracted) {
    return std::unexpected(std::format("The installer cannot be taken from the medium: {}", extracted.error()));
  }
  std::error_code error;
  fs::permissions(initrd, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::add, error);
  std::ofstream out(initrd, std::ios::binary | std::ios::app);
  // The kernel reads the next archive from a 4-byte boundary on; zeros in between are skipped.
  const auto size = fs::file_size(initrd, error);
  out << std::string((4 - size % 4) % 4, '\0') << newcArchive(filename, answers);
  if (!out.flush()) {
    return std::unexpected(std::format("{} cannot be written.", initrd.string()));
  }
  return UnattendedFiles{.boot = {.kernel = kernel.string(), .initrd = initrd.string(), .cmdline = cmdline}, .answerDisk = {}};
}

std::optional<coffret::InstallMedia> coffret::detectInstallMedia(const std::string& isoPath) {
  const std::lock_guard lock(databaseMutex());
  OsinfoDb* db = database();
  if (!db) {
    return std::nullopt;
  }
  auto [media, os] = identify(db, isoPath);
  if (!media) {
    return std::nullopt;
  }
  std::optional<InstallMedia> result;
  {
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
    if (windowsMedium(media, os)) {
      found.unattended = true;
    } else if (OsinfoInstallScript* script = initrdScript(media, os)) {
      found.unattended = true;
      g_object_unref(script);
    }
    g_object_unref(os);
    result = std::move(found);
  }
  g_object_unref(media);
  return result;
}
