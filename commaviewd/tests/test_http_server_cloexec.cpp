#define private public
#include "http_server.h"
#undef private

#include <arpa/inet.h>
#include <cassert>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int reserve_any_port() {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);

  int opt = 1;
  assert(::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == 0);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = 0;
  assert(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);

  socklen_t len = sizeof(addr);
  assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0);
  const int port = ntohs(addr.sin_port);
  ::close(fd);
  return port;
}

void test_listener_sets_close_on_exec() {
  const int port = reserve_any_port();
  commaview::api::HttpServer server(port, [](const commaview::api::HttpRequest&) {
    return commaview::api::HttpResponse{};
  });

  std::string error;
  assert(server.start(&error));
  assert(error.empty());
  assert(server.server_fd_ >= 0);

  const int flags = ::fcntl(server.server_fd_, F_GETFD);
  assert(flags >= 0);
  assert((flags & FD_CLOEXEC) && "HttpServer listener must be close-on-exec so restart helper children cannot inherit :5002");

  server.stop();
}

}  // namespace

int main() {
  test_listener_sets_close_on_exec();
  return 0;
}
