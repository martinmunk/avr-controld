// avr-controld: single-owner broker for one AVR connection.
//
//   avr-controld --type <backend> -o key=value [-o ...] [--socket PATH] [--foreground]
//
// Owns the real Backend (serial/TCP) and serves the newline-delimited protocol
// (see include/avr_protocol.hpp) on a Unix socket. One daemon per physical
// device, enforced by an flock on "<socket>.lock". Single-threaded poll loop, so
// all client access is serialized on the wire.
//
// Optionally (config `dbus=observe|drive`) it also watches shairport-sync over
// D-Bus and, in `drive` mode, powers the AVR on and selects the configured
// input when playback starts -- verifying the receiver actually got there via
// its own live reads and retrying -- and powers it off after a grace period
// once playback stops. This is additive: with `dbus=off` (the default) the
// daemon behaves exactly as before, and the ALSA/volume path is never touched.

#include <algorithm>
#include "../../include/backend.hpp"
#include "../../include/avr_protocol.hpp"
#include "../../include/logger.hpp"
#include "../../include/avr_session_policy.hpp"
#include "../../include/shairport_dbus.hpp"
#include "../../include/volume_map.hpp"
#include "../../include/volume_sync.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <fstream>
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

// vol_ceiling is a HARD cap on any volume this daemon will set on the receiver.
// It is the last line of defence: even a stray/buggy 'SET volume 100' is clamped.
// 100 == no cap (normal mode); in volume_sync mode it is max_volume.
static std::string dispatch(Backend* be, const std::string& line, int vol_ceiling) {
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
        if (noun == "volume")  { int v; if (!(is >> v)) return "ERR bad value";
                                 if (v < 0) v = 0; if (v > vol_ceiling) v = vol_ceiling;
                                 return boolr(be->set_volume_absolute(v)); }
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

// ---- time helpers --------------------------------------------------------
static int64_t mono_sec()  { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec; }
static uint64_t mono_usec(){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
                             return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL; }
static int64_t  mono_msec(){ return (int64_t)(mono_usec() / 1000ULL); }

// Optional host-global config file (key=value). Lets an auto-spawned daemon be
// switched into D-Bus mode without changing the ALSA conf schema: drop e.g.
// "dbus=drive" in /etc/avr-controld.conf. Only fills keys not already set on the
// command line. Host-global: on a node with multiple AVRs it applies to each
// daemon; these single-AVR Pis are the intended case.
static void merge_config_file(std::map<std::string, std::string>& cfg, const char* path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t\r\n");
            size_t b = s.find_last_not_of(" \t\r\n");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (!k.empty() && cfg.find(k) == cfg.end()) cfg[k] = v;
    }
}

// Ensure the AVR is powered on and on `want_input`, verifying via live reads and
// retrying (the backend's own reconnect/backoff handles a session dropped during
// power-on). Returns true once confirmed.
static bool ensure_power_and_input(Backend* be, const std::string& want_input, Logger& log) {
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (!be->is_connected()) be->reconnect();
        be->set_power(true);
        if (!want_input.empty()) be->set_input(want_input);
        auto p  = be->get_power();
        auto in = be->get_current_input();
        bool power_ok = p.value_or(false);
        bool input_ok = want_input.empty() || (in && *in == want_input);
        if (power_ok && input_ok) {
            log.info("D-Bus: AVR confirmed on" +
                     (want_input.empty() ? std::string() : " + input=" + want_input) +
                     " (attempt " + std::to_string(attempt) + ")");
            return true;
        }
        log.warn("D-Bus: power/input not confirmed yet (power=" +
                 std::string(power_ok ? "1" : "0") + " input=" + (in ? *in : "?") +
                 "), retry " + std::to_string(attempt));
        usleep(700 * 1000);
    }
    log.error("D-Bus: gave up confirming power/input after 4 attempts");
    return false;
}

