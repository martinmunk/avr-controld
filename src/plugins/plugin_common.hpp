#pragma once

#include "../../include/backend.hpp"
#include <alsa/asoundlib.h>
#include <alsa/control_external.h>
#include <pthread.h>
#include <memory>
#include <cstring>
#include <map>

// TLV constants (from sound/tlv.h)
#define SNDRV_CTL_TLVT_DB_SCALE 1

namespace AlsaAVR {

// Note: Connection pooling is now handled internally by each backend
// (DenonConnection, NADConnection, OpalumConnection)
// No shared ConnectionPool or SharedState classes needed here anymore!

struct AVRControl {
    snd_ctl_ext_t ext;
    std::unique_ptr<Backend> backend;

    // State tracking - nullopt means unknown/not set through plugin
    std::optional<int> volume;
    std::optional<bool> mute;
    std::optional<bool> power;
    std::optional<bool> standby_timeout;
    std::vector<std::string> available_inputs;
    std::optional<int> current_input_index;
    std::map<AlsaAVR::Speaker, int> speaker_levels;  // Speaker trim levels (38-62)

    // TLV data for volume control (dynamically calculated based on receiver's max volume)
    unsigned int db_scale[4];

    // Element IDs
    enum {
        ELEM_VOLUME = 0,
        ELEM_MUTE,
        ELEM_POWER,
        ELEM_STANDBY_TIMEOUT,
        ELEM_INPUT,
        ELEM_SPEAKER_FL,      // Front Left
        ELEM_SPEAKER_FR,      // Front Right
        ELEM_SPEAKER_C,       // Center
        ELEM_SPEAKER_SW,      // Subwoofer
        ELEM_SPEAKER_SL,      // Surround Left
        ELEM_SPEAKER_SR,      // Surround Right
        ELEM_SPEAKER_SBL,     // Surround Back Left
        ELEM_SPEAKER_SBR,     // Surround Back Right
        ELEM_SPEAKER_SB,      // Surround Back
        ELEM_SPEAKER_FHL,     // Front Height Left
        ELEM_SPEAKER_FHR,     // Front Height Right
        ELEM_COUNT
    };
};

// Forward declarations
static void avr_close(snd_ctl_ext_t *ext);
static int avr_elem_count(snd_ctl_ext_t *ext);
static int avr_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id);
static snd_ctl_ext_key_t avr_find_elem(snd_ctl_ext_t *ext, const snd_ctl_elem_id_t *id);
static int avr_get_attribute(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                             int *type, unsigned int *acc, unsigned int *count);
static int avr_get_integer_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key,
                                long *imin, long *imax, long *istep);
static int avr_get_enumerated_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items);
static int avr_get_enumerated_name(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int item, char *name, size_t name_max_len);
static int avr_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value);
static int avr_read_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *item);
static int avr_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value);
static int avr_write_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *item);
static int avr_read_event(snd_ctl_ext_t *ext, snd_ctl_elem_id_t *id, unsigned int *event_mask);

static snd_ctl_ext_callback_t avr_callbacks = {
    avr_close,
    avr_elem_count,
    avr_elem_list,
    avr_find_elem,
    nullptr, // free_key
    avr_get_attribute,
    avr_get_integer_info,
    nullptr, // get_integer64_info
    avr_get_enumerated_info,
    avr_get_enumerated_name,
    avr_read_integer,
    nullptr, // read_integer64
    avr_read_enumerated,
    nullptr, // read_bytes
    nullptr, // read_iec958
    avr_write_integer,
    nullptr, // write_integer64
    avr_write_enumerated,
    nullptr, // write_bytes
    nullptr, // write_iec958
    nullptr, // subscribe_events
    avr_read_event,
    nullptr, // poll_descriptors_count
    nullptr, // poll_descriptors
    nullptr, // poll_revents
};

static void avr_close(snd_ctl_ext_t *ext) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    // Backend cleanup is handled by the backend's destructor
    // Connection pooling is now internal to each backend
    delete ctrl;
}

