// SocketClientBackend: a Backend that forwards every call to avr-controld over a
// Unix socket. If no daemon is listening it auto-spawns one and waits for it.
//
// It also provides the create_<type>_backend() factory symbols the ALSA plugins
// already call, so linking this file into a plugin (instead of the real backend)
// transparently turns that plugin into a daemon client -- no plugin code change.

#include "../../include/backend.hpp"
#include "../../include/avr_protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <optional>

namespace AlsaAVR {

// speaker <-> name (kept local to avoid coupling the protocol header to backend.hpp)
static const std::pair<Speaker, const char*> kSpk[] = {
    {Speaker::FRONT_LEFT, "FRONT_LEFT"}, {Speaker::FRONT_RIGHT, "FRONT_RIGHT"},
    {Speaker::CENTER, "CENTER"}, {Speaker::SUBWOOFER, "SUBWOOFER"},
    {Speaker::SURROUND_LEFT, "SURROUND_LEFT"}, {Speaker::SURROUND_RIGHT, "SURROUND_RIGHT"},
    {Speaker::SURROUND_BACK_LEFT, "SURROUND_BACK_LEFT"}, {Speaker::SURROUND_BACK_RIGHT, "SURROUND_BACK_RIGHT"},
    {Speaker::SURROUND_BACK, "SURROUND_BACK"},
    {Speaker::FRONT_HEIGHT_LEFT, "FRONT_HEIGHT_LEFT"}, {Speaker::FRONT_HEIGHT_RIGHT, "FRONT_HEIGHT_RIGHT"},
};
static const char* spk_name(Speaker s) { for (auto& p : kSpk) if (p.first == s) return p.second; return "FRONT_LEFT"; }
static std::optional<Speaker> spk_from(const std::string& s) { for (auto& p : kSpk) if (s == p.second) return p.first; return std::nullopt; }

class SocketClientBackend : public Backend {
public:
    explicit SocketClientBackend(std::string type) : type_(std::move(type)) {}
    ~SocketClientBackend() override { close(); }

    bool init(const std::map<std::string, std::string>& config) override {
        config_ = config;
        sockpath_ = avr_socket_path(type_, config_);
        if (!connect_or_spawn()) return false;

        // Cache the immutable bits up front.
        parse_caps(request("CAPS"));
        auto hw = ok_payload(request("HWMAX"));
        hwmax_ = hw ? atoi(hw->c_str()) : 98;
        name_ = ok_payload(request("NAME")).value_or(type_);
        inputs_ = split(ok_payload(request("INPUTS")).value_or(""));
        for (auto& s : split(ok_payload(request("SPEAKERS")).value_or("")))
            if (auto sp = spk_from(s)) speakers_.push_back(*sp);
        auto mn = ok_payload(request("SPKMIN")); spk_min_ = mn ? atoi(mn->c_str()) : 38;
        auto mx = ok_payload(request("SPKMAX")); spk_max_ = mx ? atoi(mx->c_str()) : 62;
        return true;
    }

    void close() override { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }

    BackendCaps capabilities() const override { return caps_; }

    bool set_volume_absolute(int level) override { return req_ok("SET volume " + std::to_string(level)); }
    bool volume_up() override   { return req_ok("VOLUP"); }
    bool volume_down() override { return req_ok("VOLDOWN"); }
    std::optional<int> get_volume() override { return get_int("GET volume"); }

    bool set_mute(bool m) override { return req_ok(std::string("SET mute ") + (m ? "1" : "0")); }
    std::optional<bool> get_mute() override { return get_bool("GET mute"); }

    bool set_power(bool on) override { return req_ok(std::string("SET power ") + (on ? "1" : "0")); }
    std::optional<bool> get_power() override { return get_bool("GET power"); }

    bool set_standby_timeout(bool e) override { return req_ok(std::string("SET standby ") + (e ? "1" : "0")); }
    std::optional<bool> get_standby_timeout() override { return get_bool("GET standby"); }

    std::vector<std::string> get_available_inputs() override { return inputs_; }
    bool set_input(const std::string& in) override { return req_ok("SET input " + in); }
    std::optional<std::string> get_current_input() override {
        auto p = ok_payload(request("GET input"));
        if (!p || *p == "-") return std::nullopt;
        return *p;
    }

    std::vector<Speaker> get_available_speakers() override { return speakers_; }
    bool set_speaker_level(Speaker s, int level) override {
        return req_ok("SET speaker " + std::string(spk_name(s)) + " " + std::to_string(level));
    }
    int get_speaker_level(Speaker s) override {
        auto v = get_int("GET speaker " + std::string(spk_name(s)));
        return v.value_or(50);
    }
    int get_speaker_level_min() override { return spk_min_; }
    int get_speaker_level_max() override { return spk_max_; }

    std::string name() const override { return name_; }
    int get_hardware_max_volume() const override { return hwmax_; }

