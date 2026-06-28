#include "io/socket_util.h"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

namespace miniredis {

int CreateListenSocket(const std::string& bind_addr, int port, int backlog) {
  int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;

  int one = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, bind_addr.c_str(), &addr.sin_addr) <= 0) {
    ::close(fd);
    return -1;
  }

  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return -1;
  }

  if (::listen(fd, backlog) < 0) {
    ::close(fd);
    return -1;
  }

  return fd;
}

}  // namespace miniredis
