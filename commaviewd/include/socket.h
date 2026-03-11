#pragma once

namespace commaview::net {

bool client_socket_alive(int fd);
int create_server(int port);

}  // namespace commaview::net
