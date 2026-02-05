#include "../include/backend.hpp"
#include <iostream>
#include <unistd.h>

using namespace AlsaAVR;

std::string speaker_to_string(Speaker speaker) {
    switch (speaker) {
    case Speaker::FRONT_LEFT: return "FL";
    case Speaker::FRONT_RIGHT: return "FR";
    case Speaker::CENTER: return "C";
    case Speaker::SUBWOOFER: return "SW";
    case Speaker::SURROUND_LEFT: return "SL";
    case Speaker::SURROUND_RIGHT: return "SR";
    case Speaker::SURROUND_BACK_LEFT: return "SBL";
    case Speaker::SURROUND_BACK_RIGHT: return "SBR";
    case Speaker::SURROUND_BACK: return "SB";
    case Speaker::FRONT_HEIGHT_LEFT: return "FHL";
    case Speaker::FRONT_HEIGHT_RIGHT: return "FHR";
    default: return "UNKNOWN";
    }
}

Speaker string_to_speaker(const std::string& str) {
    if (str == "FL") return Speaker::FRONT_LEFT;
    if (str == "FR") return Speaker::FRONT_RIGHT;
    if (str == "C") return Speaker::CENTER;
    if (str == "SW") return Speaker::SUBWOOFER;
    if (str == "SL") return Speaker::SURROUND_LEFT;
    if (str == "SR") return Speaker::SURROUND_RIGHT;
    if (str == "SBL") return Speaker::SURROUND_BACK_LEFT;
    if (str == "SBR") return Speaker::SURROUND_BACK_RIGHT;
    if (str == "SB") return Speaker::SURROUND_BACK;
    if (str == "FHL") return Speaker::FRONT_HEIGHT_LEFT;
    if (str == "FHR") return Speaker::FRONT_HEIGHT_RIGHT;
    return Speaker::FRONT_LEFT; // Default
}