// Power the AVR off. Deliberately does NOT change the input: switching to a
// (likely HDMI) input on the way to standby is what drives the CEC wake cascade
// on the Denon/TV/Shield bus, so we leave it as is.
static void power_off_avr(Backend* be, Logger& log) {
    if (!be->is_connected()) be->reconnect();
    be->set_power(false);
    auto p = be->get_power();
    if (p && !*p) log.info("D-Bus: AVR powered off (input left as is)");
    else          log.warn("D-Bus: power-off not confirmed");
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

    merge_config_file(config, "/etc/avr-controld.conf");

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

    // Daemon-level logger, sharing the backend's log file/level so D-Bus and AVR
    // lines interleave in one place.
    Logger log;
    log.init(config.count("log_file") ? config.at("log_file") : "",
             config.count("log_level") ? config.at("log_level") : "INFO");

    // ---- optional shairport-sync D-Bus watcher --------------------------
    // dbus = off (default) | observe (log only) | drive (also power the AVR)
    std::string dbus_mode = config.count("dbus") ? config.at("dbus") : "off";
    std::string want_input = config.count("input") ? config.at("input") : "";
    int off_grace = config.count("dbus_off_grace") ? atoi(config.at("dbus_off_grace").c_str()) : 120;
    if (off_grace <= 0) off_grace = 120;

    // ---- optional volume sync (shairport .Volume <-> AVR volume/mute) -------
    // volume_sync = off (default) | on. When on, the daemon keeps shairport's
    // AirPlay volume and the receiver's volume/mute in step in BOTH directions,
    // with echo suppression and coalescing. REQUIRES shairport to run with
    // ignore_volume_control = "yes" (so shairport outputs full-scale and does
    // not itself drive the receiver -- the daemon owns the attenuation). Needs
    // the D-Bus watcher, so it implies a bus connection.
    auto truthy = [](const std::string& v){ return v=="on"||v=="1"||v=="yes"||v=="true"; };
    bool volume_sync_on = config.count("volume_sync") && truthy(config.at("volume_sync"));
    int max_vol = config.count("max_volume") ? atoi(config.at("max_volume").c_str()) : 60;
    if (max_vol < 0) max_vol = 0; else if (max_vol > 100) max_vol = 100;
    int coalesce_ms = config.count("volume_coalesce_ms") ? atoi(config.at("volume_coalesce_ms").c_str()) : 150;
    if (coalesce_ms < 0) coalesce_ms = 0;
    double airplay_min_db = config.count("airplay_min_db") ? atof(config.at("airplay_min_db").c_str()) : -30.0;

    VolumeMap vmap; vmap.max_pct = max_vol; vmap.airplay_min_db = airplay_min_db;
    VolumeSync volsync(vmap, coalesce_ms);

    ShairportWatcher watcher(log);
    SessionPolicy policy(off_grace);
    const bool power_mode = (dbus_mode == "observe" || dbus_mode == "drive");
    const bool drive = (dbus_mode == "drive");
    bool dbus_on = false;

    // Apply a VolumeSync decision to the two sides (mute before volume so an
    // unmute+level in one update lands in the right order).
    auto apply_vol = [&](const VolumeSync::Action& a) {
        if (a.set_denon_mute) backend->set_mute(*a.set_denon_mute);
        if (a.set_denon_pct)  backend->set_volume_absolute(std::min(*a.set_denon_pct, max_vol));
        if (a.write_airplay_db) watcher.set_volume_db(*a.write_airplay_db);
    };

    if (power_mode || volume_sync_on) {
        dbus_on = watcher.connect();   // false if the system bus is unavailable
        if (dbus_on) {
            if (power_mode)
                log.info(std::string("D-Bus: mode=") + dbus_mode +
                         (drive ? (" input=" + (want_input.empty() ? std::string("(none)") : want_input) +
                                   " off_grace=" + std::to_string(off_grace) + "s") : ""));
            if (volume_sync_on)
                log.info("D-Bus: volume_sync=on max_volume=" + std::to_string(max_vol) +
                         " coalesce=" + std::to_string(coalesce_ms) + "ms airplay_min_db=" +
                         std::to_string(airplay_min_db) +
                         " (requires shairport ignore_volume_control=yes)");

            watcher.set_signal_handler([&](SessionSignal sig) {
                if (power_mode) {
                    AvrAction act = policy.feed(sig, mono_sec());
                    if (act != AvrAction::None) {
                        if (!drive)
                            log.info(std::string("D-Bus (observe): would ") +
                                     (act == AvrAction::PowerOnSelectInput ? "power on + select input" : "power off"));
                        else if (act == AvrAction::PowerOnSelectInput) ensure_power_and_input(backend.get(), want_input, log);
                        else if (act == AvrAction::PowerOff)           power_off_avr(backend.get(), log);
                    }
                }
                // On real playback start, snap the receiver to the phone's
                // current volume (capped) so the very first note is at a safe,
                // correct level rather than whatever MV the AVR was left at.
                if (volume_sync_on && sig == SessionSignal::Play && watcher.has_volume())
                    apply_vol(volsync.on_airplay_db(watcher.volume(), mono_msec()));
            });

            if (volume_sync_on)
                watcher.set_volume_handler([&](double db) {
                    apply_vol(volsync.on_airplay_db(db, mono_msec()));
                });
        } else {
            log.warn("D-Bus: requested but unavailable; running ALSA-only as usual");
        }
    }

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
    // Persistent set: [0]=lsock, then client fds. The D-Bus fd (if any) is
    // appended per iteration and handled separately, never treated as a client.
    std::vector<struct pollfd> fds;
    fds.push_back({lsock, POLLIN, 0});
    std::map<int, std::string> inbuf;   // per-client read buffer

    while (true) {
        int busfd = dbus_on ? watcher.fd() : -1;

        std::vector<struct pollfd> pfds = fds;              // lsock + clients
        size_t busslot = (size_t)-1;
        if (busfd >= 0) { busslot = pfds.size(); pfds.push_back({busfd, watcher.poll_events(), 0}); }
        (void)busslot;

        // The AVR's own push fd (Denon telnet), watched only in volume_sync mode
        // so external volume/mute changes reflect to the phone in real time.
        int evfd = (volume_sync_on && dbus_on) ? backend->event_fd() : -1;
        size_t evslot = (size_t)-1;
        if (evfd >= 0) { evslot = pfds.size(); pfds.push_back({evfd, POLLIN, 0}); }

        // Bound the wait by the D-Bus timeout and the policy's pending power-off.
        int timeout_ms = -1;
        if (busfd >= 0) {
            uint64_t tv = watcher.timeout_usec();
            if (tv != UINT64_MAX) {
                uint64_t now = mono_usec();
                int64_t ms = tv <= now ? 0 : (int64_t)((tv - now) / 1000);
                timeout_ms = (int)ms;
            }
        }
        if (dbus_on) {
            if (auto dl = policy.next_deadline()) {
                int64_t secs = *dl - mono_sec();
                int ms = secs <= 0 ? 0 : (int)(secs * 1000);
                if (timeout_ms < 0 || ms < timeout_ms) timeout_ms = ms;
            }
        }

        if (volume_sync_on && dbus_on) {
            if (auto fl = volsync.next_flush_ms()) {
                int64_t ms = *fl - mono_msec();
                int m = ms <= 0 ? 0 : (int)ms;
                if (timeout_ms < 0 || m < timeout_ms) timeout_ms = m;
            }
        }

        int n = poll(pfds.data(), pfds.size(), timeout_ms);
        if (n < 0) { if (errno == EINTR) continue; break; }

        // Service the bus first (also on timeout, for its own timers), then run
        // the policy's grace-timer.
        if (busfd >= 0) {
            watcher.process();
            AvrAction act = policy.tick(mono_sec());
            if (act == AvrAction::PowerOff && dbus_mode == "drive")
                power_off_avr(backend.get(), log);
            else if (act == AvrAction::PowerOff)
                log.info("D-Bus (observe): would power off (grace elapsed)");
        }

        // Real-time AVR pushes (external volume/mute changes) -> reflect to the
        // phone; then flush any coalesced volume the poll timeout just matured.
        if (volume_sync_on && dbus_on) {
            if (evfd >= 0 && evslot != (size_t)-1 &&
                (pfds[evslot].revents & (POLLIN | POLLHUP | POLLERR))) {
                AvrPush push = backend->pump_events();
                if (push.volume) apply_vol(volsync.on_denon_pct(*push.volume, mono_msec()));
                if (push.mute)   apply_vol(volsync.on_denon_mute(*push.mute, mono_msec()));
            }
            apply_vol(volsync.tick(mono_msec()));
        }

        // Handle lsock + clients using the persistent-portion revents. Defer
        // closes so erasing never desyncs the pfds<->fds index mapping.
        size_t nfds = fds.size();
        std::vector<int> to_close;
        for (size_t i = 0; i < nfds; ++i) {
            if (!(pfds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;

            if (fds[i].fd == lsock) {
                int c = accept(lsock, nullptr, nullptr);
                if (c >= 0) { fds.push_back({c, POLLIN, 0}); inbuf[c]; }
                continue;
            }

            int cfd = fds[i].fd;
            char buf[1024];
            ssize_t r = read(cfd, buf, sizeof(buf));
            if (r <= 0) { to_close.push_back(cfd); continue; }

            std::string& b = inbuf[cfd];
            b.append(buf, r);
            size_t pos;
            while ((pos = b.find('\n')) != std::string::npos) {
                std::string line = b.substr(0, pos);
                b.erase(0, pos + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                if (line == "SHUTDOWN") {          // graceful stop (systemd/tests)
                    const char* okmsg = "OK\n"; ssize_t w = write(cfd, okmsg, 3); (void)w;
                    unlink(sock.c_str());
                    _exit(0);
                }
                std::string reply;
                if (line == "META") {              // now-playing snapshot (D-Bus)
                    reply = dbus_on ? "OK " + watcher.snapshot() : "OK -";
                } else if (line == "PLAYSTATE") {
                    reply = dbus_on ? "OK " + (watcher.player_state().empty() ? std::string("-")
                                                                              : watcher.player_state())
                                    : "OK -";
                } else {
                    if (!backend->is_connected()) backend->reconnect();   // best effort
                    reply = dispatch(backend.get(), line, volume_sync_on ? max_vol : 100);
                }
                reply += "\n";
                if (write(cfd, reply.data(), reply.size()) < 0) { /* client gone; cleaned up below */ }
            }
        }

        for (int fd : to_close) {
            close(fd);
            inbuf.erase(fd);
            for (auto it = fds.begin(); it != fds.end(); ++it)
                if (it->fd == fd) { fds.erase(it); break; }
        }
    }
    return 0;
}
