#include "qemu/qmp_attach.h"

#include <cerrno>
#include <cstring>
#include <format>
#include <nlohmann/json.hpp>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
  struct Fd {
    int fd = -1;
    ~Fd() {
      if (fd >= 0) {
        ::close(fd);
      }
    }
    int release() { return std::exchange(fd, -1); }
  };

  // One line of QMP, which is one JSON value; empty on a timeout or when QEMU hung up.
  std::string readLine(int fd, std::string& buffer) {
    for (;;) {
      if (const auto end = buffer.find('\n'); end != std::string::npos) {
        std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        return line;
      }
      pollfd waiting{.fd = fd, .events = POLLIN, .revents = 0};
      if (::poll(&waiting, 1, 5000) <= 0) {
        return {};
      }
      char chunk[4096];
      const ssize_t got = ::read(fd, chunk, sizeof(chunk));
      if (got <= 0) {
        return {};
      }
      buffer.append(chunk, static_cast<std::size_t>(got));
    }
  }

  // Sends a command, with a descriptor riding along when passFd is not -1, and waits for its
  // answer; events in between are skipped.
  std::expected<void, std::string> command(int fd, std::string& buffer, const nlohmann::json& request, int passFd = -1) {
    const std::string text = request.dump() + "\r\n";
    iovec data{.iov_base = const_cast<char*>(text.data()), .iov_len = text.size()};
    msghdr message{};
    message.msg_iov = &data;
    message.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    if (passFd >= 0) {
      message.msg_control = control;
      message.msg_controllen = sizeof(control);
      cmsghdr* header = CMSG_FIRSTHDR(&message);
      header->cmsg_level = SOL_SOCKET;
      header->cmsg_type = SCM_RIGHTS;
      header->cmsg_len = CMSG_LEN(sizeof(int));
      std::memcpy(CMSG_DATA(header), &passFd, sizeof(int));
    }
    if (::sendmsg(fd, &message, MSG_NOSIGNAL) != static_cast<ssize_t>(text.size())) {
      return std::unexpected(std::format("cannot talk to QEMU: {}", std::strerror(errno)));
    }
    for (;;) {
      const std::string line = readLine(fd, buffer);
      if (line.empty()) {
        return std::unexpected("QEMU did not answer");
      }
      const auto reply = nlohmann::json::parse(line, nullptr, false);
      if (!reply.is_object() || reply.contains("event")) {
        continue;
      }
      if (reply.contains("error")) {
        return std::unexpected(reply["error"].value("desc", std::string("QEMU refused the request")));
      }
      if (reply.contains("return")) {
        return {};
      }
    }
  }
} // namespace

std::expected<int, std::string> coffret::openDisplayBus(const std::string& qmpSocketPath) {
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (qmpSocketPath.size() >= sizeof(address.sun_path)) {
    return std::unexpected("the socket path is too long");
  }
  std::memcpy(address.sun_path, qmpSocketPath.c_str(), qmpSocketPath.size() + 1);
  Fd qmp{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
  if (qmp.fd < 0 || ::connect(qmp.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    return std::unexpected(std::format("{}: {}", qmpSocketPath, std::strerror(errno)));
  }
  std::string buffer;
  if (readLine(qmp.fd, buffer).empty()) {
    return std::unexpected("QEMU sent no greeting");
  }
  if (auto done = command(qmp.fd, buffer, {{"execute", "qmp_capabilities"}}); !done) {
    return std::unexpected(done.error());
  }
  int pair[2];
  if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0) {
    return std::unexpected("cannot create a socket pair");
  }
  Fd ours{pair[0]}, theirs{pair[1]};
  const char* name = "coffret-display";
  if (auto done = command(qmp.fd, buffer, {{"execute", "getfd"}, {"arguments", {{"fdname", name}}}}, theirs.fd); !done) {
    return std::unexpected(done.error());
  }
  if (auto done = command(
          qmp.fd, buffer, {{"execute", "add_client"}, {"arguments", {{"protocol", "@dbus-display"}, {"fdname", name}}}}
      );
      !done) {
    return std::unexpected(done.error());
  }
  return ours.release();
}
