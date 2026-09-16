// avr-cec-relay.cpp -- relay HDMI-CEC volume to an AVR over the avr-controld socket.
//
// The Pi presents itself on the HDMI-CEC bus as an Audio System (logical
// address 5) that advertises the ARC-Rx feature and initiates ARC with the TV.
// Sources on the bus (e.g. an Nvidia Shield, or the TV's own remote) then send
// <User Control Pressed> [Volume Up/Down/Mute] straight to logical address 5.
// We translate those into VOLUP / VOLDOWN / SET mute commands to avr-controld,
// which owns the real receiver connection (e.g. the NAD over RS232). Audio
// itself does NOT flow over this path -- it reaches the receiver over Toslink;
// this relay carries only the volume-control intent.
//
// PROVEN on StueTaerten: a TCL/Roku TV does not implement the formal System
// Audio Mode handshake (it FEATURE_ABORTs 0x70), but the Shield still addresses
// volume User Control Pressed to LA5 once we are present as an ARC audio system.
//
// Safety: every relayed volume-up is gated by a hard ceiling (max_volume). Even
// though a human is pressing the button, we never let the relay walk the
// receiver above the configured ceiling.
//
// Config: -o key=value on the command line, merged over /etc/avr-cec-relay.conf.
//   cec_device   CEC char device            (default /dev/cec0)
//   avr_type     avr-controld backend type  (default nad_serial)
//   avr_device   receiver device path       (default /dev/ttyUSB0)
//   avr_socket   explicit socket path       (default: derived from type+device)
//   max_volume   hard volume ceiling 0-100  (default 80)
//   osd_name     CEC OSD name               (default PiAudio)
//   tv_addr      TV logical address         (default 0)

#include "../../include/avr_protocol.hpp"

#include <linux/cec.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <map>
#include <fstream>
#include <sstream>

using AlsaAVR::avr_socket_path;

// ---- tiny logger --------------------------------------------------------
static void logline(const char* level, const std::string& m) {
    char ts[32];
    time_t t = time(nullptr);
    struct tm tm; localtime_r(&t, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "%s %s avr-cec-relay: %s\n", ts, level, m.c_str());
    fflush(stderr);
}
#define LOGI(m) logline("INFO", (m))
#define LOGW(m) logline("WARN", (m))
#define LOGE(m) logline("ERR ", (m))

// ---- daemon socket client ----------------------------------------------
class AvrClient {
public:
    explicit AvrClient(std::string path) : path_(std::move(path)) {}
    ~AvrClient() { if (fd_ >= 0) ::close(fd_); }

    bool ensure() {
        if (fd_ >= 0) return true;
        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        struct sockaddr_un a;
        memset(&a, 0, sizeof(a));
        a.sun_family = AF_UNIX;
        strncpy(a.sun_path, path_.c_str(), sizeof(a.sun_path) - 1);
        if (::connect(fd_, (struct sockaddr*)&a, sizeof(a)) != 0) {
            ::close(fd_); fd_ = -1;
            return false;
        }
        return true;
    }

    // Send one command line, return the single reply line (without newline).
    // On any I/O error the connection is dropped and one reconnect is tried.
    std::string cmd(const std::string& line) {
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensure()) return "";
            std::string out = line;
            out += "\n";
            if (::write(fd_, out.data(), out.size()) < 0) { drop(); continue; }
            std::string reply;
            char buf[512];
            while (reply.find('\n') == std::string::npos) {
                ssize_t r = ::read(fd_, buf, sizeof(buf));
                if (r <= 0) { drop(); reply.clear(); break; }
                reply.append(buf, r);
            }
            auto nl = reply.find('\n');
            if (nl == std::string::npos) continue;      // dropped mid-read; retry
            reply.erase(nl);
            if (!reply.empty() && reply.back() == '\r') reply.pop_back();
            return reply;
        }
        return "";
    }

    // "GET volume" -> integer, or <0 if unknown/unavailable.
    int get_int(const std::string& line) {
        std::string r = cmd(line);
        if (r.rfind("OK ", 0) != 0) return -1;
        std::string v = r.substr(3);
        if (v == "-" || v.empty()) return -1;
        return atoi(v.c_str());
    }

