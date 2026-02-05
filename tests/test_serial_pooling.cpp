#include <iostream>
#include <dlfcn.h>
#include <alsa/asoundlib.h>
#include <unistd.h>

// Test serial port pooling by creating/destroying multiple ALSA controls
// This simulates what shairport-sync does frequently

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <iterations>" << std::endl;
        std::cerr << "Example: " << argv[0] << " 10" << std::endl;
        return 1;
    }

    int iterations = std::stoi(argv[1]);

    std::cout << "Testing serial port pooling with " << iterations << " iterations..." << std::endl;
    std::cout << "This simulates ALSA creating/destroying controls frequently." << std::endl;
    std::cout << "Without pooling, you'd see the serial port open/close each time." << std::endl;
    std::cout << "With pooling, the serial port stays open and is reused." << std::endl;
    std::cout << std::endl;

    for (int i = 0; i < iterations; i++) {
        std::cout << "Iteration " << (i + 1) << "/" << iterations << "..." << std::endl;

        // Open ALSA control
        snd_ctl_t *ctl;
        int err = snd_ctl_open(&ctl, "avr", 0);
        if (err < 0) {
            std::cerr << "Failed to open ALSA control: " << snd_strerror(err) << std::endl;
            return 1;
        }

        // Get volume control element
        snd_ctl_elem_id_t *id;
        snd_ctl_elem_id_alloca(&id);
        snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);
        snd_ctl_elem_id_set_name(id, "AVR");

        // Read current volume
        snd_ctl_elem_value_t *value;
        snd_ctl_elem_value_alloca(&value);
        snd_ctl_elem_value_set_id(value, id);

        err = snd_ctl_elem_read(ctl, value);
        if (err < 0) {
            std::cerr << "Failed to read volume: " << snd_strerror(err) << std::endl;
        } else {
            long vol = snd_ctl_elem_value_get_integer(value, 0);
            std::cout << "  Current volume: " << vol << std::endl;
        }

        // Set volume to 50
        snd_ctl_elem_value_set_integer(value, 0, 50);
        err = snd_ctl_elem_write(ctl, value);
        if (err < 0) {
            std::cerr << "Failed to set volume: " << snd_strerror(err) << std::endl;
        } else {
            std::cout << "  Set volume to 50" << std::endl;
        }

        // Close control (this is where pooling matters - without it, serial port closes here)
        snd_ctl_close(ctl);
        std::cout << "  Control closed (serial port should stay open in pool)" << std::endl;

        // Small delay between iterations
        usleep(100000); // 100ms
    }

    std::cout << std::endl << "Test complete!" << std::endl;
    std::cout << "Check the logs to verify the serial port was only opened once." << std::endl;

    return 0;
}
