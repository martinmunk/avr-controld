#pragma once
//
// Shared contract between avr-controld (server) and SocketClientBackend (client):
// a deterministic Unix-socket path per physical device, and a small
// newline-delimited text protocol.
//
// Protocol: the client sends one command line ("VERB [args]\n"); the daemon
// replies with exactly one line, either "OK[ payload]\n" or "ERR message\n".
// A boolean/int/string state that is unknown (backend returned nullopt) is
// reported as the payload "-".
//
// Commands (mirroring the Backend interface):
//   CAPS                         -> OK abs=1 query=1 mute=1 input=1 spk=0 standby=1 minv=0 maxv=100
//   HWMAX                        -> OK 70
//   NAME                         -> OK Denon RS232
//   CONNECTED                    -> OK 1 | OK 0
//   GET volume|mute|power|standby|input   -> OK <val> | OK -
//   SET volume <0-100>           -> OK | ERR ...
//   SET mute|power|standby <0|1> -> OK | ERR ...
//   SET input <name>             -> OK | ERR ...
//   VOLUP | VOLDOWN              -> OK | ERR ...
//   INPUTS                       -> OK name1,name2,...   (empty -> "OK ")
//   SPEAKERS                     -> OK FRONT_LEFT,FRONT_RIGHT,...
//   GET speaker <SPK>            -> OK 50
//   SET speaker <SPK> <38-62>    -> OK | ERR ...
//   SPKMIN | SPKMAX              -> OK 38 | OK 62
//   PING                         -> OK
//

#include <string>
#include <map>
#include <cctype>
#include <cstdlib>
#include <sys/stat.h>

namespace AlsaAVR {

// Fixed socket directory. Shipped as a tmpfiles.d entry (0777) and best-effort
// created here so both client and daemon always agree on the path regardless of
// which user gets there first. AVR_SOCKET_DIR overrides it (for tests / non-root).
inline std::string avr_socket_dir() {
    if (const char* e = ::getenv("AVR_SOCKET_DIR")) {
        if (*e) { ::mkdir(e, 0777); return e; }
    }
    static const char* dir = "/run/avr-controld";
    ::mkdir(dir, 0777);   // best effort; ignore EEXIST / EACCES
    return dir;
}

inline std::string avr_sanitize(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        out += (std::isalnum(c) ? static_cast<char>(c) : '_');
    return out;
}

// Identity of the *physical* device so cosmetic config (log_file/log_level/input)
// does not spawn a separate daemon: the device path if present, else host:port.
inline std::string avr_identity(const std::map<std::string, std::string>& cfg) {
    auto d = cfg.find("device");
    if (d != cfg.end() && !d->second.empty())
        return d->second;
    std::string host, port;
    auto h = cfg.find("host"); if (h != cfg.end()) host = h->second;
    auto p = cfg.find("port"); if (p != cfg.end()) port = p->second;
    return host + ":" + port;
}

inline std::string avr_socket_path(const std::string& type,
                                   const std::map<std::string, std::string>& cfg) {
    return avr_socket_dir() + "/" + avr_sanitize(type) + "_" +
           avr_sanitize(avr_identity(cfg)) + ".sock";
}

} // namespace AlsaAVR