private:
    void drop() { if (fd_ >= 0) { ::close(fd_); fd_ = -1; } }
    std::string path_;
    int fd_ = -1;
};

// ---- CEC helpers --------------------------------------------------------
static bool cec_send(int fd, __u8 from, __u8 to, const __u8* payload, size_t n) {
    struct cec_msg msg;
    cec_msg_init(&msg, from, to);
    for (size_t i = 0; i < n; ++i) msg.msg[1 + i] = payload[i];
    msg.len = 1 + n;
    if (ioctl(fd, CEC_TRANSMIT, &msg) != 0) return false;
    return true;
}

int main(int argc, char** argv) {
    std::map<std::string, std::string> cfg;

    // Defaults merged from the config file first, overridden by -o on the CLI.
    {
        std::ifstream f("/etc/avr-cec-relay.conf");
        std::string line;
        while (std::getline(f, line)) {
            auto h = line.find('#'); if (h != std::string::npos) line.erase(h);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s){
                size_t a = s.find_first_not_of(" \t\r\n");
                size_t b = s.find_last_not_of(" \t\r\n");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
    }
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) {
            std::string kv = argv[++i];
            auto eq = kv.find('=');
            if (eq != std::string::npos) cfg[kv.substr(0, eq)] = kv.substr(eq + 1);
        }
    }
    auto get = [&](const char* k, const char* dflt) {
        auto it = cfg.find(k); return it == cfg.end() || it->second.empty() ? std::string(dflt) : it->second;
    };

    std::string cec_dev  = get("cec_device", "/dev/cec0");
    std::string avr_type = get("avr_type", "nad_serial");
    std::string avr_dev  = get("avr_device", "/dev/ttyUSB0");
    std::string osd_name = get("osd_name", "PiAudio");
    int max_volume = atoi(get("max_volume", "80").c_str());
    if (max_volume < 0) max_volume = 0; else if (max_volume > 100) max_volume = 100;
    __u8 tv_addr = (__u8)atoi(get("tv_addr", "0").c_str());
    // The receiver input the TV feeds (e.g. the NAD optical the TV Toslink is on).
    // When the TV/setup is active we tell the daemon to power on and select it.
    std::string cec_input = get("cec_input", "");
    int tv_poll_ms = atoi(get("tv_poll_secs", "10").c_str()) * 1000;
    if (tv_poll_ms < 2000) tv_poll_ms = 10000;

    std::string sockpath = get("avr_socket", "");
    if (sockpath.empty()) sockpath = avr_socket_path(avr_type, {{"device", avr_dev}});

    LOGI("cec=" + cec_dev + " avr=" + avr_type + "(" + avr_dev + ") socket=" + sockpath +
         " max_volume=" + std::to_string(max_volume) + " osd=" + osd_name);

    AvrClient avr(sockpath);

    int fd = ::open(cec_dev.c_str(), O_RDWR);
    if (fd < 0) { LOGE("cannot open " + cec_dev + ": " + strerror(errno)); return 1; }

    // Claim the Audio System logical address advertising the ARC-Rx feature.
    // The empty Device Features is exactly what made a Roku TV send nothing;
    // CEC_OP_FEAT_DEV_SOURCE_HAS_ARC_RX is what turns us into a real audio sink.
    // Initiator so we can transmit AND so CEC_ADAP_S_LOG_ADDRS is permitted
    // (it returns -EBUSY unless the fd is in initiator mode); follower so the
    // kernel hands us the messages it does not auto-answer (User Control
    // Pressed, audio status, ARC). This MUST precede S_LOG_ADDRS.
    __u32 mode = CEC_MODE_INITIATOR | CEC_MODE_FOLLOWER;
    if (ioctl(fd, CEC_S_MODE, &mode) != 0) LOGW("CEC_S_MODE failed: " + std::string(strerror(errno)));

    struct cec_log_addrs want;
    memset(&want, 0, sizeof(want));
    want.cec_version = CEC_OP_CEC_VERSION_2_0;
    want.num_log_addrs = 1;
    want.vendor_id = CEC_VENDOR_ID_NONE;
    want.flags = CEC_LOG_ADDRS_FL_ALLOW_RC_PASSTHRU;
    snprintf(want.osd_name, sizeof(want.osd_name), "%s", osd_name.c_str());
    want.primary_device_type[0] = CEC_OP_PRIM_DEVTYPE_AUDIOSYSTEM;
    want.log_addr_type[0]       = CEC_LOG_ADDR_TYPE_AUDIOSYSTEM;
    want.all_device_types[0]    = CEC_OP_ALL_DEVTYPE_AUDIOSYSTEM;
    want.features[0][0]         = CEC_OP_FEAT_DEV_SOURCE_HAS_ARC_RX;

    // Use the adapter as-is if it is already an audio system (e.g. configured by
    // boot/cec-ctl); otherwise claim it. S_LOG_ADDRS needs a valid physical
    // address (live HDMI link), so retry until the TV/EDID is up.
    __u8 my_la = 0xff;
    for (int tries = 0; ; ++tries) {
        struct cec_log_addrs cur;
        memset(&cur, 0, sizeof(cur));
        bool have = (ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &cur) == 0 && cur.num_log_addrs > 0 &&
                     cur.log_addr[0] != CEC_LOG_ADDR_INVALID &&
                     cur.log_addr_type[0] == CEC_LOG_ADDR_TYPE_AUDIOSYSTEM);
        if (!have) {
            if (ioctl(fd, CEC_ADAP_S_LOG_ADDRS, &want) == 0 &&
                ioctl(fd, CEC_ADAP_G_LOG_ADDRS, &cur) == 0 && cur.num_log_addrs > 0 &&
                cur.log_addr[0] != CEC_LOG_ADDR_INVALID)
                have = true;
        }
        if (have) { my_la = cur.log_addr[0]; break; }
        if (tries == 0) LOGW("waiting for CEC logical address (is HDMI connected?): " +
                             std::string(strerror(errno)));
        sleep(3);
    }
    LOGI("claimed logical address " + std::to_string((int)my_la) + " (Audio System)");

    // Kick off ARC so the TV marks the link active and routes volume to us.
    const __u8 initiate_arc[] = {CEC_MSG_INITIATE_ARC};
    if (cec_send(fd, my_la, tv_addr, initiate_arc, 1))
        LOGI("sent Initiate ARC to TV");

    // Locally tracked audio status we report back on <Give Audio Status>.
    auto report_audio_status = [&](__u8 to) {
        int vol = avr.get_int("GET volume");           // 0-100 or -1
        std::string mr = avr.cmd("GET mute");
        bool muted = (mr == "OK 1");
        __u8 status = (vol < 0 || vol > 100) ? 0x7f : (__u8)vol;   // 0x7f = unknown
        if (muted) status |= CEC_OP_AUD_MUTE_STATUS_ON << 7;
        const __u8 p[] = {CEC_MSG_REPORT_AUDIO_STATUS, status};
        cec_send(fd, my_la, to, p, 2);
    };

    // Track whether the TV/setup is on. When it is, the receiver is needed
    // (tell the daemon to power on + select the TV input); when it goes to
    // standby, release the demand. The daemon's power policy arbitrates with
    // other demands (e.g. AirPlay) and powers off only when nothing needs it.
    // No Home Assistant involved.
    int tv_on = -1;   // -1 unknown, 0 off, 1 on
    auto set_tv = [&](int on) {
        if (on == tv_on) return;
        tv_on = on;
        if (on) {
            std::string c = "DEMAND cec 1";
            if (!cec_input.empty()) c += " " + cec_input;
            avr.cmd(c);
            LOGI("TV active -> receiver demand ON" +
                 (cec_input.empty() ? std::string() : " (input " + cec_input + ")"));
        } else {
            avr.cmd("DEMAND cec 0");
            LOGI("TV standby -> receiver demand OFF");
        }
    };
    const __u8 give_power[] = {CEC_MSG_GIVE_DEVICE_POWER_STATUS};
    cec_send(fd, my_la, tv_addr, give_power, 1);   // initial TV power query

    struct pollfd pfd { fd, POLLIN, 0 };
    LOGI("relay running");

    while (true) {
        int n = poll(&pfd, 1, tv_poll_ms);
        if (n < 0) { if (errno == EINTR) continue; break; }
        if (n == 0) {
            cec_send(fd, my_la, tv_addr, give_power, 1);   // periodic TV power poll
            // Re-assert the demand while the TV is on so the daemon recovers the
            // state after a restart. The daemon acts on the activation edge only,
            // so a re-assertion won't re-select the input or fight another source.
            if (tv_on == 1) {
                std::string c = "DEMAND cec 1";
                if (!cec_input.empty()) c += " " + cec_input;
                avr.cmd(c);
            }
            continue;
        }
        if (!(pfd.revents & POLLIN)) continue;

        struct cec_msg msg;
        memset(&msg, 0, sizeof(msg));
        if (ioctl(fd, CEC_RECEIVE, &msg) != 0) continue;
        if (msg.len < 2) continue;                     // poll/ping, no opcode

        __u8 initiator = cec_msg_initiator(&msg);
        __u8 opcode    = msg.msg[1];

        switch (opcode) {
        case CEC_MSG_USER_CONTROL_PRESSED: {
            set_tv(1);                                  // remote activity = setup in use
            if (msg.len < 3) break;
            __u8 ui = msg.msg[2];
            if (ui == CEC_OP_UI_CMD_VOLUME_UP) {
                int vol = avr.get_int("GET volume");
                if (vol >= 0 && vol >= max_volume) {
                    LOGW("volume-up ignored: at ceiling " + std::to_string(vol) +
                         ">=" + std::to_string(max_volume));
                } else {
                    avr.cmd("VOLUP");
                }
            } else if (ui == CEC_OP_UI_CMD_VOLUME_DOWN) {
                avr.cmd("VOLDOWN");
            } else if (ui == CEC_OP_UI_CMD_MUTE || ui == CEC_OP_UI_CMD_MUTE_FUNCTION) {
                std::string mr = avr.cmd("GET mute");
                bool muted = (mr == "OK 1");
                avr.cmd(std::string("SET mute ") + (muted ? "0" : "1"));
            }
            break;
        }
        case CEC_MSG_GIVE_AUDIO_STATUS:
            report_audio_status(initiator);
            break;
        case CEC_MSG_GIVE_SYSTEM_AUDIO_MODE_STATUS: {
            const __u8 p[] = {CEC_MSG_SYSTEM_AUDIO_MODE_STATUS, CEC_OP_SYS_AUD_STATUS_ON};
            cec_send(fd, my_la, initiator, p, 2);
            break;
        }
        case CEC_MSG_REQUEST_ARC_INITIATION:
            cec_send(fd, my_la, initiator, initiate_arc, 1);
            LOGI("ARC initiation requested -> Initiate ARC");
            break;
        case CEC_MSG_REPORT_ARC_INITIATED:
            LOGI("ARC initiated by TV");
            break;

        // ---- TV/setup power tracking -> receiver power demand ----------
        case CEC_MSG_REPORT_POWER_STATUS:
            if (initiator == tv_addr && msg.len >= 3)
                set_tv(msg.msg[2] == CEC_OP_POWER_STATUS_ON ? 1 : 0);
            break;
        case CEC_MSG_STANDBY:
            set_tv(0);                                  // TV/all going to standby
            break;
        case CEC_MSG_ACTIVE_SOURCE:
        case CEC_MSG_SET_STREAM_PATH:
        case CEC_MSG_ROUTING_CHANGE:
        case CEC_MSG_IMAGE_VIEW_ON:
        case CEC_MSG_TEXT_VIEW_ON:
            set_tv(1);                                  // a source became active
            break;
        case CEC_MSG_SET_SYSTEM_AUDIO_MODE:
            if (msg.len >= 3 && msg.msg[2] != 0) set_tv(1);
            break;

        default:
            break;                                     // let the kernel handle the rest
        }
    }

    ::close(fd);
    return 0;
}
