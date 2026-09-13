// ShairportWatcher implementation -- see include/shairport_dbus.hpp.

#include "../../include/shairport_dbus.hpp"

#include <systemd/sd-bus.h>
#include <poll.h>
#include <cstring>
#include <cmath>

namespace AlsaAVR {

static const char* kBusName = "org.gnome.ShairportSync";
static const char* kPath    = "/org/gnome/ShairportSync";
static const char* kIfaceMain   = "org.gnome.ShairportSync";
static const char* kIfaceRemote = "org.gnome.ShairportSync.RemoteControl";

// ---- a{sv} metadata parser ----------------------------------------------
// Cursor must be positioned to read an "a{sv}" (an array of dict entries).
// Pulls out the xesam:* fields we care about; ignores the rest.
static void parse_metadata(sd_bus_message* m, std::string& title,
                           std::string& artist, std::string& album) {
    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") < 0) return;
    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(m, "s", &key) < 0 || !key) {
            sd_bus_message_exit_container(m);
            break;
        }
        const char* contents = nullptr;
        char vtype = 0;
        sd_bus_message_peek_type(m, &vtype, &contents);   // vtype == 'v'
        if (std::strcmp(key, "xesam:title") == 0 && contents && !std::strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "v", "s", &v);
            title = v ? v : "";
        } else if (std::strcmp(key, "xesam:album") == 0 && contents && !std::strcmp(contents, "s")) {
            const char* v = nullptr;
            sd_bus_message_read(m, "v", "s", &v);
            album = v ? v : "";
        } else if (std::strcmp(key, "xesam:artist") == 0 && contents && !std::strcmp(contents, "as")) {
            // artist is an array of strings; take the first.
            artist.clear();
            sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "as");
            sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "s");
            const char* v = nullptr;
            if (sd_bus_message_read(m, "s", &v) > 0 && v) artist = v;
            // skip any remaining entries
            while (sd_bus_message_read(m, "s", &v) > 0) { /* discard */ }
            sd_bus_message_exit_container(m);
            sd_bus_message_exit_container(m);
        } else {
            sd_bus_message_skip(m, "v");   // not interested
        }
        sd_bus_message_exit_container(m);  // dict entry
    }
    sd_bus_message_exit_container(m);      // array
}

// ---- static trampoline ---------------------------------------------------
static int props_changed_cb(sd_bus_message* m, void* userdata, sd_bus_error*) {
    return static_cast<ShairportWatcher*>(userdata)->on_properties_changed(m);
}

ShairportWatcher::~ShairportWatcher() {
    if (slot_) sd_bus_slot_unref(slot_);
    if (bus_)  sd_bus_flush_close_unref(bus_);
}

bool ShairportWatcher::connect() {
    // AVR_DBUS_ADDRESS lets tests point the watcher at a private bus (a fake
    // shairport on a dbus-run-session bus). Unset in production -> system bus.
    int r;
    if (const char* addr = ::getenv("AVR_DBUS_ADDRESS"); addr && *addr) {
        if ((r = sd_bus_new(&bus_)) >= 0 &&
            (r = sd_bus_set_address(bus_, addr)) >= 0 &&
            (r = sd_bus_set_bus_client(bus_, 1)) >= 0)
            r = sd_bus_start(bus_);
    } else {
        r = sd_bus_open_system(&bus_);
    }
    if (r < 0) {
        logger_.warn(std::string("D-Bus: cannot open bus: ") + strerror(-r));
        if (bus_) { sd_bus_unref(bus_); bus_ = nullptr; }
        return false;
    }
    r = sd_bus_match_signal(bus_, &slot_, kBusName, kPath,
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            props_changed_cb, this);
    if (r < 0) {
        logger_.warn(std::string("D-Bus: cannot install PropertiesChanged match: ") + strerror(-r));
        sd_bus_flush_close_unref(bus_);
        bus_ = nullptr;
        return false;
    }
    logger_.info("D-Bus: watching " + std::string(kBusName) + " on the system bus");
    refresh_initial_state();   // best-effort; shairport may not be up yet
    return true;
}

int ShairportWatcher::fd() const {
    return bus_ ? sd_bus_get_fd(bus_) : -1;
}

short ShairportWatcher::poll_events() const {
    if (!bus_) return 0;
    int e = sd_bus_get_events(bus_);
    return e < 0 ? 0 : static_cast<short>(e);
}

uint64_t ShairportWatcher::timeout_usec() const {
    if (!bus_) return UINT64_MAX;
    uint64_t t = UINT64_MAX;
    sd_bus_get_timeout(bus_, &t);
    return t;
}

void ShairportWatcher::process() {
    if (!bus_) return;
    // Drain everything queued; sd_bus_process returns >0 while progress is made.
    while (sd_bus_process(bus_, nullptr) > 0) { /* keep going */ }
}

void ShairportWatcher::apply_player_state(const std::string& s) {
    if (s == player_state_) return;
    player_state_ = s;
    logger_.info("D-Bus: PlayerState -> " + s);
    if (s == "Playing")      emit(SessionSignal::Play);
    else if (s == "Paused")  emit(SessionSignal::Pause);
    else if (s == "Stopped") emit(SessionSignal::Stop);
    // "Not Available" and others: no power action.
}

void ShairportWatcher::apply_active(bool a) {
    if (a == active_) return;          // no change
    active_ = a;
    logger_.info(std::string("D-Bus: Active -> ") + (a ? "true" : "false"));
    if (a) emit(SessionSignal::SessionUp);
    else   emit(SessionSignal::SessionDown);
}

