#pragma once

namespace httplib {
class Server;
} // namespace httplib

namespace fh6 {
class AudioSourceManager;
class ConfigStore;
} // namespace fh6

namespace fh6::http {

void wire_roon_routes(httplib::Server& svr, AudioSourceManager& mgr, ConfigStore& store);

} // namespace fh6::http
