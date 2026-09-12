#pragma once

#include <string>
#include <memory>
#include <map>
#include <vector>
#include <optional>

namespace AlsaAVR {

// Speaker identifiers for individual level control
enum class Speaker {
    FRONT_LEFT,
    FRONT_RIGHT,
    CENTER,
    SUBWOOFER,
    SURROUND_LEFT,
    SURROUND_RIGHT,
    SURROUND_BACK_LEFT,
    SURROUND_BACK_RIGHT,
    SURROUND_BACK,
    FRONT_HEIGHT_LEFT,
    FRONT_HEIGHT_RIGHT
};

// Backend capabilities
struct BackendCaps {
    bool supports_absolute;    // Can set specific volume levels
    bool supports_query;       // Can query current volume
    bool supports_mute;        // Has mute control
    bool supports_input_select; // Has input selection control
    bool supports_speaker_level; // Has individual speaker level control
    bool supports_standby_timeout = false; // Has standby timeout control
    int min_volume;           // Minimum volume level (0-100 scale)
    int max_volume;           // Maximum volume level (0-100 scale)
};

// Backend interface - all AVR control backends must implement this
class Backend {
public:
    virtual ~Backend() = default;

    // Initialize backend with configuration
    virtual bool init(const std::map<std::string, std::string>& config) = 0;

    // Close backend connection
    virtual void close() = 0;

    // Get backend capabilities
    virtual BackendCaps capabilities() const = 0;

    // Volume control (0-100 scale)
    // get_volume returns nullopt if state is unknown (not set through plugin)
    virtual bool set_volume_absolute(int level) = 0;
    virtual bool volume_up() = 0;
    virtual bool volume_down() = 0;
    virtual std::optional<int> get_volume() = 0;

    // Mute control
    // get_mute returns nullopt if state is unknown
    virtual bool set_mute(bool muted) = 0;
    virtual std::optional<bool> get_mute() = 0;

    // Power control (optional)
    // get_power returns nullopt if state is unknown
    virtual bool set_power(bool on) { return false; }
    virtual std::optional<bool> get_power() { return true; }

    // Standby timeout control (optional)
    // get_standby_timeout returns nullopt if state is unknown
    virtual bool set_standby_timeout(bool enabled) { return false; }
    virtual std::optional<bool> get_standby_timeout() { return std::nullopt; }

    // Input selection control (optional)
    // get_current_input returns nullopt if state is unknown
    virtual std::vector<std::string> get_available_inputs() { return {}; }
    virtual bool set_input(const std::string& input) { return false; }
    virtual std::optional<std::string> get_current_input() { return std::nullopt; }
    // The input this handle was configured to use (the plugin's "input"
    // parameter), if any. Exposed by the ALSA plugins as the read-only
    // "Input Source Default" element so a session manager (avr-power) can
    // switch to it after saving the input that was active before.
    virtual std::optional<std::string> configured_input() const { return std::nullopt; }

    // Speaker level control (optional)
    virtual std::vector<Speaker> get_available_speakers() { return {}; }
    virtual bool set_speaker_level(Speaker speaker, int level) { return false; }
    virtual int get_speaker_level(Speaker speaker) { return 50; }  // 50 = 0dB reference
    virtual int get_speaker_level_min() { return 38; }  // Typical: -12dB
    virtual int get_speaker_level_max() { return 62; }  // Typical: +12dB

    // Backend name for logging
    virtual std::string name() const = 0;

    // Check if backend is connected and ready
    virtual bool is_connected() const = 0;

    // Reconnect if connection lost
    virtual bool reconnect() = 0;

    // Get hardware max volume for TLV dB range calculation (optional)
    // Returns the hardware maximum volume setting detected from receiver
    // Default: 98 (standard Denon maximum)
    virtual int get_hardware_max_volume() const { return 98; }
};

// Backend factory
class BackendFactory {
public:
    static std::unique_ptr<Backend> create(const std::string& type);
    static std::vector<std::string> available_backends();
};

} // namespace AlsaAVR