static int avr_elem_count(snd_ctl_ext_t *ext) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    int count = 3;  // Volume, Mute, Power (always available)

    // Add Standby Timeout control if supported
    if (ctrl->backend && ctrl->backend->capabilities().supports_standby_timeout) {
        count++;
    }

    // Add Input Source control if supported
    if (ctrl->backend && ctrl->backend->capabilities().supports_input_select && !ctrl->available_inputs.empty()) {
        count++;
    }

    // Add speaker level controls based on actual available speakers
    if (ctrl->backend && ctrl->backend->capabilities().supports_speaker_level) {
        count += ctrl->backend->get_available_speakers().size();
    }

    return count;
}

static int offset_to_elem(snd_ctl_ext_t *ext, unsigned int offset) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    // Map offset to element ID based on what's supported
    if (offset == 0) return AVRControl::ELEM_VOLUME;
    if (offset == 1) return AVRControl::ELEM_MUTE;
    if (offset == 2) return AVRControl::ELEM_POWER;

    unsigned int current = 3;

    // Standby timeout (if supported)
    if (ctrl->backend && ctrl->backend->capabilities().supports_standby_timeout) {
        if (offset == current) return AVRControl::ELEM_STANDBY_TIMEOUT;
        current++;
    }

    // Input source (if supported)
    if (ctrl->backend && ctrl->backend->capabilities().supports_input_select && !ctrl->available_inputs.empty()) {
        if (offset == current) return AVRControl::ELEM_INPUT;
        current++;
    }

    // Speaker levels (if supported) - map based on actual available speakers
    if (ctrl->backend && ctrl->backend->capabilities().supports_speaker_level) {
        auto speakers = ctrl->backend->get_available_speakers();
        for (const auto& speaker : speakers) {
            if (offset == current++) {
                // Map Speaker enum to ELEM_SPEAKER_* based on speaker type
                switch (speaker) {
                case AlsaAVR::Speaker::FRONT_LEFT: return AVRControl::ELEM_SPEAKER_FL;
                case AlsaAVR::Speaker::FRONT_RIGHT: return AVRControl::ELEM_SPEAKER_FR;
                case AlsaAVR::Speaker::CENTER: return AVRControl::ELEM_SPEAKER_C;
                case AlsaAVR::Speaker::SUBWOOFER: return AVRControl::ELEM_SPEAKER_SW;
                case AlsaAVR::Speaker::SURROUND_LEFT: return AVRControl::ELEM_SPEAKER_SL;
                case AlsaAVR::Speaker::SURROUND_RIGHT: return AVRControl::ELEM_SPEAKER_SR;
                case AlsaAVR::Speaker::SURROUND_BACK_LEFT: return AVRControl::ELEM_SPEAKER_SBL;
                case AlsaAVR::Speaker::SURROUND_BACK_RIGHT: return AVRControl::ELEM_SPEAKER_SBR;
                case AlsaAVR::Speaker::SURROUND_BACK: return AVRControl::ELEM_SPEAKER_SB;
                case AlsaAVR::Speaker::FRONT_HEIGHT_LEFT: return AVRControl::ELEM_SPEAKER_FHL;
                case AlsaAVR::Speaker::FRONT_HEIGHT_RIGHT: return AVRControl::ELEM_SPEAKER_FHR;
                }
            }
        }
    }

    return -1;  // Invalid offset
}