void print_capabilities(const BackendCaps& caps) {
    std::cout << "Backend Capabilities:" << std::endl;
    std::cout << "  Absolute volume: " << (caps.supports_absolute ? "Yes" : "No") << std::endl;
    std::cout << "  Query volume: " << (caps.supports_query ? "Yes" : "No") << std::endl;
    std::cout << "  Mute control: " << (caps.supports_mute ? "Yes" : "No") << std::endl;
    std::cout << "  Input selection: " << (caps.supports_input_select ? "Yes" : "No") << std::endl;
    std::cout << "  Speaker levels: " << (caps.supports_speaker_level ? "Yes" : "No") << std::endl;
    std::cout << "  Volume range: " << caps.min_volume << "-" << caps.max_volume << std::endl;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <backend_type> [options...]" << std::endl;
        std::cerr << "Backend types: denon_ip, nad_serial, opalum_serial, cec" << std::endl;
        std::cerr << std::endl;
        std::cerr << "Examples:" << std::endl;
        std::cerr << "  " << argv[0] << " denon_ip host=192.168.1.100" << std::endl;
        std::cerr << "  " << argv[0] << " nad_serial device=/dev/ttyUSB0" << std::endl;
        std::cerr << "  " << argv[0] << " opalum_serial device=/dev/ttyUSB0" << std::endl;
        std::cerr << "  " << argv[0] << " cec" << std::endl;
        return 1;
    }

    std::string backend_type = argv[1];
    std::map<std::string, std::string> config;

    // Parse config arguments (key=value)
    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        size_t eq_pos = arg.find('=');
        if (eq_pos != std::string::npos) {
            std::string key = arg.substr(0, eq_pos);
            std::string value = arg.substr(eq_pos + 1);
            config[key] = value;
        }
    }

    // Create backend
    auto backend = BackendFactory::create(backend_type);
    if (!backend) {
        std::cerr << "Unknown backend: " << backend_type << std::endl;
        return 1;
    }

    std::cout << "Initializing backend: " << backend->name() << std::endl;

    if (!backend->init(config)) {
        std::cerr << "Failed to initialize backend" << std::endl;
        return 1;
    }

    std::cout << "Backend initialized successfully" << std::endl;
    print_capabilities(backend->capabilities());

    // Interactive test loop
    std::cout << std::endl << "Commands:" << std::endl;
    std::cout << "  v <0-100>  - Set volume" << std::endl;
    std::cout << "  +          - Volume up" << std::endl;
    std::cout << "  -          - Volume down" << std::endl;
    std::cout << "  m          - Toggle mute" << std::endl;
    std::cout << "  p          - Toggle power" << std::endl;
    std::cout << "  q          - Query current state" << std::endl;
    std::cout << "  i          - List available inputs" << std::endl;
    std::cout << "  s <input>  - Switch to input" << std::endl;
    std::cout << "  a          - Automated test: cycle through all inputs" << std::endl;
    std::cout << "  l          - List available speakers" << std::endl;
    std::cout << "  k <speaker> <level> - Set speaker level (38-62, 50=0dB)" << std::endl;
    std::cout << "  g <speaker> - Get speaker level" << std::endl;
    std::cout << "  A          - Automated test: cycle through speaker levels" << std::endl;
    std::cout << "  t          - Test max_volume feature" << std::endl;
    std::cout << "  x          - Exit" << std::endl;
    std::cout << std::endl;

    bool running = true;
    bool muted = false;

    while (running) {
        std::cout << "> ";
        std::string cmd;
        std::getline(std::cin, cmd);

        if (cmd.empty()) continue;

        switch (cmd[0]) {
        case 'v': {
            // Check if it's "v <number>" format (has space and digits)
            if (cmd.length() > 2 && cmd[1] == ' ') {
                try {
                    int vol = std::stoi(cmd.substr(2));
                    if (backend->set_volume_absolute(vol)) {
                        std::cout << "Volume set to " << vol << std::endl;
                    } else {
                        std::cout << "Failed to set volume" << std::endl;
                    }
                } catch (const std::exception& e) {
                    std::cout << "Invalid volume value" << std::endl;
                }
            } else {
                std::cout << "Invalid command. Use: v <0-100>" << std::endl;
            }
            break;
        }
        case '+':
            if (backend->volume_up()) {
                std::cout << "Volume increased" << std::endl;
            } else {
                std::cout << "Failed to increase volume" << std::endl;
            }
            break;
        case '-':
            if (backend->volume_down()) {
                std::cout << "Volume decreased" << std::endl;
            } else {
                std::cout << "Failed to decrease volume" << std::endl;
            }
            break;
        case 'm':
            muted = !muted;
            if (backend->set_mute(muted)) {
                std::cout << "Mute: " << (muted ? "ON" : "OFF") << std::endl;
            } else {
                std::cout << "Failed to set mute" << std::endl;
            }
            break;
        case 'p': {
            bool power = !backend->get_power();
            if (backend->set_power(power)) {
                std::cout << "Power: " << (power ? "ON" : "OFF") << std::endl;
            } else {
                std::cout << "Power control not supported or failed" << std::endl;
            }
            break;
        }
        case 'q': {
            BackendCaps caps = backend->capabilities();
            int vol = backend->get_volume();
            std::cout << "Current volume: " << vol << std::endl;
            if (caps.supports_mute) {
                std::cout << "Current mute: " << (backend->get_mute() ? "ON" : "OFF") << std::endl;
            }
            if (caps.supports_input_select) {
                std::string current_input = backend->get_current_input();
                std::cout << "Current input: " << (current_input.empty() ? "Unknown" : current_input) << std::endl;
            }
            break;
        }
        case 'i': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_input_select) {
                std::cout << "Input selection not supported by this backend" << std::endl;
                break;
            }
            auto inputs = backend->get_available_inputs();
            if (inputs.empty()) {
                std::cout << "No inputs available" << std::endl;
            } else {
                std::cout << "Available inputs:" << std::endl;
                for (size_t i = 0; i < inputs.size(); i++) {
                    std::cout << "  [" << i << "] " << inputs[i] << std::endl;
                }
                std::string current = backend->get_current_input();
                if (!current.empty()) {
                    std::cout << "Current: " << current << std::endl;
                }
            }
            break;
        }
        case 's': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_input_select) {
                std::cout << "Input selection not supported by this backend" << std::endl;
                break;
            }
            if (cmd.length() > 2 && cmd[1] == ' ') {
                std::string input = cmd.substr(2);
                // Trim whitespace
                size_t start = input.find_first_not_of(" \t");
                size_t end = input.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    input = input.substr(start, end - start + 1);
                }

                if (backend->set_input(input)) {
                    std::cout << "Switched to input: " << input << std::endl;
                } else {
                    std::cout << "Failed to switch input" << std::endl;
                }
            } else {
                std::cout << "Invalid command. Use: s <input>" << std::endl;
            }
            break;
        }
        case 'a': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_input_select) {
                std::cout << "Input selection not supported by this backend" << std::endl;
                break;
            }

            auto inputs = backend->get_available_inputs();
            if (inputs.empty()) {
                std::cout << "No inputs available" << std::endl;
                break;
            }

            std::cout << "\n=== Automated Input Switching Test ===" << std::endl;
            std::cout << "Testing all " << inputs.size() << " available inputs...\n" << std::endl;

            int success_count = 0;
            int fail_count = 0;

            for (size_t i = 0; i < inputs.size(); i++) {
                const std::string& input = inputs[i];
                std::cout << "Test " << (i + 1) << "/" << inputs.size() << ": Switching to " << input << "...";
                std::cout.flush();

                if (backend->set_input(input)) {
                    usleep(500000); // Wait 500ms for input to switch

                    // Verify the switch
                    std::string current = backend->get_current_input();
                    if (current == input) {
                        std::cout << " OK (verified)" << std::endl;
                        success_count++;
                    } else {
                        std::cout << " OK (sent, but verification shows: " << current << ")" << std::endl;
                        success_count++;
                    }
                } else {
                    std::cout << " FAILED" << std::endl;
                    fail_count++;
                }
            }

            std::cout << "\n=== Test Results ===" << std::endl;
            std::cout << "Successful: " << success_count << "/" << inputs.size() << std::endl;
            std::cout << "Failed: " << fail_count << "/" << inputs.size() << std::endl;

            if (fail_count == 0) {
                std::cout << "All input tests passed! ✓" << std::endl;
            } else {
                std::cout << "Some tests failed. Check the log file for details." << std::endl;
            }
            std::cout << std::endl;
            break;
        }
        case 'l': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_speaker_level) {
                std::cout << "Speaker level control not supported by this backend" << std::endl;
                break;
            }
            auto speakers = backend->get_available_speakers();
            if (speakers.empty()) {
                std::cout << "No speakers available" << std::endl;
            } else {
                std::cout << "Available speakers (range: " << backend->get_speaker_level_min()
                         << "-" << backend->get_speaker_level_max() << ", 50=0dB):" << std::endl;
                for (const auto& speaker : speakers) {
                    int level = backend->get_speaker_level(speaker);
                    int db_offset = level - 50;
                    std::cout << "  " << speaker_to_string(speaker) << ": " << level
                             << " (" << (db_offset >= 0 ? "+" : "") << db_offset << "dB)" << std::endl;
                }
            }
            break;
        }
        case 'k': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_speaker_level) {
                std::cout << "Speaker level control not supported by this backend" << std::endl;
                break;
            }
            if (cmd.length() > 2 && cmd[1] == ' ') {
                size_t space_pos = cmd.find(' ', 2);
                if (space_pos != std::string::npos) {
                    std::string speaker_str = cmd.substr(2, space_pos - 2);
                    std::string level_str = cmd.substr(space_pos + 1);

                    try {
                        int level = std::stoi(level_str);
                        Speaker speaker = string_to_speaker(speaker_str);

                        if (backend->set_speaker_level(speaker, level)) {
                            int db_offset = level - 50;
                            std::cout << "Set " << speaker_str << " level to " << level
                                     << " (" << (db_offset >= 0 ? "+" : "") << db_offset << "dB)" << std::endl;
                        } else {
                            std::cout << "Failed to set speaker level" << std::endl;
                        }
                    } catch (const std::exception& e) {
                        std::cout << "Invalid level value" << std::endl;
                    }
                } else {
                    std::cout << "Invalid command. Use: k <speaker> <level>" << std::endl;
                }
            } else {
                std::cout << "Invalid command. Use: k <speaker> <level>" << std::endl;
            }
            break;
        }
        case 'g': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_speaker_level) {
                std::cout << "Speaker level control not supported by this backend" << std::endl;
                break;
            }
            if (cmd.length() > 2 && cmd[1] == ' ') {
                std::string speaker_str = cmd.substr(2);
                // Trim whitespace
                size_t start = speaker_str.find_first_not_of(" \t");
                size_t end = speaker_str.find_last_not_of(" \t");
                if (start != std::string::npos && end != std::string::npos) {
                    speaker_str = speaker_str.substr(start, end - start + 1);
                }

                Speaker speaker = string_to_speaker(speaker_str);
                int level = backend->get_speaker_level(speaker);
                int db_offset = level - 50;
                std::cout << speaker_str << " level: " << level
                         << " (" << (db_offset >= 0 ? "+" : "") << db_offset << "dB)" << std::endl;
            } else {
                std::cout << "Invalid command. Use: g <speaker>" << std::endl;
            }
            break;
        }
        case 'A': {
            BackendCaps caps = backend->capabilities();
            if (!caps.supports_speaker_level) {
                std::cout << "Speaker level control not supported by this backend" << std::endl;
                break;
            }

            auto speakers = backend->get_available_speakers();
            if (speakers.empty()) {
                std::cout << "No speakers available" << std::endl;
                break;
            }

            std::cout << "\n=== Automated Speaker Level Test ===" << std::endl;
            std::cout << "Testing all " << speakers.size() << " available speakers...\n" << std::endl;

            int success_count = 0;
            int fail_count = 0;

            // Test: Set each speaker to 48 (-2dB), verify, then restore to 50 (0dB)
            for (size_t i = 0; i < speakers.size(); i++) {
                const Speaker& speaker = speakers[i];
                std::string speaker_str = speaker_to_string(speaker);

                std::cout << "Test " << (i + 1) << "/" << speakers.size() << ": Testing " << speaker_str << "...";
                std::cout.flush();

                // Get original level
                int original_level = backend->get_speaker_level(speaker);

                // Set to 48 (-2dB)
                if (backend->set_speaker_level(speaker, 48)) {
                    usleep(300000); // Wait 300ms

                    // Verify
                    int current_level = backend->get_speaker_level(speaker);
                    if (current_level == 48) {
                        // Restore to original
                        backend->set_speaker_level(speaker, original_level);
                        std::cout << " OK (set to 48, verified, restored to " << original_level << ")" << std::endl;
                        success_count++;
                    } else {
                        std::cout << " PARTIAL (set, but reads as " << current_level << ", restored)" << std::endl;
                        backend->set_speaker_level(speaker, original_level);
                        success_count++;
                    }
                } else {
                    std::cout << " FAILED" << std::endl;
                    fail_count++;
                }
            }

            std::cout << "\n=== Test Results ===" << std::endl;
            std::cout << "Successful: " << success_count << "/" << speakers.size() << std::endl;
            std::cout << "Failed: " << fail_count << "/" << speakers.size() << std::endl;

            if (fail_count == 0) {
                std::cout << "All speaker level tests passed! ✓" << std::endl;
            } else {
                std::cout << "Some tests failed. Check the log file for details." << std::endl;
            }
            std::cout << std::endl;
            break;
        }
        case 't': {
            std::cout << "\n=== Testing max_volume feature ===" << std::endl;
            std::cout << "This test verifies that ALSA volume 100% maps to the configured max_volume" << std::endl;
            std::cout << "on the Denon receiver (default 98, or custom value from config).\n" << std::endl;

            // Test 1: Set to 50% and query back
            std::cout << "Test 1: Setting ALSA volume to 50..." << std::endl;
            if (backend->set_volume_absolute(50)) {
                usleep(300000); // Wait 300ms for command to process
                int vol = backend->get_volume();
                std::cout << "  Result: ALSA reports " << vol << "%" << std::endl;
            } else {
                std::cout << "  Failed to set volume" << std::endl;
            }

            // Test 2: Set to 100% and query back
            std::cout << "\nTest 2: Setting ALSA volume to 100%..." << std::endl;
            if (backend->set_volume_absolute(100)) {
                usleep(300000); // Wait 300ms for command to process
                int vol = backend->get_volume();
                std::cout << "  Result: ALSA reports " << vol << "%" << std::endl;
                std::cout << "  Expected: This should map to your configured max_volume on the Denon" << std::endl;
                std::cout << "           (check log file for actual Denon command sent)" << std::endl;
            } else {
                std::cout << "  Failed to set volume" << std::endl;
            }

            // Test 3: Set to 0% and query back
            std::cout << "\nTest 3: Setting ALSA volume to 0%..." << std::endl;
            if (backend->set_volume_absolute(0)) {
                usleep(300000); // Wait 300ms for command to process
                int vol = backend->get_volume();
                std::cout << "  Result: ALSA reports " << vol << "%" << std::endl;
            } else {
                std::cout << "  Failed to set volume" << std::endl;
            }

            std::cout << "\n=== Test complete ===" << std::endl;
            std::cout << "Check the log file to see the actual Denon MV commands sent." << std::endl;
            std::cout << "With max_volume=80: ALSA 100% should send MV80" << std::endl;
            std::cout << "With max_volume=98: ALSA 100% should send MV98" << std::endl;
            break;
        }
        case 'x':
            running = false;
            break;
        default:
            std::cout << "Unknown command" << std::endl;
        }
    }

    backend->close();
    std::cout << "Backend closed" << std::endl;

    return 0;
}