void ShairportWatcher::apply_volume(double v) {
    // Deduplicate tiny/no-op changes; VolumeSync also guards, but this keeps the
    // log and callback quiet on repeats.
    if (has_volume_ && std::abs(v - volume_) < 0.01) return;
    volume_ = v;
    has_volume_ = true;
    logger_.debug("D-Bus: Volume -> " + std::to_string(v) + " dB");
    if (on_volume_) on_volume_(v);
}

bool ShairportWatcher::set_volume_db(double db) {
    if (!bus_) return false;
    // The sender's AirPlay volume is set by CALLING RemoteControl.SetAirplayVolume(d)
    // (dB). shairport applies it and reports it to the AirPlay sender, so the
    // phone slider follows. The main-interface "Volume" property is write/init-
    // only and does NOT reflect the sender -- never use it for this.
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call_method(bus_, kBusName, kPath, kIfaceRemote,
                               "SetAirplayVolume", &err, &reply, "d", db);
    if (r < 0) {
        logger_.warn(std::string("D-Bus: SetAirplayVolume failed: ") +
                     (err.message ? err.message : strerror(-r)));
        sd_bus_error_free(&err);
        return false;
    }
    if (reply) sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    // Reflect our own write into the cache so an echoed PropertiesChanged with
    // the same value is deduplicated by apply_volume().
    volume_ = db;
    has_volume_ = true;
    return true;
}

int ShairportWatcher::on_properties_changed(sd_bus_message* m) {
    const char* iface = nullptr;
    if (sd_bus_message_read(m, "s", &iface) < 0 || !iface) return 0;
    const bool is_main   = (std::strcmp(iface, kIfaceMain) == 0);
    const bool is_remote = (std::strcmp(iface, kIfaceRemote) == 0);
    if (!is_main && !is_remote) return 0;   // Diagnostics / MPRIS etc: ignore

    if (sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}") < 0) return 0;
    while (sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv") > 0) {
        const char* key = nullptr;
        if (sd_bus_message_read(m, "s", &key) < 0 || !key) {
            sd_bus_message_exit_container(m);
            break;
        }
        const char* contents = nullptr;
        char vtype = 0;
        sd_bus_message_peek_type(m, &vtype, &contents);

        if (is_main && std::strcmp(key, "Active") == 0) {
            int v = 0;
            sd_bus_message_read(m, "v", "b", &v);
            apply_active(v != 0);
        } else if (is_remote && std::strcmp(key, "AirplayVolume") == 0) {
            double v = 0.0;
            sd_bus_message_read(m, "v", "d", &v);
            apply_volume(v);
        } else if (is_remote && std::strcmp(key, "PlayerState") == 0) {
            const char* v = nullptr;
            sd_bus_message_read(m, "v", "s", &v);
            apply_player_state(v ? v : "");
        } else if (is_remote && std::strcmp(key, "ProgressString") == 0) {
            const char* v = nullptr;
            sd_bus_message_read(m, "v", "s", &v);
            progress_ = v ? v : "";
        } else if (is_remote && std::strcmp(key, "Metadata") == 0) {
            sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, "a{sv}");
            parse_metadata(m, title_, artist_, album_);
            sd_bus_message_exit_container(m);
            logger_.info("D-Bus: now playing -- " + artist_ + " / " + title_);
        } else {
            sd_bus_message_skip(m, "v");
        }
        sd_bus_message_exit_container(m);  // dict entry
    }
    sd_bus_message_exit_container(m);      // array
    return 0;
}

void ShairportWatcher::refresh_initial_state() {
    if (!bus_) return;
    sd_bus_error err = SD_BUS_ERROR_NULL;

    // Active
    int active = 0;
    if (sd_bus_get_property_trivial(bus_, kBusName, kPath, kIfaceMain,
                                    "Active", &err, 'b', &active) >= 0) {
        active_ = (active != 0);
    }
    sd_bus_error_free(&err);

    // Volume: the sender's AirPlay volume is on RemoteControl.AirplayVolume
    // (populated from the 'pvol' metadata; it tracks the sender even under
    // ignore_volume_control=yes). The main-interface "Volume" is write/init-only.
    double vol = 0.0;
    if (sd_bus_get_property_trivial(bus_, kBusName, kPath, kIfaceRemote,
                                    "AirplayVolume", &err, 'd', &vol) >= 0) {
        volume_ = vol; has_volume_ = true;
    }
    sd_bus_error_free(&err);

    // PlayerState
    char* ps = nullptr;
    if (sd_bus_get_property_string(bus_, kBusName, kPath, kIfaceRemote,
                                   "PlayerState", &err, &ps) >= 0 && ps) {
        player_state_ = ps;
    }
    free(ps);
    sd_bus_error_free(&err);

    // Metadata
    sd_bus_message* reply = nullptr;
    if (sd_bus_get_property(bus_, kBusName, kPath, kIfaceRemote,
                            "Metadata", &err, &reply, "a{sv}") >= 0 && reply) {
        parse_metadata(reply, title_, artist_, album_);
    }
    if (reply) sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    logger_.info("D-Bus: initial state active=" + std::string(active_ ? "1" : "0") +
                 " player=" + (player_state_.empty() ? "?" : player_state_) +
                 " now-playing=" + artist_ + " / " + title_);
}

static std::string sanitize_field(std::string s) {
    for (char& c : s) if (c == '|' || c == '\n' || c == '\r') c = ' ';
    return s;
}

std::string ShairportWatcher::snapshot() const {
    return "title=" + sanitize_field(title_) +
           "|artist=" + sanitize_field(artist_) +
           "|album=" + sanitize_field(album_) +
           "|state=" + (player_state_.empty() ? "Unknown" : player_state_) +
           "|active=" + (active_ ? "1" : "0") +
           "|progress=" + sanitize_field(progress_);
}

} // namespace AlsaAVR
