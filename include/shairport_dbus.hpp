#pragma once
//
// ShairportWatcher: a thin sd-bus (libsystemd) client that subscribes to
// shairport-sync's D-Bus interface on the SYSTEM bus and turns its property
// changes into SessionSignal callbacks, while caching now-playing metadata.
//
// Why sd-bus: libsystemd is already installed on every Debian 13 node (systemd
// is PID 1), cross-compiles for arm64 out of the box, and exposes a single
// pollable fd (sd_bus_get_fd) that folds straight into the daemon's existing
// poll() loop -- no glib main loop, no extra runtime dependency.
//
// D-Bus surface actually used (verified live against shairport-sync 5.2.1):
//   bus name : org.gnome.ShairportSync           (system bus)
//   path     : /org/gnome/ShairportSync
//   iface org.gnome.ShairportSync            prop Active (b)
//   iface org.gnome.ShairportSync.RemoteControl
//                                            prop PlayerState (s: Playing/
//                                                 Paused/Stopped/Not Available)
//                                            prop Metadata (a{sv}, xesam:*)
//                                            prop ProgressString (s)
// All delivered via org.freedesktop.DBus.Properties.PropertiesChanged.

#include <string>
#include <functional>
#include <cstdint>
#include "avr_session_policy.hpp"
#include "logger.hpp"

struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;

namespace AlsaAVR {

class ShairportWatcher {
public:
    using SignalHandler = std::function<void(SessionSignal)>;

    explicit ShairportWatcher(Logger& logger) : logger_(logger) {}
    ~ShairportWatcher();

    ShairportWatcher(const ShairportWatcher&) = delete;
    ShairportWatcher& operator=(const ShairportWatcher&) = delete;

    // Connect to the system bus and install the PropertiesChanged match, then
    // read the current state once. Returns false if the bus is unavailable
    // (the caller then simply runs without D-Bus). Never throws.
    bool connect();

    // Poll integration: fold these into the daemon's pollfd set.
    int  fd() const;              // -1 if not connected
    short poll_events() const;    // POLLIN/POLLOUT flags sd-bus wants
    // Absolute CLOCK_MONOTONIC microseconds sd-bus next needs servicing, or
    // UINT64_MAX for "no timeout". Fold into the poll() deadline.
    uint64_t timeout_usec() const;

    // Drain all pending bus work (call after poll wakes, or opportunistically).
    void process();

    void set_signal_handler(SignalHandler h) { on_signal_ = std::move(h); }

    // Cached now-playing state (updated as PropertiesChanged arrives).
    bool active() const { return active_; }
    const std::string& player_state() const { return player_state_; }
    const std::string& title()  const { return title_; }
    const std::string& artist() const { return artist_; }
    const std::string& album()  const { return album_; }
    // One-line, pipe-delimited snapshot for the socket protocol / logging.
    std::string snapshot() const;

    // Internal: invoked from the C match callback. Public so the file-scope
    // trampoline can reach it; not part of the intended API.
    int on_properties_changed(sd_bus_message* m);

private:
    void refresh_initial_state();
    void emit(SessionSignal sig) { if (on_signal_) on_signal_(sig); }
    // Apply a new PlayerState string, emitting the matching signal on change.
    void apply_player_state(const std::string& s);
    void apply_active(bool a);

    Logger& logger_;
    sd_bus* bus_ = nullptr;
    sd_bus_slot* slot_ = nullptr;
    SignalHandler on_signal_;

    bool active_ = false;
    std::string player_state_;
    std::string title_, artist_, album_, progress_;
};

} // namespace AlsaAVR
