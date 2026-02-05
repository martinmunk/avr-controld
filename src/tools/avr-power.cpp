// avr-power.cpp - AVR power control via ALSA with state save/restore
// Compile: g++ -o avr-power avr-power.cpp -lasound
//
// Usage:
//   avr-power on [device]      - Save current state, then power on
//   avr-power off [device]     - Power off
//   avr-power restore [device] - Restore saved input state
//
// Symlink usage (like busybox):
//   ln -s avr-power avr-power-on
//   ln -s avr-power avr-power-off
//   ln -s avr-power avr-power-restore
//
// State is saved to /tmp/avr-state-<device-sanitized>.conf

#include <alsa/asoundlib.h>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <optional>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

constexpr const char* STATE_DIR = "/tmp";

struct AVRState {
    bool power_was_on = false;
    bool power_was_unknown = false;
    unsigned int input_index = 0;
    std::string input_name;
    bool input_was_unknown = false;
};

class AVRPowerControl {
public:
    explicit AVRPowerControl(const std::string& device) : device_(device) {}

    bool open() {
        int err = snd_ctl_open(&ctl_, device_.c_str(), 0);
        if (err < 0) {
            std::cerr << "Cannot open control " << device_ << ": " << snd_strerror(err) << "\n";
            return false;
        }
        return true;
    }

    ~AVRPowerControl() {
        if (ctl_) {
            snd_ctl_close(ctl_);
        }
    }

    // Non-copyable
    AVRPowerControl(const AVRPowerControl&) = delete;
    AVRPowerControl& operator=(const AVRPowerControl&) = delete;

    bool powerOn() {
        // 1. Query current state
        bool power_unknown = false;
        bool input_unknown = false;

        auto power = readPower(power_unknown);
        auto input = readInputSource(input_unknown);
        std::string input_name;
        if (input) {
            input_name = readInputName(*input).value_or("");
        }

        // 2. Save state (include whether values were unknown)
        AVRState state;
        state.power_was_on = power.value_or(false);  // Default to "off" if unknown
        state.power_was_unknown = power_unknown;
        state.input_index = input.value_or(0);
        state.input_name = input_name;
        state.input_was_unknown = input_unknown;
        saveState(state);

        // 3. Power on
        if (!writePower(true)) {
            std::cerr << "Cannot set power on\n";
            return false;
        }

        std::cout << "Power ON: " << device_ << "\n";

        // Report saved state
        std::cout << "  Saved state: power=";
        if (power_unknown) {
            std::cout << "unknown (treating as off)";
        } else {
            std::cout << (*power ? "on" : "off");
        }

        if (input_unknown) {
            std::cout << ", input=unknown\n";
        } else if (input) {
            std::cout << ", input=" << *input << " (" << input_name << ")\n";
        } else {
            std::cout << "\n";
        }

        return true;
    }

    bool powerOff() {
        if (!writePower(false)) {
            std::cerr << "Cannot set power off\n";
            return false;
        }

        std::cout << "Power OFF: " << device_ << "\n";
        return true;
    }

    bool restore() {
        // 1. Load saved state
        auto state = loadState();
        if (!state) {
            std::cerr << "No saved state found for " << device_ << "\n";
            return false;
        }

        bool success = true;

        // 2. Restore input if we have a saved state (and it wasn't unknown)
        if (!state->input_was_unknown && !state->input_name.empty()) {
            if (writeInputSource(state->input_index)) {
                std::cout << "Restored input: " << state->input_index
                          << " (" << state->input_name << ")\n";
            } else {
                std::cerr << "Warning: Could not restore input\n";
                success = false;
            }
        } else if (state->input_was_unknown) {
            std::cout << "Input was unknown, not restoring\n";
        }

        // 3. Power off if device was off before (or unknown - treat as off for safety)
        if (!state->power_was_on) {
            if (writePower(false)) {
                if (state->power_was_unknown) {
                    std::cout << "Power OFF (was unknown, treating as off): " << device_ << "\n";
                } else {
                    std::cout << "Power OFF (was off before): " << device_ << "\n";
                }
            } else {
                std::cerr << "Warning: Could not power off\n";
                success = false;
            }
        } else {
            std::cout << "Power was already on, leaving on: " << device_ << "\n";
        }

        // 4. Delete state file
        deleteState();

        return success;
    }

private:
    std::string device_;
    snd_ctl_t* ctl_ = nullptr;

    std::string sanitizeForFilename(const std::string& input) const {
        std::string result;
        result.reserve(input.size());
        for (char c : input) {
            if (std::isalnum(c) || c == '-' || c == '_') {
                result += c;
            } else if (c == ':' || c == '/' || c == '.') {
                result += '_';
            }
        }
        return result;
    }

    fs::path getStateFilePath() const {
        return fs::path(STATE_DIR) / ("avr-state-" + sanitizeForFilename(device_) + ".conf");
    }

