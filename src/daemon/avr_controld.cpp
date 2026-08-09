// avr-controld: single-owner broker for one AVR connection.
//
//   avr-controld --type <backend> -o key=value [-o ...] [--socket PATH] [--foreground]
//
// Owns the real Backend (serial/TCP) and serves the newline-delimited protocol
// (see include/avr_protocol.hpp) on a Unix socket. One daemon per physical
// device, enforced by an flock on "<socket>.lock". Single-threaded poll loop, so
// all client access is serialized on the wire.

#include "../../include/backend.hpp"
#include "../../include/avr_protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <optional>

using namespace AlsaAVR;

// ---- Speaker <-> string --------------------------------------------------
static const std::pair<Speaker, const char*> kSpeakers[] = {
    {Speaker::FRONT_LEFT, "FRONT_LEFT"}, {Speaker::FRONT_RIGHT, "FRONT_RIGHT"},
    {Speaker::CENTER, "CENTER"}, {Speaker::SUBWOOFER, "SUBWOOFER"},
    {Speaker::SURROUND_LEFT, "SURROUND_LEFT"}, {Speaker::SURROUND_RIGHT, "SURROUND_RIGHT"},
    {Speaker::SURROUND_BACK_LEFT, "SURROUND_BACK_LEFT"}, {Speaker::SURROUND_BACK_RIGHT, "SURROUND_BACK_RIGHT"},
    {Speaker::SURROUND_BACK, "SURROUND_BACK"},
    {Speaker::FRONT_HEIGHT_LEFT, "FRONT_HEIGHT_LEFT"}, {Speaker::FRONT_HEIGHT_RIGHT, "FRONT_HEIGHT_RIGHT"},
};
static const char* speaker_to_str(Speaker s) {
    for (auto& p : kSpeakers) if (p.first == s) return p.second;
    return "FRONT_LEFT";
}
static std::optional<Speaker> str_to_speaker(const std::string& s) {
    for (auto& p : kSpeakers) if (s == p.second) return p.first;
    return std::nullopt;
}

// ---- Command dispatch ----------------------------------------------------
static std::string opt_int(std::optional<int> v)  { return v ? std::to_string(*v) : std::string("-"); }
static std::string opt_bool(std::optional<bool> v) { return v ? std::string(*v ? "1" : "0") : std::string("-"); }
static std::string opt_str(std::optional<std::string> v) { return v ? *v : std::string("-"); }

static std::string dispatch(Backend* be, const std::string& line) {
    std::istringstream is(line);
    std::string verb; is >> verb;
    auto ok    = [](const std::string& p = "") { return p.empty() ? std::string("OK") : "OK " + p; };
    auto boolr = [&](bool r) { return r ? std::string("OK") : std::string("ERR failed"); };

    if (verb == "PING")      return ok();
    if (verb == "NAME")      return ok(be->name());
    if (verb == "CONNECTED") return ok(be->is_connected() ? "1" : "0");
    if (verb == "HWMAX")     return ok(std::to_string(be->get_hardware_max_volume()));
    if (verb == "SPKMIN")    return ok(std::to_string(be->get_speaker_level_min()));
    if (verb == "SPKMAX")    return ok(std::to_string(be->get_speaker_level_max()));
    if (verb == "VOLUP")     return boolr(be->volume_up());
    if (verb == "VOLDOWN")   return boolr(be->volume_down());

    if (verb == "CAPS") {
        auto c = be->capabilities();
        std::ostringstream o;
        o << "abs=" << c.supports_absolute << " query=" << c.supports_query
          << " mute=" << c.supports_mute << " input=" << c.supports_input_select
          << " spk=" << c.supports_speaker_level << " standby=" << c.supports_standby_timeout
          << " minv=" << c.min_volume << " maxv=" << c.max_volume;
        return ok(o.str());
    }
    if (verb == "INPUTS") {
        std::string s; auto v = be->get_available_inputs();
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += v[i]; }
        return ok(s);
    }
    if (verb == "SPEAKERS") {
        std::string s; auto v = be->get_available_speakers();
        for (size_t i = 0; i < v.size(); ++i) { if (i) s += ","; s += speaker_to_str(v[i]); }
        return ok(s);
    }
    if (verb == "GET") {
        std::string noun; is >> noun;
        if (noun == "volume")  return ok(opt_int(be->get_volume()));
        if (noun == "mute")    return ok(opt_bool(be->get_mute()));
        if (noun == "power")   return ok(opt_bool(be->get_power()));
        if (noun == "standby") return ok(opt_bool(be->get_standby_timeout()));
        if (noun == "input")   return ok(opt_str(be->get_current_input()));
        if (noun == "speaker") {
            std::string sp; is >> sp;
            auto s = str_to_speaker(sp);
            if (!s) return "ERR bad speaker";
            return ok(std::to_string(be->get_speaker_level(*s)));
        }
        return "ERR bad GET";
    }
    if (verb == "SET") {
        std::string noun; is >> noun;
        if (noun == "volume")  { int v; if (!(is >> v)) return "ERR bad value"; return boolr(be->set_volume_absolute(v)); }
        if (noun == "mute")    { int v; if (!(is >> v)) return "ERR bad value"; return boolr(be->set_mute(v != 0)); }
        if (noun == "power")   { int v; if (!(is >> v)) return "ERR bad value"; return boolr(be->set_power(v != 0)); }
        if (noun == "standby") { int v; if (!(is >> v)) return "ERR bad value"; return boolr(be->set_standby_timeout(v != 0)); }
        if (noun == "input")   { std::string name; std::getline(is >> std::ws, name); return boolr(be->set_input(name)); }
        if (noun == "speaker") {
            std::string sp; int v;
            if (!(is >> sp >> v)) return "ERR bad value";
            auto s = str_to_speaker(sp);
            if (!s) return "ERR bad speaker";
            return boolr(be->set_speaker_level(*s, v));
        }
        return "ERR bad SET";
    }
    return "ERR unknown verb";
}

