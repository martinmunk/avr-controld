#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <string>
#include <cstring>

// We need to test the protocol parsing logic from denon_ip.cpp
// Since parse_volume_response is private, we'll create a test harness

namespace AlsaAVR {
namespace Testing {

// Helper class to expose protocol parsing logic for testing
class DenonProtocolParser {
public:
    static constexpr int DENON_MAX_VOLUME = 98;
    int max_volume_ = DENON_MAX_VOLUME;

    // Reimplementation of parse_volume_response for testing
    int parse_volume_response(const std::string& response) {
        size_t pos = response.find("MV");
        if (pos == std::string::npos) return -1;

        // Check for MVMAX in response and extract max volume
        size_t mvmax_pos = response.find("MVMAX ");
        if (mvmax_pos != std::string::npos) {
            std::string max_str = response.substr(mvmax_pos + 6);
            size_t max_end = 0;
            while (max_end < max_str.length() && isdigit(max_str[max_end])) {
                max_end++;
            }
            if (max_end > 0) {
                try {
                    int detected_max = std::stoi(max_str.substr(0, max_end));
                    if (detected_max >= 1 && detected_max <= DENON_MAX_VOLUME) {
                        max_volume_ = detected_max;
                    }
                } catch (...) {
                    // Ignore parsing errors
                }
            }
        }

        std::string vol_str = response.substr(pos + 2);
        size_t end = 0;
        while (end < vol_str.length() && isdigit(vol_str[end])) {
            end++;
        }
        if (end == 0 || end > 3) return -1;

        vol_str = vol_str.substr(0, end);
        try {
            int vol_int = std::stoi(vol_str);
            float denon_vol;
            if (vol_str.length() == 2) {
                denon_vol = vol_int;
            } else {
                denon_vol = vol_int / 10.0f;
            }
            return static_cast<int>((denon_vol * 100.0f) / max_volume_ + 0.5f);
        } catch (...) {
            return -1;
        }
    }

    // Helper to generate volume command string
    std::string format_volume_command(int level, int max_volume = 98) {
        int denon_vol = (level * max_volume) / 100;
        char buffer[16];
        snprintf(buffer, sizeof(buffer), "MV%02d", denon_vol);
        return std::string(buffer);
    }

    // Parse mute response
    bool parse_mute_response(const std::string& response, bool& muted) {
        if (response.find("MUON") != std::string::npos) {
            muted = true;
            return true;
        } else if (response.find("MUOFF") != std::string::npos) {
            muted = false;
            return true;
        }
        return false;
    }

    // Parse power response
    bool parse_power_response(const std::string& response, bool& power_on) {
        if (response.find("PWON") != std::string::npos) {
            power_on = true;
            return true;
        } else if (response.find("PWSTANDBY") != std::string::npos) {
            power_on = false;
            return true;
        }
        return false;
    }