    bool is_connected() const override {
        auto p = const_cast<SocketClientBackend*>(this)->ok_payload(
                     const_cast<SocketClientBackend*>(this)->request("CONNECTED"));
        return p && *p == "1";
    }
    bool reconnect() override { close(); return connect_or_spawn(); }

private:
    std::string type_;
    std::map<std::string, std::string> config_;
    std::string sockpath_;
    int fd_ = -1;

    BackendCaps caps_{};
    int hwmax_ = 98, spk_min_ = 38, spk_max_ = 62;
    std::string name_;
    std::vector<std::string> inputs_;
    std::vector<Speaker> speakers_;

    static std::vector<std::string> split(const std::string& s) {
        std::vector<std::string> out;
        if (s.empty()) return out;
        std::stringstream ss(s); std::string t;
        while (std::getline(ss, t, ',')) if (!t.empty()) out.push_back(t);
        return out;
    }

    bool try_connect() {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return false;
        struct sockaddr_un a; memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, sockpath_.c_str(), sizeof(a.sun_path) - 1);
        if (connect(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { ::close(fd); return false; }
        fd_ = fd;
        return true;
    }

    void spawn_daemon() {
        // Build argv BEFORE fork (no malloc in the child before exec).
        std::vector<std::string> args = {"avr-controld", "--type", type_, "--socket", sockpath_};
        for (auto& kv : config_) { args.push_back("-o"); args.push_back(kv.first + "=" + kv.second); }
        std::vector<char*> argv;
        for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid < 0) return;
        if (pid == 0) {
            execvp("avr-controld", argv.data());
            execv("/usr/bin/avr-controld", argv.data());
            _exit(127);
        }
        // avr-controld self-daemonizes, so our direct child exits promptly; reap it.
        int st; waitpid(pid, &st, 0);
    }

    bool connect_or_spawn() {
        if (try_connect()) return true;
        spawn_daemon();
        for (int i = 0; i < 120; ++i) {          // up to ~6s for backend init
            usleep(50 * 1000);
            if (try_connect()) return true;
        }
        return false;
    }

    // One request/reply round-trip; one reconnect retry on I/O error.
    std::string request(const std::string& cmd) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (fd_ < 0 && !connect_or_spawn()) return "ERR nodaemon";
            std::string line = cmd + "\n";
            if (write(fd_, line.data(), line.size()) < 0) { close(); continue; }
            std::string resp; char c;
            bool got = false;
            while (true) {
                ssize_t r = read(fd_, &c, 1);
                if (r <= 0) { close(); break; }
                if (c == '\n') { got = true; break; }
                resp += c;
            }
            if (got) return resp;
        }
        return "ERR ioerror";
    }

    std::optional<std::string> ok_payload(const std::string& resp) {
        if (resp.rfind("OK", 0) == 0) {
            if (resp.size() >= 3 && resp[2] == ' ') return resp.substr(3);
            return std::string("");
        }
        return std::nullopt;
    }
    bool req_ok(const std::string& cmd) { return ok_payload(request(cmd)).has_value(); }
    std::optional<int> get_int(const std::string& cmd) {
        auto p = ok_payload(request(cmd));
        if (!p || *p == "-" || p->empty()) return std::nullopt;
        return atoi(p->c_str());
    }
    std::optional<bool> get_bool(const std::string& cmd) {
        auto p = ok_payload(request(cmd));
        if (!p || *p == "-" || p->empty()) return std::nullopt;
        return *p == "1";
    }
    void parse_caps(const std::string& resp) {
        auto p = ok_payload(resp);
        caps_ = BackendCaps{};
        if (!p) return;
        std::stringstream ss(*p); std::string tok;
        while (ss >> tok) {
            auto eq = tok.find('=');
            if (eq == std::string::npos) continue;
            std::string k = tok.substr(0, eq); int v = atoi(tok.substr(eq + 1).c_str());
            if (k == "abs") caps_.supports_absolute = v;
            else if (k == "query") caps_.supports_query = v;
            else if (k == "mute") caps_.supports_mute = v;
            else if (k == "input") caps_.supports_input_select = v;
            else if (k == "spk") caps_.supports_speaker_level = v;
            else if (k == "standby") caps_.supports_standby_timeout = v;
            else if (k == "minv") caps_.min_volume = v;
            else if (k == "maxv") caps_.max_volume = v;
        }
    }
};

static std::unique_ptr<Backend> make_client(const std::string& type) {
    return std::make_unique<SocketClientBackend>(type);
}

// Factory symbols the plugins already call -- now they yield daemon clients.
std::unique_ptr<Backend> create_denon_ip_backend()      { return make_client("denon_ip"); }
std::unique_ptr<Backend> create_opalum_serial_backend() { return make_client("opalum_serial"); }
std::unique_ptr<Backend> create_nad_serial_backend()    { return make_client("nad_serial"); }
std::unique_ptr<Backend> create_cec_backend()           { return make_client("cec"); }

} // namespace AlsaAVR
