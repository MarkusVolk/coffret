#include "storage/process.h"

#include "fs/gio_ptr.h"

#include <format>
#include <gio/gio.h>

std::expected<std::string, std::string> coffret::runCommand(const std::vector<std::string>& command) {
  std::vector<const char*> argv;
  for (const auto& argument : command) {
    argv.push_back(argument.c_str());
  }
  argv.push_back(nullptr);
  GError* error = nullptr;
  GSubprocess* process = g_subprocess_newv(
      argv.data(), static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE), &error
  );
  if (!process) {
    std::string message = std::format("{} could not be started: {}", command.front(), error->message);
    g_error_free(error);
    return std::unexpected(std::move(message));
  }
  char* out = nullptr;
  char* complaint = nullptr;
  g_subprocess_communicate_utf8(process, nullptr, nullptr, &out, &complaint, &error);
  std::string output = out ? out : "";
  std::string problem = complaint ? complaint : "";
  g_free(out);
  g_free(complaint);
  const bool ok = !error && g_subprocess_get_successful(process);
  if (error) {
    problem = error->message;
    g_error_free(error);
  }
  g_object_unref(process);
  if (!ok) {
    while (!problem.empty() && (problem.back() == '\n' || problem.back() == ' ')) {
      problem.pop_back();
    }
    return std::unexpected(problem.empty() ? std::format("{} failed.", command.front()) : problem);
  }
  return output;
}
