#include "plugin_common.hpp"

namespace AlsaAVR {

// Forward declaration of backend creation function
extern std::unique_ptr<Backend> create_cec_backend();

} // namespace AlsaAVR

// ALSA plugin entry point
extern "C" {
SND_CTL_PLUGIN_DEFINE_FUNC(cec) {
    using namespace AlsaAVR;

    snd_config_iterator_t it, next;
    std::map<std::string, std::string> backend_config;

    // Parse configuration
    snd_config_for_each(it, next, conf) {
        snd_config_t *n = snd_config_iterator_entry(it);
        const char *id;
        if (snd_config_get_id(n, &id) < 0)
            continue;

        if (strcmp(id, "type") == 0)
            continue;

        const char* value;
        if (snd_config_get_string(n, &value) == 0) {
            backend_config[id] = value;
        }
    }

    // Create CEC backend directly (no connection pooling needed for CEC)
    auto backend = create_cec_backend();
    if (!backend) {
        SNDERR("CEC plugin: Failed to create backend");
        return -EINVAL;
    }

    if (!backend->init(backend_config)) {
        SNDERR("CEC plugin: Backend initialization failed");
        return -EIO;
    }

    // Create control structure
    AVRControl* ctrl = new AVRControl();
    ctrl->backend = std::move(backend);

    // Initialize state from backend
    ctrl->volume = ctrl->backend->get_volume();
    ctrl->mute = ctrl->backend->get_mute();
    ctrl->power = ctrl->backend->get_power();

    // Initialize available inputs and current selection
    ctrl->available_inputs = ctrl->backend->get_available_inputs();
    auto current_input = ctrl->backend->get_current_input();
    if (current_input) {
        for (size_t i = 0; i < ctrl->available_inputs.size(); i++) {
            if (ctrl->available_inputs[i] == *current_input) {
                ctrl->current_input_index = static_cast<int>(i);
                break;
            }
        }
    }

    // Initialize speaker levels
    auto speakers = ctrl->backend->get_available_speakers();
    for (const auto& speaker : speakers) {
        int level = ctrl->backend->get_speaker_level(speaker);
        ctrl->speaker_levels[speaker] = level;
    }

    // Calculate TLV dB range based on detected hardware max volume
    int hw_max_volume = ctrl->backend->get_hardware_max_volume();
    ctrl->db_scale[0] = SNDRV_CTL_TLVT_DB_SCALE;        // Type
    ctrl->db_scale[1] = 2 * sizeof(unsigned int);       // Length (2 elements following)
    ctrl->db_scale[2] = static_cast<unsigned int>(-8000);  // min dB (-80 dB in 1/100 dB units)
    ctrl->db_scale[3] = static_cast<unsigned int>(hw_max_volume);  // step in 1/100 dB units

    // Initialize ALSA external control
    ctrl->ext.version = SND_CTL_EXT_VERSION;
    ctrl->ext.card_idx = 0;
    strcpy(ctrl->ext.id, "CEC");
    strcpy(ctrl->ext.driver, "CEC HDMI Control");
    strcpy(ctrl->ext.name, "CEC Control");
    strcpy(ctrl->ext.longname, "CEC HDMI External Volume Control");
    strcpy(ctrl->ext.mixername, "CEC Mixer");
    ctrl->ext.callback = &avr_callbacks;
    ctrl->ext.private_data = ctrl;
    ctrl->ext.tlv.p = ctrl->db_scale;  // Set TLV data for dB range support

    int err = snd_ctl_ext_create(&ctrl->ext, name, mode);
    if (err < 0) {
        delete ctrl;
        return err;
    }

    *handlep = ctrl->ext.handle;
    return 0;
}
} // extern "C"

// Plugin symbol export - must be outside extern "C" for C++ linkage with ALSA
SND_CTL_PLUGIN_SYMBOL(cec);