// ---- daemonize -----------------------------------------------------------
static void daemonize() {
    if (fork() > 0) _exit(0);          // parent leaves; child continues
    setsid();
    if (fork() > 0) _exit(0);          // drop session leadership
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
    if (chdir("/") != 0) { /* ignore */ }
}

int main(int argc, char** argv) {
    std::string type, socket_override;
    std::map<std::string, std::string> config;
    bool foreground = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--type" && i + 1 < argc)        type = argv[++i];
        else if (a == "--socket" && i + 1 < argc) socket_override = argv[++i];
        else if (a == "-o" && i + 1 < argc) {
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos) config[kv.substr(0, eq)] = kv.substr(eq + 1);
        } else if (a == "--foreground" || a == "-f") foreground = true;
    }
    if (type.empty()) { fprintf(stderr, "avr-controld: --type required\n"); return 2; }

    std::string sock = socket_override.empty() ? avr_socket_path(type, config) : socket_override;
    std::string lockpath = sock + ".lock";

    signal(SIGPIPE, SIG_IGN);
    if (!foreground) daemonize();

    // One daemon per device: whoever holds the flock owns it. Take it BEFORE
    // init() so a losing race never opens the device.
    int lockfd = open(lockpath.c_str(), O_CREAT | O_RDWR, 0666);
    if (lockfd < 0) { fprintf(stderr, "avr-controld: cannot open lock %s\n", lockpath.c_str()); return 1; }
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) return 0;   // another daemon owns this device

    auto backend = BackendFactory::create(type);
    if (!backend) { fprintf(stderr, "avr-controld: unknown backend %s\n", type.c_str()); return 1; }
    if (!backend->init(config)) { fprintf(stderr, "avr-controld: backend init failed\n"); return 1; }

    int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lsock < 0) return 1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
    unlink(sock.c_str());
    if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "avr-controld: bind %s failed\n", sock.c_str());
        return 1;
    }
    chmod(sock.c_str(), 0666);          // allow cross-user clients (shairport-sync/root)
    listen(lsock, 16);

    // ---- poll loop -------------------------------------------------------
    std::vector<struct pollfd> fds;
    fds.push_back({lsock, POLLIN, 0});
    std::map<int, std::string> inbuf;   // per-client read buffer

    while (true) {
        int n = poll(fds.data(), fds.size(), -1);
        if (n < 0) { if (errno == EINTR) continue; break; }

        for (size_t i = 0; i < fds.size(); ++i) {
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            if (fds[i].fd == lsock) {
                int c = accept(lsock, nullptr, nullptr);
                if (c >= 0) { fds.push_back({c, POLLIN, 0}); inbuf[c]; }
                continue;
            }

            int cfd = fds[i].fd;
            char buf[1024];
            ssize_t r = read(cfd, buf, sizeof(buf));
            if (r <= 0) { close(cfd); inbuf.erase(cfd); fds.erase(fds.begin() + i); --i; continue; }

            std::string& b = inbuf[cfd];
            b.append(buf, r);
            size_t pos;
            while ((pos = b.find('\n')) != std::string::npos) {
                std::string line = b.substr(0, pos);
                b.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (!backend->is_connected()) backend->reconnect();   // best effort
                std::string reply = dispatch(backend.get(), line) + "\n";
                if (write(cfd, reply.data(), reply.size()) < 0) { /* client gone; cleaned up next loop */ }
            }
        }
    }
    return 0;
}