    // Parse input response
    bool parse_input_response(const std::string& response, std::string& input) {
        size_t si_pos = response.find("SI");
        if (si_pos == std::string::npos) return false;

        std::string input_code = response.substr(si_pos + 2);
        // Extract input name (until CR or whitespace)
        size_t end = input_code.find_first_of("\r\n ");
        if (end != std::string::npos) {
            input_code = input_code.substr(0, end);
        }
        if (!input_code.empty()) {
            input = input_code;
            return true;
        }
        return false;
    }
};

// Test fixture for Denon protocol parsing
class DenonProtocolTest : public ::testing::Test {
protected:
    DenonProtocolParser parser_;
};

// ============================================================================
// Volume Response Parsing Tests
// ============================================================================

TEST_F(DenonProtocolTest, ParseVolumeResponse_TwoDigits) {
    // MV45 should be parsed as volume 45 (out of 98)
    int result = parser_.parse_volume_response("MV45\r");
    EXPECT_EQ(result, 46);  // 45/98 * 100 = 45.9 -> 46
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_ThreeDigits) {
    // MV385 should be parsed as 38.5 (out of 98)
    int result = parser_.parse_volume_response("MV385\r");
    EXPECT_EQ(result, 39);  // 38.5/98 * 100 = 39.3 -> 39
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_Zero) {
    int result = parser_.parse_volume_response("MV00\r");
    EXPECT_EQ(result, 0);
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_MaxVolume) {
    int result = parser_.parse_volume_response("MV98\r");
    EXPECT_EQ(result, 100);
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_WithMVMAX) {
    // Receiver returns both volume and max volume
    int result = parser_.parse_volume_response("MV70\rMVMAX 70\r");
    EXPECT_EQ(parser_.max_volume_, 70);  // Max volume detected
    EXPECT_EQ(result, 100);  // 70/70 * 100 = 100
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_MVMAX_UpdatesScaling) {
    // First set max volume to 70
    parser_.parse_volume_response("MVMAX 70\rMV35\r");
    EXPECT_EQ(parser_.max_volume_, 70);

    // Volume 35 with max 70 = 50%
    int result = parser_.parse_volume_response("MV35\r");
    EXPECT_EQ(result, 50);  // 35/70 * 100 = 50
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_InvalidFormat_NoDigits) {
    int result = parser_.parse_volume_response("MVUP\r");
    EXPECT_EQ(result, -1);
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_InvalidFormat_NoMV) {
    int result = parser_.parse_volume_response("45\r");
    EXPECT_EQ(result, -1);
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_InvalidFormat_TooManyDigits) {
    int result = parser_.parse_volume_response("MV1234\r");
    EXPECT_EQ(result, -1);
}

TEST_F(DenonProtocolTest, ParseVolumeResponse_WithExtraData) {
    // Real responses often have multiple status lines
    std::string response = "PWON\rMV55\rMUOFF\r";
    int result = parser_.parse_volume_response(response);
    EXPECT_EQ(result, 56);  // 55/98 * 100 = 56.1 -> 56
}

// ============================================================================
// Volume Command Formatting Tests
// ============================================================================

TEST_F(DenonProtocolTest, FormatVolumeCommand_Zero) {
    std::string cmd = parser_.format_volume_command(0);
    EXPECT_EQ(cmd, "MV00");
}

TEST_F(DenonProtocolTest, FormatVolumeCommand_Fifty) {
    std::string cmd = parser_.format_volume_command(50);
    EXPECT_EQ(cmd, "MV49");  // 50/100 * 98 = 49
}

TEST_F(DenonProtocolTest, FormatVolumeCommand_Max) {
    std::string cmd = parser_.format_volume_command(100);
    EXPECT_EQ(cmd, "MV98");
}

TEST_F(DenonProtocolTest, FormatVolumeCommand_WithCustomMax) {
    // Some receivers have max volume of 70
    std::string cmd = parser_.format_volume_command(50, 70);
    EXPECT_EQ(cmd, "MV35");  // 50/100 * 70 = 35
}

TEST_F(DenonProtocolTest, FormatVolumeCommand_RoundingDown) {
    std::string cmd = parser_.format_volume_command(33);
    EXPECT_EQ(cmd, "MV32");  // 33/100 * 98 = 32.34 -> 32
}

// ============================================================================
// Mute Response Parsing Tests
// ============================================================================

TEST_F(DenonProtocolTest, ParseMuteResponse_On) {
    bool muted;
    bool result = parser_.parse_mute_response("MUON\r", muted);
    EXPECT_TRUE(result);
    EXPECT_TRUE(muted);
}

TEST_F(DenonProtocolTest, ParseMuteResponse_Off) {
    bool muted;
    bool result = parser_.parse_mute_response("MUOFF\r", muted);
    EXPECT_TRUE(result);
    EXPECT_FALSE(muted);
}

TEST_F(DenonProtocolTest, ParseMuteResponse_Invalid) {
    bool muted = true;
    bool result = parser_.parse_mute_response("MVUP\r", muted);
    EXPECT_FALSE(result);
    EXPECT_TRUE(muted);  // Should remain unchanged
}

TEST_F(DenonProtocolTest, ParseMuteResponse_WithExtraData) {
    bool muted;
    std::string response = "PWON\rMUON\rMV55\r";
    bool result = parser_.parse_mute_response(response, muted);
    EXPECT_TRUE(result);
    EXPECT_TRUE(muted);
}

// ============================================================================
// Power Response Parsing Tests
// ============================================================================

TEST_F(DenonProtocolTest, ParsePowerResponse_On) {
    bool power_on;
    bool result = parser_.parse_power_response("PWON\r", power_on);
    EXPECT_TRUE(result);
    EXPECT_TRUE(power_on);
}

TEST_F(DenonProtocolTest, ParsePowerResponse_Standby) {
    bool power_on;
    bool result = parser_.parse_power_response("PWSTANDBY\r", power_on);
    EXPECT_TRUE(result);
    EXPECT_FALSE(power_on);
}

TEST_F(DenonProtocolTest, ParsePowerResponse_Invalid) {
    bool power_on = true;
    bool result = parser_.parse_power_response("MVUP\r", power_on);
    EXPECT_FALSE(result);
    EXPECT_TRUE(power_on);  // Should remain unchanged
}

// ============================================================================
// Input Response Parsing Tests
// ============================================================================

TEST_F(DenonProtocolTest, ParseInputResponse_MPLAY) {
    std::string input;
    bool result = parser_.parse_input_response("SIMPLAY\r", input);
    EXPECT_TRUE(result);
    EXPECT_EQ(input, "MPLAY");
}

TEST_F(DenonProtocolTest, ParseInputResponse_DVD) {
    std::string input;
    bool result = parser_.parse_input_response("SIDVD\r", input);
    EXPECT_TRUE(result);
    EXPECT_EQ(input, "DVD");
}

TEST_F(DenonProtocolTest, ParseInputResponse_WithWhitespace) {
    std::string input;
    bool result = parser_.parse_input_response("SIMPLAY \r", input);
    EXPECT_TRUE(result);
    EXPECT_EQ(input, "MPLAY");
}

TEST_F(DenonProtocolTest, ParseInputResponse_Invalid_NoSI) {
    std::string input;
    bool result = parser_.parse_input_response("MVUP\r", input);
    EXPECT_FALSE(result);
}

TEST_F(DenonProtocolTest, ParseInputResponse_WithExtraData) {
    std::string input;
    std::string response = "PWON\rSIMPLAY\rMV55\r";
    bool result = parser_.parse_input_response(response, input);
    EXPECT_TRUE(result);
    EXPECT_EQ(input, "MPLAY");
}

// ============================================================================
// Real-world Protocol Scenarios
// ============================================================================

TEST_F(DenonProtocolTest, RealWorld_InitialProbe) {
    // When connecting, receiver sends multiple status lines
    std::string response = "PWON\rMV70\rMVMAX 70\rMUOFF\rSIMPLAY\r";

    int volume = parser_.parse_volume_response(response);
    EXPECT_EQ(parser_.max_volume_, 70);
    EXPECT_EQ(volume, 100);

    bool muted;
    parser_.parse_mute_response(response, muted);
    EXPECT_FALSE(muted);

    bool power_on;
    parser_.parse_power_response(response, power_on);
    EXPECT_TRUE(power_on);

    std::string input;
    parser_.parse_input_response(response, input);
    EXPECT_EQ(input, "MPLAY");
}

TEST_F(DenonProtocolTest, RealWorld_VolumeChangeEcho) {
    // When you send MV50, receiver echoes it back
    std::string response = "MV50\r";
    int volume = parser_.parse_volume_response(response);
    EXPECT_EQ(volume, 51);  // 50/98 * 100 = 51.0
}

TEST_F(DenonProtocolTest, RealWorld_VolumeUpIncrement) {
    // MVUP command doesn't return a specific value, just echo
    std::string response = "MVUP\r";
    int volume = parser_.parse_volume_response(response);
    EXPECT_EQ(volume, -1);  // Can't parse MVUP
}

} // namespace Testing
} // namespace AlsaAVR