static int avr_elem_list(snd_ctl_ext_t *ext, unsigned int offset, snd_ctl_elem_id_t *id) {
    snd_ctl_elem_id_set_interface(id, SND_CTL_ELEM_IFACE_MIXER);

    int elem = offset_to_elem(ext, offset);
    if (elem < 0) {
        return -EINVAL;  // Invalid offset
    }

    switch (elem) {
    case AVRControl::ELEM_VOLUME:
        snd_ctl_elem_id_set_name(id, "Master Playback Volume");
        break;
    case AVRControl::ELEM_MUTE:
        snd_ctl_elem_id_set_name(id, "Master Playback Switch");
        break;
    case AVRControl::ELEM_POWER:
        snd_ctl_elem_id_set_name(id, "Master Playback Power");
        break;
    case AVRControl::ELEM_STANDBY_TIMEOUT:
        snd_ctl_elem_id_set_name(id, "Standby Timeout");
        break;
    case AVRControl::ELEM_INPUT:
        snd_ctl_elem_id_set_name(id, "Input Source");
        break;
    case AVRControl::ELEM_SPEAKER_FL:
        snd_ctl_elem_id_set_name(id, "01 FL Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_FR:
        snd_ctl_elem_id_set_name(id, "02 FR Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_C:
        snd_ctl_elem_id_set_name(id, "03 C Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SW:
        snd_ctl_elem_id_set_name(id, "04 SW Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SL:
        snd_ctl_elem_id_set_name(id, "05 SL Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SR:
        snd_ctl_elem_id_set_name(id, "06 SR Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SBL:
        snd_ctl_elem_id_set_name(id, "07 SBL Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SBR:
        snd_ctl_elem_id_set_name(id, "08 SBR Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_SB:
        snd_ctl_elem_id_set_name(id, "09 SB Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_FHL:
        snd_ctl_elem_id_set_name(id, "10 FHL Playback Volume");
        break;
    case AVRControl::ELEM_SPEAKER_FHR:
        snd_ctl_elem_id_set_name(id, "11 FHR Playback Volume");
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

static snd_ctl_ext_key_t avr_find_elem(snd_ctl_ext_t * /*ext*/, const snd_ctl_elem_id_t *id) {
    const char* name = snd_ctl_elem_id_get_name(id);

    if (strcmp(name, "Master Playback Volume") == 0)
        return AVRControl::ELEM_VOLUME;
    if (strcmp(name, "Master Playback Switch") == 0)
        return AVRControl::ELEM_MUTE;
    if (strcmp(name, "Master Playback Power") == 0)
        return AVRControl::ELEM_POWER;
    if (strcmp(name, "Standby Timeout") == 0)
        return AVRControl::ELEM_STANDBY_TIMEOUT;
    if (strcmp(name, "Input Source") == 0)
        return AVRControl::ELEM_INPUT;

    // Speaker level controls
    if (strcmp(name, "01 FL Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_FL;
    if (strcmp(name, "02 FR Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_FR;
    if (strcmp(name, "03 C Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_C;
    if (strcmp(name, "04 SW Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SW;
    if (strcmp(name, "05 SL Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SL;
    if (strcmp(name, "06 SR Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SR;
    if (strcmp(name, "07 SBL Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SBL;
    if (strcmp(name, "08 SBR Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SBR;
    if (strcmp(name, "09 SB Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_SB;
    if (strcmp(name, "10 FHL Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_FHL;
    if (strcmp(name, "11 FHR Playback Volume") == 0)
        return AVRControl::ELEM_SPEAKER_FHR;

    return SND_CTL_EXT_KEY_NOT_FOUND;
}

static int avr_get_attribute(snd_ctl_ext_t * /*ext*/, snd_ctl_ext_key_t key,
                             int *type, unsigned int *acc, unsigned int *count) {
    switch (key) {
    case AVRControl::ELEM_VOLUME:
        *type = SND_CTL_ELEM_TYPE_INTEGER;
        *acc = SND_CTL_EXT_ACCESS_READWRITE | SND_CTL_EXT_ACCESS_TLV_READ;
        *count = 1;
        return 0;
    case AVRControl::ELEM_MUTE:
    case AVRControl::ELEM_POWER:
    case AVRControl::ELEM_STANDBY_TIMEOUT:
        *type = SND_CTL_ELEM_TYPE_BOOLEAN;
        *acc = SND_CTL_EXT_ACCESS_READWRITE;
        *count = 1;
        return 0;
    case AVRControl::ELEM_INPUT:
        *type = SND_CTL_ELEM_TYPE_ENUMERATED;
        *acc = SND_CTL_EXT_ACCESS_READWRITE;
        *count = 1;
        return 0;
    case AVRControl::ELEM_SPEAKER_FL:
    case AVRControl::ELEM_SPEAKER_FR:
    case AVRControl::ELEM_SPEAKER_C:
    case AVRControl::ELEM_SPEAKER_SW:
    case AVRControl::ELEM_SPEAKER_SL:
    case AVRControl::ELEM_SPEAKER_SR:
    case AVRControl::ELEM_SPEAKER_SBL:
    case AVRControl::ELEM_SPEAKER_SBR:
    case AVRControl::ELEM_SPEAKER_SB:
    case AVRControl::ELEM_SPEAKER_FHL:
    case AVRControl::ELEM_SPEAKER_FHR:
        *type = SND_CTL_ELEM_TYPE_INTEGER;
        *acc = SND_CTL_EXT_ACCESS_READWRITE;
        *count = 1;
        return 0;
    }
    return -EINVAL;
}

static int avr_get_integer_info(snd_ctl_ext_t * /*ext*/, snd_ctl_ext_key_t key,
                                long *imin, long *imax, long *istep) {
    if (key == AVRControl::ELEM_VOLUME) {
        *imin = 0;
        *imax = 100;
        *istep = 1;
        return 0;
    }

    // Speaker level controls: 38-62 (representing -12dB to +12dB, 50 = 0dB)
    if (key >= AVRControl::ELEM_SPEAKER_FL && key <= AVRControl::ELEM_SPEAKER_FHR) {
        *imin = 38;
        *imax = 62;
        *istep = 1;
        return 0;
    }

    return -EINVAL;
}

static int avr_get_enumerated_info(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *items) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    if (key != AVRControl::ELEM_INPUT)
        return -EINVAL;

    *items = ctrl->available_inputs.size();
    return 0;
}

static int avr_get_enumerated_name(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int item, char *name, size_t name_max_len) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    if (key != AVRControl::ELEM_INPUT)
        return -EINVAL;

    if (item >= ctrl->available_inputs.size())
        return -EINVAL;

    strncpy(name, ctrl->available_inputs[item].c_str(), name_max_len - 1);
    name[name_max_len - 1] = '\0';
    return 0;
}

static AlsaAVR::Speaker elem_to_speaker(snd_ctl_ext_key_t key) {
    switch (key) {
    case AVRControl::ELEM_SPEAKER_FL: return AlsaAVR::Speaker::FRONT_LEFT;
    case AVRControl::ELEM_SPEAKER_FR: return AlsaAVR::Speaker::FRONT_RIGHT;
    case AVRControl::ELEM_SPEAKER_C: return AlsaAVR::Speaker::CENTER;
    case AVRControl::ELEM_SPEAKER_SW: return AlsaAVR::Speaker::SUBWOOFER;
    case AVRControl::ELEM_SPEAKER_SL: return AlsaAVR::Speaker::SURROUND_LEFT;
    case AVRControl::ELEM_SPEAKER_SR: return AlsaAVR::Speaker::SURROUND_RIGHT;
    case AVRControl::ELEM_SPEAKER_SBL: return AlsaAVR::Speaker::SURROUND_BACK_LEFT;
    case AVRControl::ELEM_SPEAKER_SBR: return AlsaAVR::Speaker::SURROUND_BACK_RIGHT;
    case AVRControl::ELEM_SPEAKER_SB: return AlsaAVR::Speaker::SURROUND_BACK;
    case AVRControl::ELEM_SPEAKER_FHL: return AlsaAVR::Speaker::FRONT_HEIGHT_LEFT;
    case AVRControl::ELEM_SPEAKER_FHR: return AlsaAVR::Speaker::FRONT_HEIGHT_RIGHT;
    default: return AlsaAVR::Speaker::FRONT_LEFT; // Fallback
    }
}

static int avr_read_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    switch (key) {
    case AVRControl::ELEM_VOLUME:
        if (!ctrl->volume) return -EAGAIN;  // State unknown
        *value = *ctrl->volume;
        return 0;
    case AVRControl::ELEM_MUTE:
        if (!ctrl->mute) return -EAGAIN;  // State unknown
        *value = *ctrl->mute ? 1 : 0;
        return 0;
    case AVRControl::ELEM_POWER:
        if (!ctrl->power) return -EAGAIN;  // State unknown
        *value = *ctrl->power ? 1 : 0;
        return 0;
    case AVRControl::ELEM_STANDBY_TIMEOUT:
        if (!ctrl->standby_timeout) return -EAGAIN;  // State unknown
        *value = *ctrl->standby_timeout ? 1 : 0;
        return 0;
    }

    // Speaker level controls
    if (key >= AVRControl::ELEM_SPEAKER_FL && key <= AVRControl::ELEM_SPEAKER_FHR) {
        AlsaAVR::Speaker speaker = elem_to_speaker(key);
        auto it = ctrl->speaker_levels.find(speaker);
        if (it != ctrl->speaker_levels.end()) {
            *value = it->second;
            return 0;
        } else {
            // Speaker not initialized - should not happen for valid offsets
            return -ENXIO;
        }
    }

    return -EINVAL;
}

static int avr_write_integer(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, long *value) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    if (!ctrl->backend || !ctrl->backend->is_connected()) {
        return -EIO;
    }

    switch (key) {
    case AVRControl::ELEM_VOLUME:
        if (*value < 0 || *value > 100)
            return -EINVAL;
        if (ctrl->backend->set_volume_absolute(*value)) {
            ctrl->volume = *value;
            return 1; // Value changed
        }
        return -EIO;

    case AVRControl::ELEM_MUTE:
        if (ctrl->backend->set_mute(*value != 0)) {
            ctrl->mute = (*value != 0);
            return 1;
        }
        return -EIO;

    case AVRControl::ELEM_POWER:
        if (ctrl->backend->set_power(*value != 0)) {
            ctrl->power = (*value != 0);
            return 1;
        }
        return -EIO;

    case AVRControl::ELEM_STANDBY_TIMEOUT:
        if (ctrl->backend->set_standby_timeout(*value != 0)) {
            ctrl->standby_timeout = (*value != 0);
            return 1;
        }
        return -EIO;
    }

    // Speaker level controls
    if (key >= AVRControl::ELEM_SPEAKER_FL && key <= AVRControl::ELEM_SPEAKER_FHR) {
        if (*value < 38 || *value > 62)
            return -EINVAL;

        AlsaAVR::Speaker speaker = elem_to_speaker(key);
        if (ctrl->backend->set_speaker_level(speaker, *value)) {
            ctrl->speaker_levels[speaker] = *value;
            return 1; // Value changed
        }
        return -EIO;
    }

    return -EINVAL;
}

static int avr_read_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *item) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    if (key != AVRControl::ELEM_INPUT)
        return -EINVAL;

    if (!ctrl->current_input_index) return -EAGAIN;  // State unknown
    *item = *ctrl->current_input_index;
    return 0;
}

static int avr_write_enumerated(snd_ctl_ext_t *ext, snd_ctl_ext_key_t key, unsigned int *item) {
    AVRControl* ctrl = reinterpret_cast<AVRControl*>(ext->private_data);

    if (key != AVRControl::ELEM_INPUT)
        return -EINVAL;

    if (!ctrl->backend || !ctrl->backend->is_connected())
        return -EIO;

    if (*item >= ctrl->available_inputs.size())
        return -EINVAL;

    const std::string& input = ctrl->available_inputs[*item];
    if (ctrl->backend->set_input(input)) {
        ctrl->current_input_index = static_cast<int>(*item);
        return 1; // Value changed
    }
    return -EIO;
}

static int avr_read_event(snd_ctl_ext_t * /*ext*/, snd_ctl_elem_id_t * /*id*/, unsigned int * /*event_mask*/) {
    // No event support in this version
    return -EAGAIN;
}

} // namespace AlsaAVR
