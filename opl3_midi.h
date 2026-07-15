/*
 * MIDI sequencer and voice allocator for the public Nuked OPL3 interface.
 *
 * This layer deliberately treats opl3_chip as opaque storage: implementation
 * code calls only functions declared by opl3.h and never accesses chip fields.
 */
#ifndef OPL3_MIDI_H
#define OPL3_MIDI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OPL3_MIDI_CHANNEL_COUNT 16u
#define OPL3_MIDI_VOICE_COUNT   18u
#define OPL3_MIDI_SCOPE_COUNT   OPL3_MIDI_VOICE_COUNT
#define OPL3_MIDI_TITLE_LENGTH  128u

typedef struct opl3_midi_player opl3_midi_player;

typedef enum opl3_midi_mode {
    OPL3_MIDI_MODE_GM1 = 0,
    OPL3_MIDI_MODE_GM2,
    OPL3_MIDI_MODE_GS_COMPAT,
    OPL3_MIDI_MODE_XG_COMPAT
} opl3_midi_mode;

typedef enum opl3_midi_program_family {
    OPL3_MIDI_FAMILY_PIANO = 0,
    OPL3_MIDI_FAMILY_CHROMATIC_PERCUSSION,
    OPL3_MIDI_FAMILY_ORGAN,
    OPL3_MIDI_FAMILY_GUITAR,
    OPL3_MIDI_FAMILY_BASS,
    OPL3_MIDI_FAMILY_STRINGS,
    OPL3_MIDI_FAMILY_ENSEMBLE,
    OPL3_MIDI_FAMILY_BRASS,
    OPL3_MIDI_FAMILY_REED,
    OPL3_MIDI_FAMILY_PIPE,
    OPL3_MIDI_FAMILY_SYNTH_LEAD,
    OPL3_MIDI_FAMILY_SYNTH_PAD,
    OPL3_MIDI_FAMILY_SYNTH_EFFECTS,
    OPL3_MIDI_FAMILY_ETHNIC,
    OPL3_MIDI_FAMILY_PERCUSSIVE,
    OPL3_MIDI_FAMILY_SOUND_EFFECTS,
    OPL3_MIDI_FAMILY_DRUMS
} opl3_midi_program_family;

typedef struct opl3_midi_info {
    uint16_t format;
    uint16_t track_count;
    uint16_t division;
    size_t event_count;
    uint64_t duration_frames;
    double duration_seconds;
    /* declared_mode is meaningful only when has_mode_reset is nonzero. */
    int has_mode_reset;
    opl3_midi_mode declared_mode;
    uint16_t used_channel_mask;
    uint32_t instrument_stream_count;
    uint8_t scope_count;
    uint8_t scope_programs[OPL3_MIDI_SCOPE_COUNT];
    uint16_t scope_tracks[OPL3_MIDI_SCOPE_COUNT];
    uint8_t scope_channels[OPL3_MIDI_SCOPE_COUNT];
    uint8_t scope_lanes[OPL3_MIDI_SCOPE_COUNT];
    char title[OPL3_MIDI_TITLE_LENGTH];
} opl3_midi_info;

typedef struct opl3_midi_snapshot {
    uint64_t position_frames;
    uint64_t duration_frames;
    uint32_t sample_rate;
    unsigned active_voice_count;
    int loaded;
    int finished;
    int loop_enabled;
    float gain;
    opl3_midi_mode mode;
    uint8_t programs[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t bank_msb[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t bank_lsb[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t pending_bank_msb[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t pending_bank_lsb[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_volume[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_expression[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_pan[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_modulation[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_harmonic_content[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_release_time[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_attack_time[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_brightness[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_decay_time[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t channel_sustain[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t bend_semitones[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t bend_cents[OPL3_MIDI_CHANNEL_COUNT];
    int16_t pitch_bend[OPL3_MIDI_CHANNEL_COUNT];
    uint8_t voice_note[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_channel[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_velocity[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_active[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_released[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_key_down[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_sustained[OPL3_MIDI_VOICE_COUNT];
    uint8_t voice_percussion[OPL3_MIDI_VOICE_COUNT];
    uint8_t scope_active[OPL3_MIDI_SCOPE_COUNT];
    uint8_t scope_note[OPL3_MIDI_SCOPE_COUNT];
    uint8_t scope_percussion[OPL3_MIDI_SCOPE_COUNT];
} opl3_midi_snapshot;

/* sample_rate must be in the range 8000..192000 Hz. */
opl3_midi_player *opl3_midi_create(uint32_t sample_rate);
void opl3_midi_destroy(opl3_midi_player *player);

/* Loading is transactional: an error leaves any previously loaded song intact. */
int opl3_midi_load_memory(opl3_midi_player *player,
                          const void *data,
                          size_t size);
int opl3_midi_load_file(opl3_midi_player *player, const char *path);
void opl3_midi_unload(opl3_midi_player *player);

/*
 * Render interleaved signed 16-bit stereo frames. The buffer is always filled.
 * This function performs no allocation and is suitable for an audio callback.
 */
void opl3_midi_render(opl3_midi_player *player,
                      int16_t *stereo_output,
                      size_t frame_count);

/*
 * Render stereo plus 18 isolated two-operator FM voice taps. voice_output is
 * channel-major: voice_output[voice * frame_count + frame]. Voice capture is
 * enabled automatically and performs no allocation in the render callback.
 */
void opl3_midi_render_voices(opl3_midi_player *player,
                             int16_t *stereo_output,
                             int16_t *voice_output,
                             size_t frame_count);

/*
 * Render stereo plus 18 deterministic scope lanes. Lanes are fixed at load
 * time by (program, source track, MIDI channel, chord lane), in that order.
 * scope_output is channel-major: scope_output[scope * frame_count + frame].
 */
void opl3_midi_render_scopes(opl3_midi_player *player,
                             int16_t *stereo_output,
                             int16_t *scope_output,
                             size_t frame_count);

/* Seeking reconstructs MIDI state at the target; held envelopes restart there. */
int opl3_midi_seek_frames(opl3_midi_player *player, uint64_t target_frame);
int opl3_midi_seek_seconds(opl3_midi_player *player, double seconds);
void opl3_midi_rewind(opl3_midi_player *player);

void opl3_midi_set_loop(opl3_midi_player *player, int enabled);
void opl3_midi_set_gain(opl3_midi_player *player, float gain);
/* Enabling during playback reconstructs state at the current song position. */
void opl3_midi_set_voice_capture(opl3_midi_player *player, int enabled);
void opl3_midi_set_scope_capture(opl3_midi_player *player, int enabled);

int opl3_midi_get_info(const opl3_midi_player *player, opl3_midi_info *info);
void opl3_midi_get_snapshot(const opl3_midi_player *player,
                            opl3_midi_snapshot *snapshot);
const char *opl3_midi_last_error(const opl3_midi_player *player);

const char *opl3_midi_mode_name(opl3_midi_mode mode);
const char *opl3_midi_program_name(unsigned program);
opl3_midi_program_family opl3_midi_program_family_for(unsigned program);
const char *opl3_midi_program_family_name(opl3_midi_program_family family);

#ifdef __cplusplus
}
#endif

#endif
