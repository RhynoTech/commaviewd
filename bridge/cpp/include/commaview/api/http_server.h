#pragma once

#include <functional>
#include <string>
#include <unordered_map>

namespace commaview::api {

struct HttpRequest {
  std::string method;
  std::string path;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
  std::unordered_map<std::string, std::string> headers;
};

using RequestHandler = std::function<HttpResponse(const HttpRequest&)>;

class HttpServer {
 public:
  HttpServer(int port, RequestHandler handler);
  ~HttpServer();

  bool start(std::string* error);
  void serve_forever();
  void stop();

 private:
  bool handle_client(int client_fd);

  int port_;
  int server_fd_ = -1;
  RequestHandler handler_;
  bool running_ = false;
};

}  // namespace commaview::api
