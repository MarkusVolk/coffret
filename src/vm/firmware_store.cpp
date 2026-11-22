#include "vm/firmware_store.h"

#include "fs/xdg_dirs.h"
#include "storage/process.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <vector>

namespace {
  namespace fs = std::filesystem;

  struct Described {
    std::string code, codeFormat, variables, variablesFormat;
  };

  // The description's firmware when it is one for x86_64 on q35 over flash with the secure boot asked for.
  std::optional<Described> matching(const fs::path& file, bool secureBoot) {
    std::ifstream stream(file);
    const auto root = nlohmann::json::parse(stream, nullptr, false);
    if (!root.is_object()) {
      return std::nullopt;
    }
    const auto contains = [&root](const char* key, const char* value) {
      const auto list = root.find(key);
      return list != root.end() && list->is_array() && std::ranges::find(*list, value) != list->end();
    };
    bool q35 = false;
    for (const auto& target : root.value("targets", nlohmann::json::array())) {
      if (target.value("architecture", "") != "x86_64") {
        continue;
      }
      for (const auto& machine : target.value("machines", nlohmann::json::array())) {
        q35 = q35 || (machine.is_string() && machine.get<std::string>().find("q35") != std::string::npos);
      }
    }
    const auto mapping = root.value("mapping", nlohmann::json::object());
    if (!q35 || !contains("interface-types", "uefi") || contains("features", "secure-boot") != secureBoot
        || mapping.value("device", "") != "flash") {
      return std::nullopt;
    }
    const auto code = mapping.value("executable", nlohmann::json::object());
    const auto variables = mapping.value("nvram-template", nlohmann::json::object());
    Described described{code.value("filename", ""), code.value("format", "raw"), variables.value("filename", ""),
                        variables.value("format", "raw")};
    return described.code.empty() || described.variables.empty() ? std::nullopt : std::optional(described);
  }

  // The file as qcow2: itself, or a copy that is made again when the firmware was updated.
  std::expected<std::string, std::string> asQcow2(const std::string& file, const std::string& format, const fs::path& folder) {
    if (format == "qcow2") {
      return file;
    }
    std::error_code error;
    const fs::path target = folder / (fs::path(file).stem().string() + ".qcow2");
    const auto source = fs::last_write_time(file, error);
    if (!error && fs::exists(target, error) && fs::last_write_time(target, error) >= source) {
      return target.string();
    }
    fs::create_directories(folder, error);
    const fs::path fresh = target.string() + ".new";
    const auto converted = coffret::runCommand({"qemu-img", "convert", "-f", format, "-O", "qcow2", file, fresh.string()});
    if (!converted) {
      fs::remove(fresh, error);
      return std::unexpected(converted.error());
    }
    fs::rename(fresh, target, error);
    if (error) {
      return std::unexpected(error.message());
    }
    return target.string();
  }
} // namespace

std::expected<coffret::FirmwareFiles, std::string> coffret::prepareFirmware(bool secureBoot) {
  // In QEMU's order of precedence; within a folder by name.
  std::vector<fs::path> files;
  std::error_code error;
  for (const fs::path& folder : {configHome() / "qemu/firmware", fs::path("/etc/qemu/firmware"), fs::path("/usr/share/qemu/firmware")}) {
    std::vector<fs::path> found;
    for (const auto& entry : fs::directory_iterator(folder, error)) {
      if (entry.path().extension() == ".json") {
        found.push_back(entry.path());
      }
    }
    std::ranges::sort(found);
    files.insert(files.end(), found.begin(), found.end());
  }
  for (const auto& file : files) {
    const auto described = matching(file, secureBoot);
    if (!described) {
      continue;
    }
    const fs::path folder = dataHome() / "coffret" / "firmware";
    const auto code = asQcow2(described->code, described->codeFormat, folder);
    if (!code) {
      return std::unexpected(code.error());
    }
    const auto variables = asQcow2(described->variables, described->variablesFormat, folder);
    if (!variables) {
      return std::unexpected(variables.error());
    }
    return FirmwareFiles{*code, *variables};
  }
  return std::unexpected(std::string(
      secureBoot ? "QEMU has no UEFI firmware with secure boot for this kind of machine." : "QEMU has no UEFI firmware for this kind of machine."
  ));
}