    // Returns: value, or nullopt if not supported, or nullopt with errno=EAGAIN if unknown
    std::optional<bool> readPower(bool& state_unknown) {
        state_unknown = false;
        snd_ctl_elem_id_t* id;
        snd_ctl_elem_value_t* value;

        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "Master Playback Power");

        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);

        int err = snd_ctl_elem_read(ctl_, value);
        if (err < 0) {
            if (err == -EAGAIN) {
                state_unknown = true;  // State not yet set through plugin
            }
            return std::nullopt;
        }

        return snd_ctl_elem_value_get_boolean(value, 0) != 0;
    }

    bool writePower(bool on) {
        snd_ctl_elem_id_t* id;
        snd_ctl_elem_value_t* value;

        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "Master Playback Power");

        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);
        snd_ctl_elem_value_set_boolean(value, 0, on ? 1 : 0);

        return snd_ctl_elem_write(ctl_, value) >= 0;
    }

    std::optional<unsigned int> readInputSource(bool& state_unknown) {
        state_unknown = false;
        snd_ctl_elem_id_t* id;
        snd_ctl_elem_value_t* value;

        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "Input Source");

        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);

        int err = snd_ctl_elem_read(ctl_, value);
        if (err < 0) {
            if (err == -EAGAIN) {
                state_unknown = true;  // State not yet set through plugin
            }
            return std::nullopt;
        }

        return snd_ctl_elem_value_get_enumerated(value, 0);
    }

    bool writeInputSource(unsigned int index) {
        snd_ctl_elem_id_t* id;
        snd_ctl_elem_value_t* value;

        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "Input Source");

        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);
        snd_ctl_elem_value_set_enumerated(value, 0, index);

        return snd_ctl_elem_write(ctl_, value) >= 0;
    }

    std::optional<std::string> readInputName(unsigned int index) {
        snd_ctl_elem_info_t* info;
        snd_ctl_elem_id_t* id;

        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "Input Source");

        snd_ctl_elem_info_alloca(&info);
        snd_ctl_elem_info_set_id(info, id);

        if (snd_ctl_elem_info(ctl_, info) < 0) {
            return std::nullopt;
        }

        if (index >= snd_ctl_elem_info_get_items(info)) {
            return std::nullopt;
        }

        snd_ctl_elem_info_set_item(info, index);
        if (snd_ctl_elem_info(ctl_, info) < 0) {
            return std::nullopt;
        }

        return std::string(snd_ctl_elem_info_get_item_name(info));
    }

    void saveState(const AVRState& state) {
        auto path = getStateFilePath();
        std::ofstream f(path);
        if (!f) {
            std::cerr << "Warning: Cannot save state to " << path << "\n";
            return;
        }

        f << "power_was_on=" << (state.power_was_on ? 1 : 0) << "\n";
        f << "power_was_unknown=" << (state.power_was_unknown ? 1 : 0) << "\n";
        f << "input_index=" << state.input_index << "\n";
        f << "input_name=" << state.input_name << "\n";
        f << "input_was_unknown=" << (state.input_was_unknown ? 1 : 0) << "\n";
    }

    std::optional<AVRState> loadState() {
        auto path = getStateFilePath();
        std::ifstream f(path);
        if (!f) {
            return std::nullopt;
        }

        AVRState state;
        std::string line;

        while (std::getline(f, line)) {
            if (line.starts_with("power_was_on=")) {
                state.power_was_on = (line.substr(13) == "1");
            } else if (line.starts_with("power_was_unknown=")) {
                state.power_was_unknown = (line.substr(18) == "1");
            } else if (line.starts_with("input_index=")) {
                state.input_index = std::stoul(line.substr(12));
            } else if (line.starts_with("input_name=")) {
                state.input_name = line.substr(11);
            } else if (line.starts_with("input_was_unknown=")) {
                state.input_was_unknown = (line.substr(18) == "1");
            }
        }

        return state;
    }

    void deleteState() {
        std::error_code ec;
        fs::remove(getStateFilePath(), ec);
    }
};

enum class Command { On, Off, Restore };

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " on|off|restore [device]\n";
    std::cerr << "       Or symlink as avr-power-on / avr-power-off / avr-power-restore\n";
    std::cerr << "\n";
    std::cerr << "  on      - Save current state (power, input), then power on\n";
    std::cerr << "  off     - Power off\n";
    std::cerr << "  restore - Restore saved input, power off if was off before\n";
    std::cerr << "  device  - ALSA control device (default: 'default')\n";
}

int main(int argc, char* argv[]) {
    std::string device = "default";
    std::optional<Command> cmd;

    // Get program base name for symlink detection
    std::string prog_path = argv[0];
    std::string prog_name = fs::path(prog_path).filename().string();

    // Check if called via symlink
    if (prog_name.find("power-on") != std::string::npos) {
        cmd = Command::On;
        device = argc > 1 ? argv[1] : "default";
    } else if (prog_name.find("power-off") != std::string::npos) {
        cmd = Command::Off;
        device = argc > 1 ? argv[1] : "default";
    } else if (prog_name.find("power-restore") != std::string::npos) {
        cmd = Command::Restore;
        device = argc > 1 ? argv[1] : "default";
    } else {
        // Not symlinked, require subcommand argument
        if (argc < 2) {
            printUsage(prog_name.c_str());
            return 1;
        }

        std::string arg = argv[1];
        if (arg == "on") {
            cmd = Command::On;
        } else if (arg == "off") {
            cmd = Command::Off;
        } else if (arg == "restore") {
            cmd = Command::Restore;
        } else {
            printUsage(prog_name.c_str());
            return 1;
        }

        device = argc > 2 ? argv[2] : "default";
    }

    AVRPowerControl ctrl(device);
    if (!ctrl.open()) {
        return 1;
    }

    switch (*cmd) {
        case Command::On:
            return ctrl.powerOn() ? 0 : 1;
        case Command::Off:
            return ctrl.powerOff() ? 0 : 1;
        case Command::Restore:
            return ctrl.restore() ? 0 : 1;
    }

    return 1;
}
