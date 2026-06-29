// include/hades/serve_config.h — resolve the HTTP front-end bind config from the manifest
//
// Reads an optional `Serve { host, port, webroot }` block. host defaults to loopback
// (127.0.0.1), port to 8080 (a positive cli_port from `--serve <port>` overrides the
// block), webroot to "web". Pure; never throws (bad values fall back to defaults).
#pragma once
#include <string>
#include "hades/config.h"
namespace hades {
struct ServeConfig {
  std::string host;
  int port;
  std::string webroot;
};
ServeConfig resolve_serve_config(const Manifest& m, int cli_port);
}  // namespace hades
