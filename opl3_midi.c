#include "opl3_midi.h"
#include "opl3.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MIDI_ERROR_LENGTH       256u
#define MIDI_MAX_FILE_SIZE      (256u * 1024u * 1024u)
#define MIDI_RELEASE_SECONDS    2u
#define MIDI_TAIL_SECONDS       3u
#define MIDI_CAPTURE_RENDER_CHUNK 1024u
#define OPL3_NATIVE_RATE        49716.0

typedef enum midi_event_kind {
    MIDI_EVENT_NOTE_OFF = 0,
    MIDI_EVENT_NOTE_ON,
    MIDI_EVENT_POLY_PRESSURE,
    MIDI_EVENT_CONTROL,
    MIDI_EVENT_PROGRAM,
    MIDI_EVENT_CHANNEL_PRESSURE,
    MIDI_EVENT_PITCH_BEND,
    MIDI_EVENT_TEMPO,
    MIDI_EVENT_RESET,
    MIDI_EVENT_END
} midi_event_kind;

typedef struct midi_event {
    uint64_t tick;
    uint64_t frame;
    uint64_t order;
    uint32_t value;
    uint16_t track;
    uint8_t kind;
    uint8_t channel;
    uint8_t a;
    uint8_t b;
    uint8_t stream_program;
    uint8_t scope_first;
    uint8_t scope_count;
} midi_event;

typedef struct midi_song {
    midi_event *events;
    size_t count;
    size_t capacity;
    uint16_t format;
    uint16_t track_count;
    uint16_t division;
    uint64_t duration_frames;
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
} midi_song;

typedef struct scope_note_identity {
    size_t event_index;
    uint64_t tick;
    uint16_t track;
    uint8_t program;
    uint8_t channel;
} scope_note_identity;

typedef struct scope_stream_analysis {
    size_t first_note;
    size_t note_count;
    uint16_t track;
    uint8_t program;
    uint8_t channel;
    uint8_t requested_lanes;
    uint8_t assigned_lanes;
    uint8_t first_scope;
} scope_stream_analysis;

typedef struct midi_reader {
    const uint8_t *cursor;
    const uint8_t *end;
} midi_reader;

typedef struct opl_operator {
    uint8_t multiplier;
    uint8_t level;
    uint8_t key_scale_level;
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain_level;
    uint8_t release;
    uint8_t waveform;
    uint8_t tremolo;
    uint8_t vibrato;
    uint8_t sustained;
    uint8_t key_scale_rate;
} opl_operator;

typedef struct opl_patch {
    opl_operator modulator;
    opl_operator carrier;
    uint8_t feedback;
    uint8_t additive;
    int8_t transpose;
    uint8_t velocity_depth;
} opl_patch;

typedef struct midi_channel_state {
    uint8_t program;
    uint8_t bank_msb;
    uint8_t bank_lsb;
    uint8_t pending_bank_msb;
    uint8_t pending_bank_lsb;
    uint8_t volume;
    uint8_t expression;
    uint8_t pan;
    uint8_t modulation;
    uint8_t harmonic_content;
    uint8_t release_time;
    uint8_t attack_time;
    uint8_t brightness;
    uint8_t decay_time;
    uint8_t sustain;
    uint8_t rpn_msb;
    uint8_t rpn_lsb;
    uint8_t bend_semitones;
    uint8_t bend_cents;
    int16_t pitch_bend;
} midi_channel_state;

typedef struct opl_voice {
    opl_patch patch;
    uint64_t serial;
    uint64_t deadline;
    uint8_t active;
    uint8_t released;
    uint8_t key_down;
    uint8_t sustained;
    uint8_t percussion;
    uint8_t exclusive_group;
    uint16_t track;
    uint8_t channel;
    uint8_t scope_slot;
    uint8_t note;
    uint8_t pitch_note;
    uint8_t velocity;
    uint8_t fnum_low;
    uint8_t block_fnum_high;
} opl_voice;

struct opl3_midi_player {
    opl3_chip chip;
    opl3_chip voice_chips[OPL3_MIDI_VOICE_COUNT];
    opl3_chip scope_chips[OPL3_MIDI_SCOPE_COUNT];
    midi_song song;
    midi_channel_state channels[OPL3_MIDI_CHANNEL_COUNT];
    opl_voice voices[OPL3_MIDI_VOICE_COUNT];
    size_t next_event;
    uint64_t current_frame;
    uint64_t voice_serial;
    uint32_t sample_rate;
    float gain;
    opl3_midi_mode mode;
    int loaded;
    int finished;
    int loop_enabled;
    int voice_capture_enabled;
    int scope_capture_enabled;
    char error[MIDI_ERROR_LENGTH];
};

static const uint8_t opl_operator_offset[9] = {
    0u, 1u, 2u, 8u, 9u, 10u, 16u, 17u, 18u
};

static const char *const gm_program_names[128] = {
    "Acoustic Grand Piano", "Bright Acoustic Piano",
    "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1",
    "Electric Piano 2", "Harpsichord", "Clavinet", "Celesta",
    "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone",
    "Tubular Bells", "Dulcimer", "Drawbar Organ", "Percussive Organ",
    "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica",
    "Tango Accordion", "Acoustic Guitar (nylon)",
    "Acoustic Guitar (steel)", "Electric Guitar (jazz)",
    "Electric Guitar (clean)", "Electric Guitar (muted)",
    "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
    "Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)",
    "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1",
    "Synth Bass 2", "Violin", "Viola", "Cello", "Contrabass",
    "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
    "String Ensemble 1", "String Ensemble 2", "Synth Strings 1",
    "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice",
    "Orchestra Hit", "Trumpet", "Trombone", "Tuba", "Muted Trumpet",
    "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
    "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe",
    "English Horn", "Bassoon", "Clarinet", "Piccolo", "Flute", "Recorder",
    "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
    "Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)",
    "Lead 4 (chiff)", "Lead 5 (charang)", "Lead 6 (voice)",
    "Lead 7 (fifths)", "Lead 8 (bass + lead)", "Pad 1 (new age)",
    "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)",
    "Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
    "FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)",
    "FX 4 (atmosphere)", "FX 5 (brightness)", "FX 6 (goblins)",
    "FX 7 (echoes)", "FX 8 (sci-fi)", "Sitar", "Banjo", "Shamisen",
    "Koto", "Kalimba", "Bag Pipe", "Fiddle", "Shanai", "Tinkle Bell",
    "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom",
    "Synth Drum", "Reverse Cymbal", "Guitar Fret Noise", "Breath Noise",
    "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause",
    "Gunshot"
};

static void set_error_text(char *error, const char *text)
{
    if (error == NULL) {
        return;
    }
    (void)snprintf(error, MIDI_ERROR_LENGTH, "%s", text != NULL ? text : "");
}

static void set_error_format(char *error, const char *prefix, const char *detail)
{
    if (error == NULL) {
        return;
    }
    (void)snprintf(error, MIDI_ERROR_LENGTH, "%s%s", prefix, detail);
}

static uint16_t read_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint32_t read_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
           | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int reader_vlq(midi_reader *reader, uint32_t *value)
{
    uint32_t result = 0;
    unsigned i;

    for (i = 0; i < 4; ++i) {
        uint8_t byte;
        if (reader->cursor >= reader->end) {
            return 0;
        }
        byte = *reader->cursor++;
        result = (result << 7) | (uint32_t)(byte & 0x7fu);
        if ((byte & 0x80u) == 0u) {
            *value = result;
            return 1;
        }
    }
    return 0;
}

static void song_free(midi_song *song)
{
    if (song == NULL) {
        return;
    }
    free(song->events);
    memset(song, 0, sizeof(*song));
}

static int song_append(midi_song *song, const midi_event *event, char *error)
{
    midi_event *resized;
    size_t new_capacity;

    if (song->count == song->capacity) {
        new_capacity = song->capacity == 0u ? 256u : song->capacity * 2u;
        if (new_capacity < song->capacity
            || new_capacity > SIZE_MAX / sizeof(*song->events)) {
            set_error_text(error, "MIDI event list is too large");
            return 0;
        }
        resized = (midi_event *)realloc(song->events,
                                        new_capacity * sizeof(*song->events));
        if (resized == NULL) {
            set_error_text(error, "Out of memory while reading MIDI events");
            return 0;
        }
        song->events = resized;
        song->capacity = new_capacity;
    }
    song->events[song->count++] = *event;
    return 1;
}

static void copy_meta_title(char *destination,
                            const uint8_t *source,
                            size_t length)
{
    size_t i;
    size_t copy_length;

    if (destination[0] != '\0') {
        return;
    }
    copy_length = length;
    if (copy_length >= OPL3_MIDI_TITLE_LENGTH) {
        copy_length = OPL3_MIDI_TITLE_LENGTH - 1u;
    }
    for (i = 0; i < copy_length; ++i) {
        uint8_t byte = source[i];
        destination[i] = (byte >= 32u && byte <= 126u) ? (char)byte : ' ';
    }
    while (copy_length > 0u && destination[copy_length - 1u] == ' ') {
        --copy_length;
    }
    destination[copy_length] = '\0';
}

static int detect_reset_sysex(const uint8_t *data,
                              size_t length,
                              opl3_midi_mode *mode)
{
    if (length == 5u && data[0] == 0x7eu && data[1] <= 0x7fu
        && data[2] == 0x09u
        && data[4] == 0xf7u) {
        if (data[3] == 0x01u) {
            *mode = OPL3_MIDI_MODE_GM1;
            return 1;
        }
        if (data[3] == 0x03u) {
            *mode = OPL3_MIDI_MODE_GM2;
            return 1;
        }
    }

    if (length == 10u && data[0] == 0x41u
        && (data[1] <= 0x1fu || data[1] == 0x7fu) && data[2] == 0x42u
        && data[3] == 0x12u && data[4] == 0x40u && data[5] == 0x00u
        && data[6] == 0x7fu && data[7] == 0x00u && data[8] == 0x41u
        && data[9] == 0xf7u) {
        *mode = OPL3_MIDI_MODE_GS_COMPAT;
        return 1;
    }

    if (length == 8u && data[0] == 0x43u && (data[1] & 0xf0u) == 0x10u
        && data[2] == 0x4cu && data[3] == 0x00u && data[4] == 0x00u
        && data[5] == 0x7eu && data[6] == 0x00u && data[7] == 0xf7u) {
        *mode = OPL3_MIDI_MODE_XG_COMPAT;
        return 1;
    }

    return 0;
}

static int append_channel_event(midi_song *song,
                                uint64_t tick,
                                uint64_t order,
                                uint16_t track,
                                uint8_t status,
                                uint8_t data_a,
                                uint8_t data_b,
                                char *error)
{
    midi_event event;
    uint8_t command = status & 0xf0u;

    memset(&event, 0, sizeof(event));
    event.tick = tick;
    event.order = order;
    event.track = track;
    event.channel = status & 0x0fu;
    event.a = data_a;
    event.b = data_b;
    if (command == 0x90u && data_b != 0u) {
        song->used_channel_mask |= (uint16_t)(1u << event.channel);
    }

    switch (command) {
    case 0x80u:
        event.kind = MIDI_EVENT_NOTE_OFF;
        break;
    case 0x90u:
        event.kind = data_b == 0u ? MIDI_EVENT_NOTE_OFF : MIDI_EVENT_NOTE_ON;
        break;
    case 0xa0u:
        event.kind = MIDI_EVENT_POLY_PRESSURE;
        break;
    case 0xb0u:
        event.kind = MIDI_EVENT_CONTROL;
        break;
    case 0xc0u:
        event.kind = MIDI_EVENT_PROGRAM;
        break;
    case 0xd0u:
        event.kind = MIDI_EVENT_CHANNEL_PRESSURE;
        break;
    case 0xe0u:
        event.kind = MIDI_EVENT_PITCH_BEND;
        event.value = (uint32_t)data_a | ((uint32_t)data_b << 7);
        break;
    default:
        set_error_text(error, "Invalid MIDI channel event");
        return 0;
    }
    return song_append(song, &event, error);
}

static int parse_track(midi_song *song,
                       const uint8_t *data,
                       size_t length,
                       uint16_t track_index,
                       uint64_t *global_order,
                       uint64_t *song_end_tick,
                       char *error)
{
    midi_reader reader;
    uint64_t tick = 0;
    uint8_t running_status = 0;
    int saw_end = 0;

    reader.cursor = data;
    reader.end = data + length;

    while (reader.cursor < reader.end && !saw_end) {
        uint32_t delta;
        uint8_t status;
        uint8_t first_data = 0;
        int have_first_data = 0;

        if (!reader_vlq(&reader, &delta)) {
            set_error_text(error, "Invalid or truncated MIDI delta time");
            return 0;
        }
        if (UINT64_MAX - tick < (uint64_t)delta) {
            set_error_text(error, "MIDI tick position overflow");
            return 0;
        }
        tick += (uint64_t)delta;
        if (tick > *song_end_tick) {
            *song_end_tick = tick;
        }
        if (reader.cursor >= reader.end) {
            set_error_text(error, "MIDI track ends after a delta time");
            return 0;
        }

        if (*reader.cursor < 0x80u) {
            if (running_status < 0x80u || running_status >= 0xf0u) {
                set_error_text(error, "MIDI running status has no channel status");
                return 0;
            }
            status = running_status;
            first_data = *reader.cursor++;
            have_first_data = 1;
        } else {
            status = *reader.cursor++;
            if (status < 0xf0u) {
                running_status = status;
            } else if (status < 0xf8u) {
                running_status = 0;
            }
        }

        if (status == 0xffu) {
            uint8_t meta_type;
            uint32_t meta_length;
            midi_event event;

            if (reader.cursor >= reader.end) {
                set_error_text(error, "Truncated MIDI meta-event type");
                return 0;
            }
            meta_type = *reader.cursor++;
            if (!reader_vlq(&reader, &meta_length)
                || (size_t)(reader.end - reader.cursor) < (size_t)meta_length) {
                set_error_text(error, "Invalid or truncated MIDI meta-event");
                return 0;
            }

            if (meta_type == 0x03u && meta_length > 0u) {
                copy_meta_title(song->title, reader.cursor, meta_length);
            } else if (meta_type == 0x51u) {
                if (meta_length != 3u) {
                    set_error_text(error, "Set Tempo meta-event must contain 3 bytes");
                    return 0;
                }
                memset(&event, 0, sizeof(event));
                event.tick = tick;
                event.order = (*global_order)++;
                event.track = track_index;
                event.kind = MIDI_EVENT_TEMPO;
                event.value = ((uint32_t)reader.cursor[0] << 16)
                              | ((uint32_t)reader.cursor[1] << 8)
                              | (uint32_t)reader.cursor[2];
                if (event.value == 0u) {
                    set_error_text(error, "Set Tempo value cannot be zero");
                    return 0;
                }
                if (!song_append(song, &event, error)) {
                    return 0;
                }
            } else if (meta_type == 0x2fu) {
                if (meta_length != 0u) {
                    set_error_text(error, "End of Track meta-event must be empty");
                    return 0;
                }
                saw_end = 1;
            }
            reader.cursor += meta_length;
            continue;
        }

        if (status == 0xf0u || status == 0xf7u) {
            uint32_t sysex_length;
            opl3_midi_mode reset_mode;

            if (!reader_vlq(&reader, &sysex_length)
                || (size_t)(reader.end - reader.cursor) < (size_t)sysex_length) {
                set_error_text(error, "Invalid or truncated MIDI SysEx event");
                return 0;
            }
            if (status == 0xf0u
                && detect_reset_sysex(reader.cursor, sysex_length, &reset_mode)) {
                midi_event event;
                memset(&event, 0, sizeof(event));
                event.tick = tick;
                event.order = (*global_order)++;
                event.track = track_index;
                event.kind = MIDI_EVENT_RESET;
                event.value = (uint32_t)reset_mode;
                if (!song_append(song, &event, error)) {
                    return 0;
                }
            }
            reader.cursor += sysex_length;
            continue;
        }

        if (status >= 0xf0u) {
            set_error_text(error,
                           "System status is invalid in an SMF track; use an F7 escaped event");
            return 0;
        }

        {
            uint8_t command = status & 0xf0u;
            unsigned data_length = (command == 0xc0u || command == 0xd0u) ? 1u : 2u;
            uint8_t data_a;
            uint8_t data_b = 0;

            if (have_first_data) {
                data_a = first_data;
            } else {
                if (reader.cursor >= reader.end) {
                    set_error_text(error, "Truncated MIDI channel event");
                    return 0;
                }
                data_a = *reader.cursor++;
            }
            if ((data_a & 0x80u) != 0u) {
                set_error_text(error, "MIDI channel data byte has its status bit set");
                return 0;
            }
            if (data_length == 2u) {
                if (reader.cursor >= reader.end) {
                    set_error_text(error, "Truncated two-byte MIDI channel event");
                    return 0;
                }
                data_b = *reader.cursor++;
                if ((data_b & 0x80u) != 0u) {
                    set_error_text(error, "MIDI channel data byte has its status bit set");
                    return 0;
                }
            }
            if (!append_channel_event(song, tick, (*global_order)++,
                                      track_index, status,
                                      data_a, data_b, error)) {
                return 0;
            }
        }
    }

    return 1;
}

static int midi_event_compare(const void *left, const void *right)
{
    const midi_event *a = (const midi_event *)left;
    const midi_event *b = (const midi_event *)right;
    if (a->tick < b->tick) {
        return -1;
    }
    if (a->tick > b->tick) {
        return 1;
    }
    if (a->order < b->order) {
        return -1;
    }
    if (a->order > b->order) {
        return 1;
    }
    return 0;
}

static int scope_note_compare(const void *left, const void *right)
{
    const scope_note_identity *a = (const scope_note_identity *)left;
    const scope_note_identity *b = (const scope_note_identity *)right;
    if (a->program != b->program) return a->program < b->program ? -1 : 1;
    if (a->track != b->track) return a->track < b->track ? -1 : 1;
    if (a->channel != b->channel) return a->channel < b->channel ? -1 : 1;
    if (a->tick != b->tick) return a->tick < b->tick ? -1 : 1;
    if (a->event_index != b->event_index) {
        return a->event_index < b->event_index ? -1 : 1;
    }
    return 0;
}

static int same_scope_stream(const scope_note_identity *a,
                             const scope_note_identity *b)
{
    return a->program == b->program && a->track == b->track
           && a->channel == b->channel;
}

static int analyze_scope_streams(midi_song *song, char *error)
{
    scope_note_identity *notes = NULL;
    scope_stream_analysis *streams = NULL;
    uint8_t *track_programs = NULL;
    size_t note_count = 0u;
    size_t stream_count = 0u;
    size_t i;
    size_t note_index;
    size_t stream_index;
    unsigned remaining;
    unsigned lane_level;
    unsigned scope = 0u;

    memset(song->scope_programs, 0, sizeof(song->scope_programs));
    memset(song->scope_tracks, 0, sizeof(song->scope_tracks));
    memset(song->scope_channels, 0, sizeof(song->scope_channels));
    memset(song->scope_lanes, 0, sizeof(song->scope_lanes));
    song->scope_count = 0u;
    song->instrument_stream_count = 0u;
    for (i = 0u; i < song->count; ++i) {
        song->events[i].scope_first = UINT8_MAX;
        song->events[i].scope_count = 0u;
        if (song->events[i].kind == MIDI_EVENT_NOTE_ON) ++note_count;
    }
    if (note_count == 0u) return 1;
    if (note_count > SIZE_MAX / sizeof(*notes)) {
        set_error_text(error, "MIDI has too many note events to analyze scopes");
        return 0;
    }
    notes = (scope_note_identity *)malloc(note_count * sizeof(*notes));
    if (notes == NULL) {
        set_error_text(error, "Out of memory while arranging scope streams");
        return 0;
    }

    track_programs = (uint8_t *)calloc(
        (size_t)song->track_count * OPL3_MIDI_CHANNEL_COUNT,
        sizeof(*track_programs));
    if (track_programs == NULL) {
        free(notes);
        set_error_text(error, "Out of memory while resolving track programs");
        return 0;
    }

    note_index = 0u;
    for (i = 0u; i < song->count; ++i) {
        midi_event *event = &song->events[i];
        if (event->kind == MIDI_EVENT_RESET) {
            memset(track_programs, 0,
                   (size_t)song->track_count * OPL3_MIDI_CHANNEL_COUNT);
        } else if (event->kind == MIDI_EVENT_PROGRAM) {
            if (event->track < song->track_count) {
                track_programs[(size_t)event->track * OPL3_MIDI_CHANNEL_COUNT
                               + event->channel] = event->a;
            }
        } else if (event->kind == MIDI_EVENT_NOTE_ON) {
            scope_note_identity *identity = &notes[note_index++];
            uint8_t program = event->track < song->track_count
                            ? track_programs[(size_t)event->track
                                             * OPL3_MIDI_CHANNEL_COUNT
                                             + event->channel] : 0u;
            identity->event_index = i;
            identity->tick = event->tick;
            identity->track = event->track;
            identity->program = program;
            identity->channel = event->channel;
            event->stream_program = program;
        }
    }
    free(track_programs);
    track_programs = NULL;
    qsort(notes, note_count, sizeof(*notes), scope_note_compare);
    for (i = 0u; i < note_count; ++i) {
        if (i == 0u || !same_scope_stream(&notes[i - 1u], &notes[i])) {
            ++stream_count;
        }
    }
    if (stream_count > UINT32_MAX
        || stream_count > SIZE_MAX / sizeof(*streams)) {
        free(notes);
        set_error_text(error, "MIDI has too many distinct instrument streams");
        return 0;
    }
    streams = (scope_stream_analysis *)calloc(stream_count, sizeof(*streams));
    if (streams == NULL) {
        free(notes);
        set_error_text(error, "Out of memory while categorizing scope streams");
        return 0;
    }

    note_index = 0u;
    stream_index = 0u;
    while (note_index < note_count) {
        size_t end = note_index + 1u;
        size_t chord_start = note_index;
        unsigned widest_chord = 1u;
        scope_stream_analysis *stream = &streams[stream_index++];
        while (end < note_count && same_scope_stream(&notes[note_index],
                                                     &notes[end])) {
            ++end;
        }
        for (i = note_index + 1u; i <= end; ++i) {
            if (i == end || notes[i].tick != notes[chord_start].tick) {
                size_t width = i - chord_start;
                if (width > widest_chord) {
                    widest_chord = width > OPL3_MIDI_SCOPE_COUNT
                                  ? OPL3_MIDI_SCOPE_COUNT : (unsigned)width;
                }
                chord_start = i;
            }
        }
        stream->first_note = note_index;
        stream->note_count = end - note_index;
        stream->track = notes[note_index].track;
        stream->program = notes[note_index].program;
        stream->channel = notes[note_index].channel;
        stream->requested_lanes = (uint8_t)widest_chord;
        stream->first_scope = UINT8_MAX;
        note_index = end;
    }

    song->instrument_stream_count = (uint32_t)stream_count;
    for (i = 0u; i < stream_count && i < OPL3_MIDI_SCOPE_COUNT; ++i) {
        streams[i].assigned_lanes = 1u;
    }
    remaining = stream_count < OPL3_MIDI_SCOPE_COUNT
              ? OPL3_MIDI_SCOPE_COUNT - (unsigned)stream_count : 0u;
    for (lane_level = 2u; remaining != 0u
         && lane_level <= OPL3_MIDI_SCOPE_COUNT; ++lane_level) {
        for (i = 0u; i < stream_count && i < OPL3_MIDI_SCOPE_COUNT
             && remaining != 0u; ++i) {
            if (streams[i].requested_lanes >= lane_level) {
                ++streams[i].assigned_lanes;
                --remaining;
            }
        }
    }

    for (stream_index = 0u; stream_index < stream_count; ++stream_index) {
        scope_stream_analysis *stream = &streams[stream_index];
        unsigned lane;
        if (stream->assigned_lanes != 0u) {
            stream->first_scope = (uint8_t)scope;
            for (lane = 0u; lane < stream->assigned_lanes; ++lane) {
                song->scope_programs[scope] = stream->program;
                song->scope_tracks[scope] = stream->track;
                song->scope_channels[scope] = stream->channel;
                song->scope_lanes[scope] = (uint8_t)lane;
                ++scope;
            }
        }
        for (i = stream->first_note;
             i < stream->first_note + stream->note_count; ++i) {
            midi_event *event = &song->events[notes[i].event_index];
            event->scope_first = stream->first_scope;
            event->scope_count = stream->assigned_lanes;
        }
    }
    song->scope_count = (uint8_t)scope;
    free(streams);
    free(notes);
    return 1;
}

static int assign_event_frames(midi_song *song,
                               uint32_t sample_rate,
                               char *error)
{
    size_t i;
    long double exact_frame = 0.0L;
    uint64_t previous_tick = 0;
    uint32_t tempo = 500000u;
    int smpte = (song->division & 0x8000u) != 0u;
    long double frames_per_tick = 0.0L;

    if (smpte) {
        unsigned fps_byte = song->division >> 8;
        int fps_code = fps_byte >= 128u ? (int)fps_byte - 256 : (int)fps_byte;
        unsigned ticks_per_frame = song->division & 0xffu;
        long double frames_per_second;
        if (ticks_per_frame == 0u) {
            set_error_text(error, "SMPTE MIDI division has zero ticks per frame");
            return 0;
        }
        switch (fps_code) {
        case -24:
            frames_per_second = 24.0L;
            break;
        case -25:
            frames_per_second = 25.0L;
            break;
        case -29:
            frames_per_second = 30000.0L / 1001.0L;
            break;
        case -30:
            frames_per_second = 30.0L;
            break;
        default:
            set_error_text(error, "Unsupported SMPTE frame rate in MIDI division");
            return 0;
        }
        frames_per_tick = (long double)sample_rate
                          / (frames_per_second * (long double)ticks_per_frame);
    } else if (song->division == 0u) {
        set_error_text(error, "MIDI PPQN division cannot be zero");
        return 0;
    }

    qsort(song->events, song->count, sizeof(*song->events), midi_event_compare);
    song->has_mode_reset = 0;
    song->declared_mode = OPL3_MIDI_MODE_GM1;

    for (i = 0; i < song->count; ++i) {
        midi_event *event = &song->events[i];
        uint64_t delta = event->tick - previous_tick;
        long double increment;

        if (smpte) {
            increment = (long double)delta * frames_per_tick;
        } else {
            increment = (long double)delta * (long double)tempo
                        * (long double)sample_rate
                        / ((long double)song->division * 1000000.0L);
        }
        exact_frame += increment;
        if (exact_frame > (long double)UINT64_MAX - 1.0L) {
            set_error_text(error, "MIDI duration is too large");
            return 0;
        }
        event->frame = (uint64_t)floorl(exact_frame + 0.5L);
        previous_tick = event->tick;
        if (!smpte && event->kind == MIDI_EVENT_TEMPO) {
            tempo = event->value;
        }
        if (event->kind == MIDI_EVENT_RESET) {
            song->has_mode_reset = 1;
            song->declared_mode = (opl3_midi_mode)event->value;
        }
        if (event->kind == MIDI_EVENT_END) {
            song->duration_frames = event->frame;
        }
    }
    return analyze_scope_streams(song, error);
}

static int unwrap_rmid(const uint8_t **data, size_t *size, char *error)
{
    const uint8_t *bytes = *data;
    size_t length = *size;
    size_t position;
    size_t riff_end;

    if (length < 12u || memcmp(bytes, "RIFF", 4u) != 0) {
        return 1;
    }
    if (memcmp(bytes + 8u, "RMID", 4u) != 0) {
        set_error_text(error, "RIFF input is not an RMID file");
        return 0;
    }
    riff_end = (size_t)read_le32(bytes + 4u) + 8u;
    if (riff_end < 12u || riff_end > length) {
        set_error_text(error, "Truncated RIFF/RMID container");
        return 0;
    }

    position = 12u;
    while (position + 8u <= riff_end) {
        uint32_t chunk_length = read_le32(bytes + position + 4u);
        size_t payload = position + 8u;
        if ((size_t)chunk_length > riff_end - payload) {
            set_error_text(error, "Truncated RIFF/RMID chunk");
            return 0;
        }
        if (memcmp(bytes + position, "data", 4u) == 0) {
            *data = bytes + payload;
            *size = (size_t)chunk_length;
            return 1;
        }
        position = payload + (size_t)chunk_length + ((size_t)chunk_length & 1u);
    }
    set_error_text(error, "RIFF/RMID container has no data chunk");
    return 0;
}

static int parse_midi(const void *input,
                      size_t input_size,
                      uint32_t sample_rate,
                      midi_song *song,
                      char *error)
{
    const uint8_t *data = (const uint8_t *)input;
    size_t size = input_size;
    size_t position;
    uint32_t header_length;
    uint16_t expected_tracks;
    uint16_t parsed_tracks = 0;
    uint64_t order = 0;
    uint64_t end_tick = 0;
    midi_event end_event;

    memset(song, 0, sizeof(*song));
    song->declared_mode = OPL3_MIDI_MODE_GM1;
    if (input == NULL || input_size == 0u) {
        set_error_text(error, "MIDI input is empty");
        return 0;
    }
    if (!unwrap_rmid(&data, &size, error)) {
        return 0;
    }
    if (size < 14u || memcmp(data, "MThd", 4u) != 0) {
        set_error_text(error, "Input does not begin with a valid MThd chunk");
        return 0;
    }
    header_length = read_be32(data + 4u);
    if (header_length < 6u || (size_t)header_length > size - 8u) {
        set_error_text(error, "Invalid or truncated MThd chunk");
        return 0;
    }
    song->format = read_be16(data + 8u);
    expected_tracks = read_be16(data + 10u);
    song->division = read_be16(data + 12u);
    if (song->format > 2u) {
        set_error_text(error, "Unsupported MIDI file format");
        return 0;
    }
    if (song->format == 2u) {
        set_error_text(error, "MIDI format 2 contains independent songs and is not supported");
        return 0;
    }
    if (expected_tracks == 0u) {
        set_error_text(error, "MIDI header declares zero tracks");
        return 0;
    }
    if (song->format == 0u && expected_tracks != 1u) {
        set_error_text(error, "MIDI format 0 must declare exactly one track");
        return 0;
    }

    position = 8u + (size_t)header_length;
    while (position + 8u <= size && parsed_tracks < expected_tracks) {
        uint32_t chunk_length = read_be32(data + position + 4u);
        size_t payload = position + 8u;
        if ((size_t)chunk_length > size - payload) {
            set_error_text(error, "Truncated MIDI chunk");
            song_free(song);
            return 0;
        }
        if (memcmp(data + position, "MTrk", 4u) == 0) {
            if (!parse_track(song, data + payload, (size_t)chunk_length,
                             parsed_tracks, &order, &end_tick, error)) {
                song_free(song);
                return 0;
            }
            ++parsed_tracks;
        }
        position = payload + (size_t)chunk_length;
    }
    if (parsed_tracks != expected_tracks) {
        set_error_text(error, "MIDI file contains fewer tracks than its header declares");
        song_free(song);
        return 0;
    }
    song->track_count = parsed_tracks;

    memset(&end_event, 0, sizeof(end_event));
    end_event.tick = end_tick;
    end_event.order = order;
    end_event.kind = MIDI_EVENT_END;
    if (!song_append(song, &end_event, error)
        || !assign_event_frames(song, sample_rate, error)) {
        song_free(song);
        return 0;
    }
    return 1;
}

#define OP(mult, lev, ksl, ar, dr, sl, rr, wave, trem, vib, sus, ksr) \
    { (mult), (lev), (ksl), (ar), (dr), (sl), (rr), (wave), \
      (trem), (vib), (sus), (ksr) }
#define PATCH(mod, car, fb, add, trans, veldepth) \
    { mod, car, (fb), (add), (trans), (veldepth) }

enum patch_id {
    PATCH_PIANO = 0,
    PATCH_BRIGHT_PIANO,
    PATCH_ELECTRIC_PIANO,
    PATCH_HONKY,
    PATCH_HARPSICHORD,
    PATCH_BELL,
    PATCH_MALLET,
    PATCH_ORGAN,
    PATCH_CHURCH_ORGAN,
    PATCH_ACCORDION,
    PATCH_NYLON_GUITAR,
    PATCH_STEEL_GUITAR,
    PATCH_CLEAN_GUITAR,
    PATCH_DISTORTED_GUITAR,
    PATCH_BASS,
    PATCH_SYNTH_BASS,
    PATCH_STRINGS,
    PATCH_PIZZICATO,
    PATCH_HARP,
    PATCH_CHOIR,
    PATCH_BRASS,
    PATCH_HORN,
    PATCH_SAX,
    PATCH_REED,
    PATCH_FLUTE,
    PATCH_SQUARE_LEAD,
    PATCH_SAW_LEAD,
    PATCH_PAD,
    PATCH_METALLIC,
    PATCH_ETHNIC,
    PATCH_DRUM,
    PATCH_NOISE,
    PATCH_KICK,
    PATCH_SNARE,
    PATCH_HAT,
    PATCH_TOM,
    PATCH_CYMBAL,
    PATCH_COUNT
};

/*
 * Original two-operator patches designed for this wrapper. They are grouped by
 * GM timbre family; they do not come from, and are not claimed to reproduce, a
 * sample-based General MIDI ROM.
 */
static const opl_patch patch_templates[PATCH_COUNT] = {
    PATCH(OP(1, 25, 1, 15, 6, 4, 4, 0, 0, 0, 0, 0),
          OP(1,  2, 1, 15, 5, 3, 5, 0, 0, 0, 0, 0), 4, 0, 0, 34),
    PATCH(OP(2, 20, 1, 15, 7, 3, 4, 0, 0, 0, 0, 0),
          OP(1,  1, 1, 15, 6, 3, 5, 0, 0, 0, 0, 0), 5, 0, 0, 38),
    PATCH(OP(1, 30, 0, 15, 5, 6, 4, 3, 1, 0, 1, 0),
          OP(1,  3, 0, 14, 4, 5, 5, 0, 1, 0, 1, 0), 3, 0, 0, 28),
    PATCH(OP(3, 18, 1, 15, 9, 5, 7, 2, 0, 0, 0, 1),
          OP(1,  4, 1, 15, 8, 4, 6, 0, 0, 0, 0, 1), 6, 0, 0, 36),
    PATCH(OP(2, 18, 0, 15, 8, 3, 5, 1, 0, 0, 0, 1),
          OP(1,  2, 0, 15, 7, 4, 5, 0, 0, 0, 0, 1), 2, 0, 0, 30),
    PATCH(OP(5, 16, 0, 15, 5, 4, 8, 0, 1, 0, 0, 1),
          OP(1,  4, 0, 15, 4, 5, 8, 0, 1, 0, 0, 1), 4, 0, 12, 34),
    PATCH(OP(4, 20, 1, 15, 7, 5, 7, 0, 0, 0, 0, 1),
          OP(1,  3, 1, 15, 6, 5, 7, 0, 0, 0, 0, 1), 4, 0, 0, 32),
    PATCH(OP(2, 22, 0, 15, 3, 2, 4, 0, 1, 1, 1, 0),
          OP(1,  4, 0, 15, 2, 2, 4, 0, 1, 1, 1, 0), 5, 1, 0, 22),
    PATCH(OP(1, 24, 0, 11, 3, 1, 5, 0, 1, 0, 1, 0),
          OP(2,  5, 0, 12, 3, 2, 6, 0, 1, 0, 1, 0), 2, 1, 0, 20),
    PATCH(OP(2, 26, 0, 13, 4, 3, 5, 0, 1, 1, 1, 0),
          OP(1,  4, 0, 14, 4, 3, 5, 0, 1, 1, 1, 0), 4, 1, 0, 24),
    PATCH(OP(1, 28, 1, 15, 6, 5, 6, 0, 0, 0, 0, 0),
          OP(1,  3, 1, 15, 5, 5, 6, 0, 0, 0, 0, 0), 3, 0, 0, 32),
    PATCH(OP(2, 25, 1, 15, 6, 4, 5, 0, 0, 0, 0, 0),
          OP(1,  2, 1, 15, 5, 4, 6, 0, 0, 0, 0, 0), 4, 0, 0, 34),
    PATCH(OP(2, 30, 0, 15, 5, 4, 5, 1, 0, 1, 1, 0),
          OP(1,  3, 0, 15, 4, 4, 5, 0, 0, 1, 1, 0), 3, 0, 0, 30),
    PATCH(OP(1, 10, 0, 15, 4, 3, 4, 4, 0, 1, 1, 1),
          OP(1,  5, 0, 15, 3, 3, 5, 1, 0, 1, 1, 1), 7, 0, -12, 26),
    PATCH(OP(1, 24, 2, 15, 5, 5, 5, 0, 0, 0, 1, 1),
          OP(1,  2, 2, 15, 4, 4, 5, 0, 0, 0, 1, 1), 4, 0, -12, 34),
    PATCH(OP(2, 17, 1, 15, 4, 4, 4, 3, 0, 1, 1, 1),
          OP(1,  3, 1, 15, 3, 4, 5, 2, 0, 1, 1, 1), 6, 0, -12, 28),
    PATCH(OP(1, 30, 0, 10, 4, 4, 5, 0, 1, 1, 1, 0),
          OP(1,  5, 0, 11, 3, 3, 6, 0, 1, 1, 1, 0), 2, 1, 0, 24),
    PATCH(OP(3, 26, 1, 15, 9, 7, 7, 0, 0, 0, 0, 1),
          OP(1,  4, 1, 15, 8, 7, 8, 0, 0, 0, 0, 1), 3, 0, 0, 28),
    PATCH(OP(2, 30, 0, 14, 5, 5, 7, 0, 0, 0, 0, 0),
          OP(1,  3, 0, 15, 4, 5, 7, 0, 0, 0, 0, 0), 2, 0, 0, 30),
    PATCH(OP(2, 29, 0, 10, 3, 3, 6, 0, 1, 1, 1, 0),
          OP(1,  6, 0, 11, 3, 3, 7, 0, 1, 1, 1, 0), 2, 1, 0, 20),
    PATCH(OP(1, 22, 0, 14, 4, 4, 5, 0, 1, 1, 1, 1),
          OP(1,  3, 0, 15, 3, 4, 5, 0, 1, 1, 1, 1), 5, 0, 0, 30),
    PATCH(OP(1, 28, 0, 12, 3, 3, 5, 0, 1, 1, 1, 0),
          OP(2,  4, 0, 13, 3, 3, 6, 0, 1, 1, 1, 0), 3, 1, -12, 24),
    PATCH(OP(2, 23, 0, 14, 4, 4, 5, 2, 1, 1, 1, 1),
          OP(1,  4, 0, 14, 3, 4, 6, 0, 1, 1, 1, 1), 5, 0, 0, 28),
    PATCH(OP(2, 28, 0, 13, 4, 4, 5, 0, 1, 1, 1, 1),
          OP(1,  4, 0, 14, 3, 4, 6, 0, 1, 1, 1, 1), 3, 0, 0, 26),
    PATCH(OP(1, 34, 0, 11, 3, 2, 6, 0, 1, 1, 1, 0),
          OP(1,  4, 0, 13, 3, 2, 7, 0, 1, 1, 1, 0), 1, 1, 12, 24),
    PATCH(OP(1, 18, 0, 15, 3, 2, 4, 2, 0, 1, 1, 1),
          OP(1,  4, 0, 15, 2, 2, 4, 2, 0, 1, 1, 1), 6, 0, 0, 28),
    PATCH(OP(1, 14, 0, 15, 4, 3, 4, 3, 0, 1, 1, 1),
          OP(1,  4, 0, 15, 3, 3, 5, 1, 0, 1, 1, 1), 7, 0, 0, 30),
    PATCH(OP(2, 32, 0, 8, 2, 2, 6, 0, 1, 1, 1, 0),
          OP(1,  7, 0, 9, 2, 2, 7, 0, 1, 1, 1, 0), 2, 1, 0, 18),
    PATCH(OP(7, 18, 0, 13, 5, 4, 7, 5, 1, 1, 1, 1),
          OP(1,  5, 0, 14, 4, 4, 7, 3, 1, 1, 1, 1), 6, 0, 0, 26),
    PATCH(OP(3, 24, 0, 15, 6, 5, 6, 0, 0, 1, 0, 1),
          OP(1,  4, 0, 15, 5, 5, 7, 0, 0, 1, 0, 1), 4, 0, 0, 30),
    PATCH(OP(3, 20, 0, 15, 10, 8, 8, 0, 0, 0, 0, 1),
          OP(1,  5, 0, 15, 9, 8, 8, 0, 0, 0, 0, 1), 6, 0, -12, 36),
    PATCH(OP(15, 9, 0, 15, 10, 10, 10, 5, 0, 0, 0, 1),
          OP(2,  8, 0, 15, 9, 10, 10, 4, 0, 0, 0, 1), 7, 0, 12, 24),
    PATCH(OP(1, 18, 0, 15, 12, 10, 9, 0, 0, 0, 0, 1),
          OP(1,  2, 0, 15, 11, 10, 9, 0, 0, 0, 0, 1), 6, 0, -24, 40),
    PATCH(OP(12, 8, 0, 15, 13, 12, 12, 5, 0, 0, 0, 1),
          OP(1,  4, 0, 15, 12, 12, 12, 4, 0, 0, 0, 1), 7, 0, 0, 38),
    PATCH(OP(15, 5, 0, 15, 15, 15, 12, 5, 0, 0, 0, 1),
          OP(2,  5, 0, 15, 14, 15, 12, 4, 0, 0, 0, 1), 7, 0, 24, 26),
    PATCH(OP(1, 18, 1, 15, 9, 8, 8, 0, 0, 0, 0, 1),
          OP(1,  3, 1, 15, 8, 8, 8, 0, 0, 0, 0, 1), 5, 0, -12, 34),
    PATCH(OP(15, 6, 0, 15, 8, 10, 10, 5, 0, 0, 0, 1),
          OP(2,  5, 0, 15, 7, 10, 10, 4, 0, 0, 0, 1), 7, 0, 24, 28)
};

/*
 * Complete GM program LUT. Every entry is an independently voiced, literal
 * two-operator OPL3 patch; no melodic program is derived from a family index.
 */
static const opl_patch gm_program_patch_lut[] = {
    /* 0..7: pianos */
    PATCH(OP(1,25,1,15,6,4,4,0,0,0,0,0),
          OP(1, 2,1,15,5,3,5,0,0,0,0,0),4,0,  0,34), /* 0 */
    PATCH(OP(2,18,1,15,7,3,4,0,0,0,0,0),
          OP(1, 1,1,15,6,3,5,0,0,0,0,0),5,0,  0,38), /* 1 */
    PATCH(OP(1,29,0,14,5,5,5,3,1,0,1,0),
          OP(1, 4,0,14,4,5,6,0,1,0,1,0),3,0,  0,30), /* 2 */
    PATCH(OP(3,17,1,15,9,5,7,2,0,0,0,1),
          OP(1, 4,1,15,8,4,6,0,0,0,0,1),6,0,  0,36), /* 3 */
    PATCH(OP(1,31,0,15,5,6,5,3,1,0,1,0),
          OP(1, 5,0,14,4,5,6,0,1,0,1,0),2,1,  0,27), /* 4 */
    PATCH(OP(5,18,0,15,5,5,8,0,1,1,0,1),
          OP(1, 5,0,15,4,5,8,0,1,1,1,1),4,0,  0,32), /* 5 */
    PATCH(OP(2,16,1,15,8,4,5,1,0,0,0,1),
          OP(1, 3,1,15,7,4,5,0,0,0,0,1),2,0,  0,31), /* 6 */
    PATCH(OP(3,14,1,15,9,5,5,2,0,0,0,1),
          OP(1, 4,1,15,8,5,6,1,0,0,0,1),5,0,  0,35), /* 7 */

    /* 8..15: chromatic percussion */
    PATCH(OP(4,20,0,15,6,6,8,0,1,0,0,1),
          OP(1, 4,0,15,5,6,8,0,1,0,0,1),3,0, 12,32), /* 8 */
    PATCH(OP(6,13,0,15,7,7,9,0,1,0,0,1),
          OP(1, 5,0,15,5,7,9,0,1,0,0,1),5,0, 12,30), /* 9 */
    PATCH(OP(5,18,0,15,5,6,8,3,1,0,0,1),
          OP(1, 6,0,15,4,6,9,0,1,0,0,1),2,0, 12,28), /* 10 */
    PATCH(OP(3,25,0,13,3,3,6,0,1,1,1,0),
          OP(1, 5,0,14,3,3,7,0,1,1,1,0),4,1,  0,22), /* 11 */
    PATCH(OP(3,22,1,15,8,7,7,0,0,0,0,1),
          OP(1, 3,1,15,7,7,8,0,0,0,0,1),3,0,  0,34), /* 12 */
    PATCH(OP(4,16,1,15,10,9,8,1,0,0,0,1),
          OP(1, 4,1,15, 9,9,9,0,0,0,0,1),5,0, 12,36), /* 13 */
    PATCH(OP(7,14,0,15,5,5,9,0,1,0,0,1),
          OP(1, 5,0,15,4,5,9,0,1,0,0,1),6,0,-12,31), /* 14 */
    PATCH(OP(2,19,1,15,7,6,6,3,0,0,0,1),
          OP(1, 3,1,15,6,6,7,0,0,0,0,1),4,0,  0,33), /* 15 */

    /* 16..23: organs */
    PATCH(OP(1,18,0,15,2,1,4,0,1,1,1,0),
          OP(1, 4,0,15,2,1,4,0,1,1,1,0),2,1,  0,20), /* 16 */
    PATCH(OP(2,22,0,15,3,2,4,0,1,1,1,0),
          OP(1, 5,0,15,2,2,4,0,1,1,1,0),4,1,  0,22), /* 17 */
    PATCH(OP(3,17,0,15,2,1,3,1,1,1,1,1),
          OP(1, 4,0,15,2,1,4,0,1,1,1,1),6,1,  0,24), /* 18 */
    PATCH(OP(1,27,0,10,3,1,6,0,1,0,1,0),
          OP(2, 6,0,11,3,2,7,0,1,0,1,0),1,1,-12,18), /* 19 */
    PATCH(OP(2,24,0,14,3,2,5,2,1,1,1,0),
          OP(1, 5,0,15,3,2,5,0,1,1,1,0),3,1,  0,23), /* 20 */
    PATCH(OP(1,25,0,13,4,3,5,0,1,1,1,0),
          OP(2, 4,0,14,3,3,5,0,1,1,1,0),3,1,  0,25), /* 21 */
    PATCH(OP(2,21,0,14,4,3,5,2,1,1,1,1),
          OP(1, 5,0,15,3,3,6,0,1,1,1,1),5,0, 12,27), /* 22 */
    PATCH(OP(2,26,0,13,4,3,5,0,1,1,1,0),
          OP(1, 4,0,14,3,3,6,1,1,1,1,0),4,1,  0,26), /* 23 */

    /* 24..31: guitars */
    PATCH(OP(1,29,1,15,6,5,6,0,0,0,0,0),
          OP(1, 3,1,15,5,5,7,0,0,0,0,0),3,0,  0,32), /* 24 */
    PATCH(OP(2,24,1,15,7,4,6,0,0,0,0,0),
          OP(1, 2,1,15,6,4,7,0,0,0,0,0),4,0,  0,35), /* 25 */
    PATCH(OP(2,31,0,15,5,4,5,1,0,1,1,0),
          OP(1, 4,0,15,4,4,6,0,0,1,1,0),2,0,  0,29), /* 26 */
    PATCH(OP(1,27,0,15,5,4,5,0,0,1,1,0),
          OP(1, 3,0,15,4,4,6,0,0,1,1,0),3,0,  0,30), /* 27 */
    PATCH(OP(3,18,1,15,11,9,8,2,0,0,0,1),
          OP(1, 5,1,15,10,9,8,1,0,0,0,1),5,0,-12,38), /* 28 */
    PATCH(OP(1,11,0,15,4,3,4,4,0,1,1,1),
          OP(1, 6,0,15,3,3,5,1,0,1,1,1),7,0,-12,26), /* 29 */
    PATCH(OP(2, 8,0,15,5,4,5,5,0,1,1,1),
          OP(1, 7,0,15,4,4,6,2,0,1,1,1),7,0,-12,28), /* 30 */
    PATCH(OP(4,16,0,15,7,7,8,3,0,1,0,1),
          OP(1, 6,0,15,6,7,8,1,0,1,0,1),6,0, 12,24), /* 31 */

    /* 32..39: basses */
    PATCH(OP(1,23,2,15,5,5,5,0,0,0,1,1),
          OP(1, 2,2,15,4,4,5,0,0,0,1,1),4,0,-12,34), /* 32 */
    PATCH(OP(1,20,2,15,4,4,5,0,0,1,1,1),
          OP(1, 3,2,15,3,4,5,0,0,1,1,1),5,0,-12,32), /* 33 */
    PATCH(OP(2,19,2,15,6,5,5,1,0,0,1,1),
          OP(1, 3,2,15,5,5,6,0,0,0,1,1),4,0,-12,36), /* 34 */
    PATCH(OP(1,28,1,14,4,3,6,3,0,1,1,0),
          OP(1, 4,1,15,3,3,7,0,0,1,1,0),2,0,-12,27), /* 35 */
    PATCH(OP(3,15,2,15,9,8,6,2,0,0,0,1),
          OP(1, 4,2,15,8,8,7,0,0,0,0,1),6,0,-12,40), /* 36 */
    PATCH(OP(4,17,2,15,8,7,6,1,0,0,0,1),
          OP(1, 5,2,15,7,7,7,0,0,0,0,1),5,0,-12,38), /* 37 */
    PATCH(OP(2,16,1,15,4,4,4,3,0,1,1,1),
          OP(1, 3,1,15,3,4,5,2,0,1,1,1),6,0,-12,28), /* 38 */
    PATCH(OP(1,13,1,15,3,3,4,4,1,1,1,1),
          OP(1, 4,1,15,3,3,5,3,0,1,1,1),7,0,-24,30), /* 39 */

    /* 40..47: strings */
    PATCH(OP(1,30,0,10,4,4,5,0,1,1,1,0),
          OP(1, 5,0,11,3,3,6,0,1,1,1,0),2,1,  0,24), /* 40 */
    PATCH(OP(2,31,0,10,4,4,5,0,1,1,1,0),
          OP(1, 6,0,11,3,3,6,0,1,1,1,0),3,1,-12,25), /* 41 */
    PATCH(OP(1,28,0, 9,4,3,6,0,1,1,1,0),
          OP(1, 5,0,10,3,3,7,0,1,1,1,0),2,1,-12,26), /* 42 */
    PATCH(OP(1,26,1, 9,4,3,6,0,1,1,1,1),
          OP(1, 4,1,10,3,3,7,0,1,1,1,1),3,1,-24,28), /* 43 */
    PATCH(OP(3,25,0,12,5,4,6,2,1,1,1,0),
          OP(1, 5,0,12,4,4,7,0,1,1,1,0),4,1,  0,25), /* 44 */
    PATCH(OP(3,24,1,15,10,9,8,0,0,0,0,1),
          OP(1, 4,1,15, 9,9,9,0,0,0,0,1),3,0,  0,32), /* 45 */
    PATCH(OP(2,29,0,14,5,5,7,0,0,0,0,0),
          OP(1, 3,0,15,4,5,8,0,0,0,0,0),2,0,  0,30), /* 46 */
    PATCH(OP(1,17,0,15,9,8,8,0,0,0,0,1),
          OP(1, 3,0,15,8,8,8,0,0,0,0,1),5,0,-12,36), /* 47 */

    /* 48..55: ensembles and voices */
    PATCH(OP(1,31,0, 9,3,3,6,0,1,1,1,0),
          OP(1, 6,0,10,3,3,7,0,1,1,1,0),2,1,  0,20), /* 48 */
    PATCH(OP(2,33,0, 8,3,2,7,0,1,1,1,0),
          OP(1, 7,0, 9,2,2,8,0,1,1,1,0),1,1,  0,18), /* 49 */
    PATCH(OP(1,26,0,10,3,2,6,3,1,1,1,0),
          OP(1, 6,0,10,2,2,7,0,1,1,1,0),4,1,  0,22), /* 50 */
    PATCH(OP(2,28,0, 9,3,2,7,2,1,1,1,0),
          OP(1, 6,0,10,2,2,8,0,1,1,1,0),3,1,-12,21), /* 51 */
    PATCH(OP(2,28,0,11,3,3,6,0,1,1,1,0),
          OP(1, 7,0,12,3,3,7,0,1,1,1,0),2,1,  0,19), /* 52 */
    PATCH(OP(1,31,0,10,2,2,6,2,1,1,1,0),
          OP(2, 7,0,11,2,2,7,0,1,1,1,0),3,1,  0,18), /* 53 */
    PATCH(OP(3,27,0,12,4,3,6,3,1,1,1,1),
          OP(1, 6,0,13,3,3,7,0,1,1,1,1),4,1, 12,20), /* 54 */
    PATCH(OP(5,15,0,15,9,8,8,5,0,0,0,1),
          OP(1, 4,0,15,8,8,9,1,0,0,0,1),7,0,  0,38), /* 55 */

    /* 56..63: brass */
    PATCH(OP(1,21,0,14,4,4,5,0,1,1,1,1),
          OP(1, 3,0,15,3,4,5,0,1,1,1,1),5,0,  0,30), /* 56 */
    PATCH(OP(2,24,0,13,4,4,5,0,1,1,1,1),
          OP(1, 4,0,14,3,4,6,0,1,1,1,1),4,0,-12,28), /* 57 */
    PATCH(OP(1,27,1,12,4,3,6,0,1,1,1,1),
          OP(1, 4,1,13,3,3,7,0,1,1,1,1),3,0,-24,27), /* 58 */
    PATCH(OP(3,17,0,15,6,5,5,2,0,1,1,1),
          OP(1, 5,0,15,5,5,6,0,0,1,1,1),6,0,  0,33), /* 59 */
    PATCH(OP(1,25,0,11,3,3,6,0,1,1,1,0),
          OP(2, 5,0,12,3,3,7,0,1,1,1,0),3,1,-12,24), /* 60 */
    PATCH(OP(2,19,0,13,4,4,5,1,1,1,1,1),
          OP(1, 4,0,14,3,4,6,0,1,1,1,1),6,0,  0,32), /* 61 */
    PATCH(OP(2,15,0,15,4,3,4,3,0,1,1,1),
          OP(1, 4,0,15,3,3,5,2,0,1,1,1),7,0,-12,29), /* 62 */
    PATCH(OP(3,18,0,14,5,4,5,4,1,1,1,1),
          OP(1, 5,0,15,4,4,6,1,0,1,1,1),6,0,-12,31), /* 63 */

    /* 64..71: reeds */
    PATCH(OP(2,22,0,13,4,4,5,2,1,1,1,1),
          OP(1, 4,0,14,3,4,6,0,1,1,1,1),5,0, 12,27), /* 64 */
    PATCH(OP(2,24,0,13,4,4,5,1,1,1,1,1),
          OP(1, 4,0,14,3,4,6,0,1,1,1,1),4,0,  0,28), /* 65 */
    PATCH(OP(2,26,0,12,4,4,6,0,1,1,1,1),
          OP(1, 4,0,13,3,4,7,0,1,1,1,1),3,0,-12,26), /* 66 */
    PATCH(OP(1,25,1,11,4,3,6,0,1,1,1,1),
          OP(1, 5,1,12,3,3,7,0,1,1,1,1),4,0,-24,25), /* 67 */
    PATCH(OP(2,27,0,12,4,3,6,3,1,1,1,0),
          OP(1, 4,0,13,3,3,7,0,1,1,1,0),3,0,  0,26), /* 68 */
    PATCH(OP(1,29,0,11,3,3,7,2,1,1,1,0),
          OP(1, 5,0,12,3,3,8,0,1,1,1,0),2,0,-12,24), /* 69 */
    PATCH(OP(1,24,1,12,4,3,6,1,1,1,1,1),
          OP(1, 4,1,13,3,3,7,0,1,1,1,1),4,0,-12,27), /* 70 */
    PATCH(OP(3,23,0,13,4,4,5,2,1,1,1,1),
          OP(1, 4,0,14,3,4,6,0,1,1,1,1),5,0,-12,29), /* 71 */

    /* 72..79: pipes */
    PATCH(OP(1,32,0,11,3,2,6,0,1,1,1,0),
          OP(1, 4,0,13,3,2,7,0,1,1,1,0),1,1, 12,24), /* 72 */
    PATCH(OP(2,34,0,10,3,2,7,0,1,1,1,0),
          OP(1, 5,0,12,3,2,8,0,1,1,1,0),1,1,  0,22), /* 73 */
    PATCH(OP(1,30,0,12,3,2,6,2,1,1,1,0),
          OP(1, 4,0,13,3,2,7,0,1,1,1,0),2,1,  0,23), /* 74 */
    PATCH(OP(1,35,0,10,2,2,7,3,1,1,1,0),
          OP(2, 5,0,12,2,2,8,0,1,1,1,0),1,1,  0,20), /* 75 */
    PATCH(OP(2,31,0,11,3,2,7,2,1,1,1,1),
          OP(1, 5,0,13,3,2,8,0,1,1,1,1),2,1, 12,22), /* 76 */
    PATCH(OP(3,33,0, 9,3,2,7,3,1,1,1,1),
          OP(1, 5,0,12,3,2,8,0,1,1,1,1),2,1,-12,21), /* 77 */
    PATCH(OP(1,28,0,14,3,2,5,1,1,1,1,0),
          OP(1, 3,0,15,2,2,6,0,1,1,1,0),3,1, 12,26), /* 78 */
    PATCH(OP(2,30,0,12,3,2,6,3,1,1,1,0),
          OP(1, 4,0,14,3,2,7,0,1,1,1,0),2,1, 12,25), /* 79 */

    /* 80..87: synth leads */
    PATCH(OP(1,17,0,15,3,2,4,2,0,1,1,1),
          OP(1, 4,0,15,2,2,4,2,0,1,1,1),6,0,  0,28), /* 80 */
    PATCH(OP(1,13,0,15,4,3,4,3,0,1,1,1),
          OP(1, 4,0,15,3,3,5,1,0,1,1,1),7,0,  0,30), /* 81 */
    PATCH(OP(2,20,0,15,3,2,4,1,1,1,1,1),
          OP(1, 4,0,15,2,2,5,0,1,1,1,1),5,0, 12,27), /* 82 */
    PATCH(OP(3,16,0,15,7,6,5,2,0,1,0,1),
          OP(1, 5,0,15,6,6,6,1,0,1,0,1),6,0,  0,34), /* 83 */
    PATCH(OP(2,10,0,15,4,3,4,4,0,1,1,1),
          OP(1, 6,0,15,3,3,5,2,0,1,1,1),7,0,-12,32), /* 84 */
    PATCH(OP(3,25,0,13,3,2,5,3,1,1,1,0),
          OP(1, 5,0,14,3,2,6,0,1,1,1,0),4,1,  0,23), /* 85 */
    PATCH(OP(3,18,0,15,3,2,4,0,0,1,1,1),
          OP(2, 6,0,15,2,2,5,0,0,1,1,1),5,1,  0,26), /* 86 */
    PATCH(OP(1,14,1,15,4,3,4,3,0,1,1,1),
          OP(1, 4,1,15,3,3,5,1,0,1,1,1),7,0,-12,35), /* 87 */

    /* 88..95: synth pads */
    PATCH(OP(2,33,0, 8,2,2,7,0,1,1,1,0),
          OP(1, 7,0, 9,2,2,8,0,1,1,1,0),2,1,  0,18), /* 88 */
    PATCH(OP(1,35,0, 7,2,1,8,0,1,1,1,0),
          OP(1, 8,0, 8,2,1,9,0,1,1,1,0),1,1,-12,16), /* 89 */
    PATCH(OP(3,30,0, 9,3,2,7,3,1,1,1,0),
          OP(1, 7,0,10,2,2,8,0,1,1,1,0),3,1,  0,20), /* 90 */
    PATCH(OP(2,32,0, 8,2,2,8,2,1,1,1,0),
          OP(1, 8,0, 9,2,2,9,0,1,1,1,0),2,1,  0,17), /* 91 */
    PATCH(OP(1,29,0, 7,2,1,8,3,1,1,1,0),
          OP(1, 7,0, 8,2,1,9,0,1,1,1,0),3,1,-12,19), /* 92 */
    PATCH(OP(7,19,0,10,4,3,8,5,1,1,1,1),
          OP(1, 7,0,11,3,3,9,3,1,1,1,1),5,0,  0,22), /* 93 */
    PATCH(OP(2,31,0, 8,2,1,9,1,1,1,1,0),
          OP(1, 8,0, 9,2,1,9,0,1,1,1,0),2,1, 12,18), /* 94 */
    PATCH(OP(3,28,0, 6,2,1,9,4,1,1,1,0),
          OP(1, 8,0, 8,2,1,9,1,1,1,1,0),4,1,-12,17), /* 95 */

    /* 96..103: synth effects */
    PATCH(OP(7,18,0,13,5,4,7,5,1,1,1,1),
          OP(1, 5,0,14,4,4,7,3,1,1,1,1),6,0,  0,26), /* 96 */
    PATCH(OP(4,25,0, 8,2,2,8,3,1,1,1,0),
          OP(1, 7,0, 9,2,2,9,0,1,1,1,0),4,1,-12,18), /* 97 */
    PATCH(OP(8,14,0,15,6,6,9,5,1,1,0,1),
          OP(1, 6,0,15,5,6,9,2,1,1,0,1),7,0, 12,30), /* 98 */
    PATCH(OP(5,27,0, 7,2,2,8,4,1,1,1,0),
          OP(1, 8,0, 9,2,2,9,0,1,1,1,0),5,1,-12,17), /* 99 */
    PATCH(OP(6,16,0,12,4,3,7,3,1,1,1,1),
          OP(1, 5,0,13,3,3,8,1,1,1,1,1),6,0, 12,25), /* 100 */
    PATCH(OP(9,12,0,11,5,4,8,5,1,1,1,1),
          OP(2, 7,0,12,4,4,9,4,1,1,1,1),7,0,-12,24), /* 101 */
    PATCH(OP(3,22,0,10,3,2,7,2,1,1,1,0),
          OP(1, 6,0,12,3,2,8,0,1,1,1,0),5,1, 12,21), /* 102 */
    PATCH(OP(11,10,0,14,7,6,9,5,1,1,0,1),
          OP(1, 7,0,15,6,6,9,3,1,1,0,1),7,0,-24,28), /* 103 */

    /* 104..111: ethnic */
    PATCH(OP(3,22,1,15,6,5,6,0,0,1,0,1),
          OP(1, 4,1,15,5,5,7,0,0,1,0,1),4,0,  0,30), /* 104 */
    PATCH(OP(2,18,1,15,8,6,6,1,0,0,0,1),
          OP(1, 3,1,15,7,6,7,0,0,0,0,1),5,0,  0,34), /* 105 */
    PATCH(OP(4,19,1,15,7,6,7,2,0,0,0,1),
          OP(1, 4,1,15,6,6,8,0,0,0,0,1),4,0,  0,32), /* 106 */
    PATCH(OP(5,21,1,15,6,5,7,3,0,0,0,1),
          OP(1, 4,1,15,5,5,8,0,0,0,0,1),3,0,  0,31), /* 107 */
    PATCH(OP(4,24,1,15,10,9,8,0,0,0,0,1),
          OP(1, 4,1,15, 9,9,9,0,0,0,0,1),5,0, 12,36), /* 108 */
    PATCH(OP(2,24,0,12,3,3,6,2,1,1,1,0),
          OP(1, 5,0,13,3,3,7,0,1,1,1,0),4,1,-12,25), /* 109 */
    PATCH(OP(3,20,0,14,4,4,5,1,1,1,1,1),
          OP(1, 4,0,15,3,4,6,0,1,1,1,1),5,0,  0,29), /* 110 */
    PATCH(OP(4,18,0,15,5,4,6,3,1,1,1,1),
          OP(1, 5,0,15,4,4,7,0,1,1,1,1),6,0, 12,30), /* 111 */

    /* 112..119: percussive */
    PATCH(OP(6,16,0,15,7,7,9,0,1,0,0,1),
          OP(1, 5,0,15,6,7,9,0,1,0,0,1),5,0, 12,32), /* 112 */
    PATCH(OP(5,18,0,15,8,8,8,2,0,0,0,1),
          OP(1, 4,0,15,7,8,9,0,0,0,0,1),6,0,  0,34), /* 113 */
    PATCH(OP(4,20,0,15,7,7,8,3,0,0,0,1),
          OP(1, 5,0,15,6,7,9,1,0,0,0,1),5,0,  0,33), /* 114 */
    PATCH(OP(12,11,0,15,13,12,12,5,0,0,0,1),
          OP(1, 5,0,15,12,12,12,4,0,0,0,1),7,0,-12,38), /* 115 */
    PATCH(OP(1,17,1,15,9,8,8,0,0,0,0,1),
          OP(1, 3,1,15,8,8,8,0,0,0,0,1),5,0,-12,34), /* 116 */
    PATCH(OP(2,15,1,15,8,7,7,2,0,0,0,1),
          OP(1, 4,1,15,7,7,8,0,0,0,0,1),6,0,-12,35), /* 117 */
    PATCH(OP(8,10,0,15,11,10,10,5,0,0,0,1),
          OP(2, 7,0,15,10,10,11,4,0,0,0,1),7,0,  0,30), /* 118 */
    PATCH(OP(7,14,0,15,5,5,9,4,1,0,0,1),
          OP(1, 6,0,15,4,5,9,1,1,0,0,1),6,0,  0,28), /* 119 */

    /* 120..127: sound effects */
    PATCH(OP(4,15,0,15,9,8,8,5,0,0,0,1),
          OP(1, 6,0,15,8,8,9,3,0,0,0,1),7,0,-12,34), /* 120 */
    PATCH(OP(15, 7,0,15,10,10,10,5,0,0,0,1),
          OP(2, 8,0,15, 9,10,10,4,0,0,0,1),7,0, 12,24), /* 121 */
    PATCH(OP(9,11,0,14,7,6,9,5,1,1,0,1),
          OP(1, 7,0,15,6,6,9,2,1,1,0,1),6,0,-24,27), /* 122 */
    PATCH(OP(12, 9,0,15,12,11,11,5,1,1,0,1),
          OP(2, 7,0,15,11,11,12,4,1,1,0,1),7,0, 24,29), /* 123 */
    PATCH(OP(6,17,0,13,5,5,8,4,1,1,1,1),
          OP(1, 6,0,14,4,5,9,1,1,1,1,1),6,0,-12,26), /* 124 */
    PATCH(OP(11,12,0,15,8,7,9,5,1,1,0,1),
          OP(1, 7,0,15,7,7,10,3,1,1,0,1),7,0, 12,28), /* 125 */
    PATCH(OP(8,14,0,12,4,4,8,3,1,1,1,1),
          OP(2, 6,0,13,4,4,9,1,1,1,1,1),6,0,-24,25), /* 126 */
    PATCH(OP(15, 6,0,15,8,10,10,5,0,0,0,1),
          OP(2, 5,0,15,7,10,10,4,0,0,0,1),7,0, 24,28)  /* 127 */
};

_Static_assert(sizeof(gm_program_patch_lut) / sizeof(gm_program_patch_lut[0])
               == 128u, "GM program LUT must contain exactly 128 patches");

#undef PATCH
#undef OP

static opl_patch patch_for_program(uint8_t program,
                                   uint8_t bank_msb,
                                   uint8_t bank_lsb)
{
    opl_patch patch = gm_program_patch_lut[program];
    unsigned bank_variant = bank_lsb;
    /* GM2 commonly uses MSB 121 and an LSB variation. Keep the base identity
       and apply only a small FM brightness variation for nonzero LSB values;
       no exact variation bank is claimed. Unknown banks use the same fallback. */
    if (bank_variant != 0u) {
        unsigned amount = 1u + (bank_variant & 3u);
        patch.modulator.level = patch.modulator.level > amount
                              ? (uint8_t)(patch.modulator.level - amount) : 0u;
    }
    (void)bank_msb;
    return patch;
}

static opl_patch patch_for_drum(uint8_t note,
                                uint8_t kit,
                                uint8_t *pitch_note,
                                uint8_t *exclusive_group,
                                uint32_t *duration_ms)
{
    enum patch_id id = PATCH_NOISE;
    opl_patch patch;

    *pitch_note = note;
    *exclusive_group = 0u;
    *duration_ms = 700u;

    if (note == 35u || note == 36u) {
        id = PATCH_KICK;
        *pitch_note = note == 35u ? 58u : 62u;
        *duration_ms = 550u;
    } else if (note == 37u || note == 38u || note == 39u || note == 40u) {
        id = PATCH_SNARE;
        *pitch_note = (uint8_t)(62u + (note - 37u) * 2u);
        *duration_ms = note == 39u ? 400u : 650u;
    } else if (note == 41u || note == 43u || note == 45u
               || note == 47u || note == 48u || note == 50u) {
        id = PATCH_TOM;
        *pitch_note = (uint8_t)(note + 7u);
        *duration_ms = 800u;
    } else if (note == 42u || note == 44u || note == 46u) {
        id = PATCH_HAT;
        *pitch_note = note == 46u ? 72u : 76u;
        *exclusive_group = 1u;
        *duration_ms = note == 46u ? 950u : 180u;
    } else if (note == 49u || note == 51u || note == 52u || note == 53u
               || note == 55u || note == 57u || note == 59u) {
        id = PATCH_CYMBAL;
        *pitch_note = (uint8_t)(68u + (note & 7u));
        *duration_ms = 1800u;
    } else if (note == 54u || note == 56u || note == 58u
               || note == 67u || note == 68u || note == 69u
               || note == 70u || note == 75u || note == 76u
               || note == 77u) {
        id = PATCH_METALLIC;
        *pitch_note = (uint8_t)(72u + (note & 15u));
        *duration_ms = 650u;
    } else if (note == 60u || note == 61u) {
        id = PATCH_DRUM;
        *pitch_note = (uint8_t)(72u + (note - 60u) * 4u);
        *duration_ms = 450u;
    } else if (note == 80u || note == 81u) {
        id = PATCH_BELL;
        *pitch_note = note == 80u ? 91u : 96u;
        *exclusive_group = 2u;
        *duration_ms = note == 81u ? 1400u : 250u;
    } else if (note >= 78u && note <= 79u) {
        id = PATCH_FLUTE;
        *pitch_note = (uint8_t)(96u + (note - 78u) * 7u);
        *duration_ms = 500u;
    } else if (note >= 64u && note <= 73u) {
        id = PATCH_DRUM;
        *pitch_note = (uint8_t)(60u + (note & 7u));
        *duration_ms = 500u;
    }

    patch = patch_templates[id];
    if (kit == 16u || kit == 24u || kit == 25u) {
        patch.feedback = 7u;
        patch.modulator.waveform = (uint8_t)((patch.modulator.waveform + 2u) & 7u);
    } else if (kit == 40u) {
        if (patch.modulator.level < 58u) patch.modulator.level += 5u;
        if (patch.carrier.level < 58u) patch.carrier.level += 5u;
    } else if (kit == 48u && (note == 35u || note == 36u)) {
        patch = patch_templates[PATCH_DRUM];
        *pitch_note = 41u;
        *duration_ms = 1200u;
    } else if (kit == 56u) {
        patch = patch_templates[PATCH_NOISE];
    }
    return patch;
}

static int controller_delta(uint8_t value, int range)
{
    int centered = (int)value - 64;
    if (centered >= 0) return (centered * range + 31) / 63;
    return -((-centered * range + 32) / 64);
}

static uint8_t clamp_level_signed(int value)
{
    if (value < 0) return 0u;
    if (value > 63) return 63u;
    return (uint8_t)value;
}

static uint8_t controlled_rate(uint8_t base, uint8_t time_controller)
{
    int rate = (int)base - controller_delta(time_controller, 6);
    if (rate < 1) rate = 1;
    if (rate > 15) rate = 15;
    return (uint8_t)rate;
}

static unsigned controlled_feedback(const opl3_midi_player *player,
                                    const opl_voice *voice)
{
    const midi_channel_state *channel = &player->channels[voice->channel];
    int feedback = (int)voice->patch.feedback
                 + controller_delta(channel->harmonic_content, 2);
    if (feedback < 0) feedback = 0;
    if (feedback > 7) feedback = 7;
    return (unsigned)feedback;
}

static uint16_t channel_register(unsigned voice_index, uint16_t base)
{
    return (uint16_t)(base + (voice_index % 9u)
                      + (voice_index >= 9u ? 0x100u : 0u));
}

static uint16_t operator_register(unsigned voice_index,
                                  uint16_t base,
                                  int carrier)
{
    unsigned local_voice = voice_index % 9u;
    unsigned offset = opl_operator_offset[local_voice] + (carrier ? 3u : 0u);
    return (uint16_t)(base + offset + (voice_index >= 9u ? 0x100u : 0u));
}

static void write_voice_register(opl3_midi_player *player,
                                 unsigned voice_index,
                                 uint16_t reg,
                                 uint8_t value)
{
    const opl_voice *voice = &player->voices[voice_index];
    OPL3_WriteReg(&player->chip, reg, value);
    if (player->voice_capture_enabled) {
        OPL3_WriteReg(&player->voice_chips[voice_index], reg, value);
    }
    if (player->scope_capture_enabled && voice->active
        && voice->scope_slot < OPL3_MIDI_SCOPE_COUNT) {
        OPL3_WriteReg(&player->scope_chips[voice->scope_slot], reg, value);
    }
}

static void write_voice_pan_register(opl3_midi_player *player,
                                     unsigned voice_index,
                                     uint16_t reg,
                                     uint8_t value)
{
    const opl_voice *voice = &player->voices[voice_index];
    OPL3_WriteReg(&player->chip, reg, value);
    if (player->voice_capture_enabled) {
        /* Scope taps are mono, so keep both OPL3 output buses enabled. */
        OPL3_WriteReg(&player->voice_chips[voice_index], reg,
                      (uint8_t)((value & (uint8_t)~0x30u) | 0x30u));
    }
    if (player->scope_capture_enabled && voice->active
        && voice->scope_slot < OPL3_MIDI_SCOPE_COUNT) {
        OPL3_WriteReg(&player->scope_chips[voice->scope_slot], reg,
                      (uint8_t)((value & (uint8_t)~0x30u) | 0x30u));
    }
}

static uint8_t operator_characteristics(const opl_operator *op, int vibrato)
{
    return (uint8_t)((op->tremolo ? 0x80u : 0u)
                     | ((op->vibrato || vibrato) ? 0x40u : 0u)
                     | (op->sustained ? 0x20u : 0u)
                     | (op->key_scale_rate ? 0x10u : 0u)
                     | (op->multiplier & 0x0fu));
}

static uint8_t operator_attack_decay(const opl_operator *op)
{
    return (uint8_t)(((op->attack & 0x0fu) << 4) | (op->decay & 0x0fu));
}

static uint8_t operator_sustain_release(const opl_operator *op)
{
    return (uint8_t)(((op->sustain_level & 0x0fu) << 4)
                     | (op->release & 0x0fu));
}

static unsigned voice_attenuation(const opl3_midi_player *player,
                                  const opl_voice *voice)
{
    const midi_channel_state *channel = &player->channels[voice->channel];
    unsigned velocity_loss = (unsigned)(127u - voice->velocity)
                             * voice->patch.velocity_depth / 127u;
    unsigned volume_loss = (unsigned)(127u - channel->volume) * 28u / 127u;
    unsigned expression_loss = (unsigned)(127u - channel->expression) * 28u / 127u;
    return velocity_loss + volume_loss + expression_loss;
}

static void write_voice_levels(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    const midi_channel_state *channel = &player->channels[voice->channel];
    unsigned attenuation = voice_attenuation(player, voice);
    int brightness = controller_delta(channel->brightness, 8);
    int harmonic = controller_delta(channel->harmonic_content, 10);
    int carrier_level = (int)voice->patch.carrier.level + (int)attenuation;
    int modulator_level = (int)voice->patch.modulator.level
                        - brightness - harmonic;
    uint8_t modulator_value;
    uint8_t carrier_value;

    if (voice->patch.additive) {
        modulator_level += (int)attenuation;
        carrier_level -= controller_delta(channel->brightness, 2);
    } else {
        modulator_level += (int)((unsigned)(127u - voice->velocity) * 7u / 127u);
    }
    modulator_value = (uint8_t)((voice->patch.modulator.key_scale_level & 3u) << 6)
                      | clamp_level_signed(modulator_level);
    carrier_value = (uint8_t)((voice->patch.carrier.key_scale_level & 3u) << 6)
                    | clamp_level_signed(carrier_level);
    write_voice_register(player, voice_index,
                         operator_register(voice_index, 0x40u, 0),
                         modulator_value);
    write_voice_register(player, voice_index,
                         operator_register(voice_index, 0x40u, 1),
                         carrier_value);
}

static uint8_t voice_pan_bits(const opl3_midi_player *player,
                              const opl_voice *voice)
{
    uint8_t pan = player->channels[voice->channel].pan;
    if (pan < 48u) {
        return 0x10u;
    }
    if (pan > 79u) {
        return 0x20u;
    }
    return 0x30u;
}

static void write_voice_pan(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    uint8_t value = (uint8_t)((controlled_feedback(player, voice) << 1)
                              | (voice->patch.additive & 1u)
                              | voice_pan_bits(player, voice));
    write_voice_pan_register(player, voice_index,
                             channel_register(voice_index, 0xc0u), value);
}

static void write_voice_characteristics(opl3_midi_player *player,
                                        unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    int modulation = player->channels[voice->channel].modulation >= 32u;
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x20u, 0),
        operator_characteristics(&voice->patch.modulator, modulation));
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x20u, 1),
        operator_characteristics(&voice->patch.carrier, modulation));
}

static void write_voice_timbre(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    const midi_channel_state *channel = &player->channels[voice->channel];
    opl_operator modulator = voice->patch.modulator;
    opl_operator carrier = voice->patch.carrier;
    modulator.attack = controlled_rate(modulator.attack, channel->attack_time);
    carrier.attack = controlled_rate(carrier.attack, channel->attack_time);
    modulator.decay = controlled_rate(modulator.decay, channel->decay_time);
    carrier.decay = controlled_rate(carrier.decay, channel->decay_time);
    modulator.release = controlled_rate(modulator.release, channel->release_time);
    carrier.release = controlled_rate(carrier.release, channel->release_time);
    write_voice_characteristics(player, voice_index);
    write_voice_levels(player, voice_index);
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x60u, 0),
        operator_attack_decay(&modulator));
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x60u, 1),
        operator_attack_decay(&carrier));
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x80u, 0),
        operator_sustain_release(&modulator));
    write_voice_register(
        player, voice_index, operator_register(voice_index, 0x80u, 1),
        operator_sustain_release(&carrier));
    write_voice_register(player, voice_index,
                         operator_register(voice_index, 0xe0u, 0),
                         voice->patch.modulator.waveform & 7u);
    write_voice_register(player, voice_index,
                         operator_register(voice_index, 0xe0u, 1),
                         voice->patch.carrier.waveform & 7u);
    write_voice_pan(player, voice_index);
}

static double channel_bend_semitones(const midi_channel_state *channel)
{
    double range = (double)channel->bend_semitones
                   + (double)channel->bend_cents / 100.0;
    double normalized = channel->pitch_bend < 0
                      ? (double)channel->pitch_bend / 8192.0
                      : (double)channel->pitch_bend / 8191.0;
    return normalized * range;
}

static void write_voice_pitch(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    double bend = voice->percussion ? 0.0
                  : channel_bend_semitones(&player->channels[voice->channel]);
    double note = (double)voice->pitch_note + (double)voice->patch.transpose + bend;
    double frequency;
    double fnum_exact = 0.0;
    unsigned block;
    unsigned fnum;
    uint8_t high;

    if (note < -12.0) note = -12.0;
    if (note > 139.0) note = 139.0;
    frequency = 440.0 * pow(2.0, (note - 69.0) / 12.0);
    for (block = 0u; block < 8u; ++block) {
        fnum_exact = frequency * ldexp(1.0, (int)(20u - block))
                     / OPL3_NATIVE_RATE;
        if (fnum_exact <= 1023.0) {
            break;
        }
    }
    if (block > 7u) block = 7u;
    if (fnum_exact < 1.0) fnum_exact = 1.0;
    if (fnum_exact > 1023.0) fnum_exact = 1023.0;
    fnum = (unsigned)lround(fnum_exact);
    high = (uint8_t)(((fnum >> 8) & 3u) | ((block & 7u) << 2));
    voice->fnum_low = (uint8_t)(fnum & 0xffu);
    voice->block_fnum_high = high;
    write_voice_register(player, voice_index,
                         channel_register(voice_index, 0xa0u),
                         voice->fnum_low);
    write_voice_register(
        player, voice_index, channel_register(voice_index, 0xb0u),
        (uint8_t)(high | ((!voice->released && voice->active) ? 0x20u : 0u)));
}

static void silence_voice(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    if (voice->active) {
        write_voice_register(player, voice_index,
                             channel_register(voice_index, 0xb0u),
                             voice->block_fnum_high);
        write_voice_register(
            player, voice_index, operator_register(voice_index, 0x40u, 0),
            (uint8_t)(((voice->patch.modulator.key_scale_level & 3u) << 6)
                      | 63u));
        write_voice_register(
            player, voice_index, operator_register(voice_index, 0x40u, 1),
            (uint8_t)(((voice->patch.carrier.key_scale_level & 3u) << 6)
                      | 63u));
        write_voice_register(
            player, voice_index, channel_register(voice_index, 0xc0u),
            (uint8_t)(((voice->patch.feedback & 7u) << 1)
                      | (voice->patch.additive & 1u)));
    }
    memset(voice, 0, sizeof(*voice));
}

static void reset_channel(midi_channel_state *channel)
{
    memset(channel, 0, sizeof(*channel));
    channel->volume = 100u;
    channel->expression = 127u;
    channel->pan = 64u;
    channel->harmonic_content = 64u;
    channel->release_time = 64u;
    channel->attack_time = 64u;
    channel->brightness = 64u;
    channel->decay_time = 64u;
    channel->rpn_msb = 127u;
    channel->rpn_lsb = 127u;
    channel->bend_semitones = 2u;
}

static void reset_opl_chip(opl3_chip *chip, uint32_t sample_rate)
{
    OPL3_Reset(chip, sample_rate);
    OPL3_WriteReg(chip, 0x105u, 0x01u);
    OPL3_WriteReg(chip, 0x104u, 0x00u);
    OPL3_WriteReg(chip, 0x0bdu, 0x00u);
}

static void engine_reset(opl3_midi_player *player, opl3_midi_mode mode)
{
    unsigned i;
    reset_opl_chip(&player->chip, player->sample_rate);
    if (player->voice_capture_enabled) {
        for (i = 0u; i < OPL3_MIDI_VOICE_COUNT; ++i) {
            reset_opl_chip(&player->voice_chips[i], player->sample_rate);
        }
    }
    if (player->scope_capture_enabled) {
        for (i = 0u; i < OPL3_MIDI_SCOPE_COUNT; ++i) {
            reset_opl_chip(&player->scope_chips[i], player->sample_rate);
        }
    }
    memset(player->voices, 0, sizeof(player->voices));
    for (i = 0; i < OPL3_MIDI_CHANNEL_COUNT; ++i) {
        reset_channel(&player->channels[i]);
    }
    player->voice_serial = 0u;
    player->mode = mode;
}

static void all_sound_off(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel) {
            silence_voice(player, i);
        }
    }
}

static uint64_t saturating_frame_add(uint64_t frame, uint64_t increment)
{
    return UINT64_MAX - frame < increment ? UINT64_MAX : frame + increment;
}

static void release_voice(opl3_midi_player *player, unsigned voice_index)
{
    opl_voice *voice = &player->voices[voice_index];
    if (!voice->active || voice->released) {
        return;
    }
    write_voice_register(player, voice_index,
                         channel_register(voice_index, 0xb0u),
                         voice->block_fnum_high);
    voice->released = 1u;
    voice->sustained = 0u;
    voice->deadline = saturating_frame_add(
        player->current_frame,
        (uint64_t)player->sample_rate * MIDI_RELEASE_SECONDS);
}

static void release_channel_notes(opl3_midi_player *player,
                                  unsigned channel,
                                  int respect_sustain)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        opl_voice *voice = &player->voices[i];
        if (!voice->active || voice->channel != channel || voice->percussion) {
            continue;
        }
        voice->key_down = 0u;
        if (respect_sustain && player->channels[channel].sustain) {
            voice->sustained = 1u;
        } else {
            release_voice(player, i);
        }
    }
}

static void release_all_notes(opl3_midi_player *player)
{
    unsigned channel;
    for (channel = 0; channel < OPL3_MIDI_CHANNEL_COUNT; ++channel) {
        player->channels[channel].sustain = 0u;
        release_channel_notes(player, channel, 0);
    }
}

static unsigned allocate_voice(opl3_midi_player *player)
{
    unsigned i;
    unsigned candidate = 0u;
    uint64_t oldest = UINT64_MAX;

    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (!player->voices[i].active) {
            return i;
        }
    }
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].released && player->voices[i].serial < oldest) {
            oldest = player->voices[i].serial;
            candidate = i;
        }
    }
    if (oldest != UINT64_MAX) {
        silence_voice(player, candidate);
        return candidate;
    }
    oldest = UINT64_MAX;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].serial < oldest) {
            oldest = player->voices[i].serial;
            candidate = i;
        }
    }
    silence_voice(player, candidate);
    return candidate;
}

static uint8_t allocate_scope_lane(opl3_midi_player *player,
                                   uint8_t first_scope,
                                   uint8_t scope_count)
{
    uint8_t slot;
    unsigned i;
    unsigned candidate = OPL3_MIDI_VOICE_COUNT;
    uint64_t oldest = UINT64_MAX;

    if (first_scope >= OPL3_MIDI_SCOPE_COUNT || scope_count == 0u
        || (unsigned)first_scope + scope_count > OPL3_MIDI_SCOPE_COUNT) {
        return UINT8_MAX;
    }
    for (slot = first_scope; slot < (uint8_t)(first_scope + scope_count); ++slot) {
        int occupied = 0;
        for (i = 0u; i < OPL3_MIDI_VOICE_COUNT; ++i) {
            if (player->voices[i].active
                && player->voices[i].scope_slot == slot) {
                occupied = 1;
                break;
            }
        }
        if (!occupied) {
            if (player->scope_capture_enabled) {
                reset_opl_chip(&player->scope_chips[slot], player->sample_rate);
            }
            return slot;
        }
    }
    for (i = 0u; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        const opl_voice *voice = &player->voices[i];
        if (voice->active && voice->released
            && voice->scope_slot >= first_scope
            && voice->scope_slot < (unsigned)first_scope + scope_count
            && voice->serial < oldest) {
            candidate = i;
            oldest = voice->serial;
        }
    }
    if (candidate == OPL3_MIDI_VOICE_COUNT) {
        for (i = 0u; i < OPL3_MIDI_VOICE_COUNT; ++i) {
            const opl_voice *voice = &player->voices[i];
            if (voice->active && voice->scope_slot >= first_scope
                && voice->scope_slot < (unsigned)first_scope + scope_count
                && voice->serial < oldest) {
                candidate = i;
                oldest = voice->serial;
            }
        }
    }
    if (candidate == OPL3_MIDI_VOICE_COUNT) return first_scope;
    slot = player->voices[candidate].scope_slot;
    player->voices[candidate].scope_slot = UINT8_MAX;
    if (player->scope_capture_enabled) {
        reset_opl_chip(&player->scope_chips[slot], player->sample_rate);
    }
    return slot;
}

static void choke_exclusive_group(opl3_midi_player *player, uint8_t group)
{
    unsigned i;
    if (group == 0u) return;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active
            && player->voices[i].exclusive_group == group) {
            silence_voice(player, i);
        }
    }
}

static void note_on(opl3_midi_player *player, const midi_event *event)
{
    opl_voice new_voice;
    uint8_t channel = event->channel;
    uint8_t note = event->a;
    uint8_t velocity = event->b;
    midi_channel_state *state = &player->channels[channel];
    unsigned voice_index;

    memset(&new_voice, 0, sizeof(new_voice));
    new_voice.active = 1u;
    new_voice.key_down = 1u;
    new_voice.track = event->track;
    new_voice.channel = channel;
    new_voice.scope_slot = UINT8_MAX;
    new_voice.note = note;
    new_voice.pitch_note = note;
    new_voice.velocity = velocity;
    new_voice.serial = ++player->voice_serial;
    new_voice.deadline = UINT64_MAX;

    if (channel == 9u) {
        uint32_t duration_ms;
        new_voice.percussion = 1u;
        new_voice.patch = patch_for_drum(note, event->stream_program,
                                         &new_voice.pitch_note,
                                         &new_voice.exclusive_group,
                                         &duration_ms);
        new_voice.deadline = saturating_frame_add(
            player->current_frame,
            (uint64_t)player->sample_rate * duration_ms / 1000u);
        choke_exclusive_group(player, new_voice.exclusive_group);
    } else {
        new_voice.patch = patch_for_program(event->stream_program,
                                            state->bank_msb,
                                            state->bank_lsb);
    }

    voice_index = allocate_voice(player);
    new_voice.scope_slot = allocate_scope_lane(player, event->scope_first,
                                               event->scope_count);
    player->voices[voice_index] = new_voice;
    write_voice_register(player, voice_index,
                         channel_register(voice_index, 0xb0u), 0u);
    write_voice_timbre(player, voice_index);
    write_voice_pitch(player, voice_index);
}

static void note_off(opl3_midi_player *player,
                     uint16_t track,
                     uint8_t channel,
                     uint8_t note)
{
    unsigned i;
    unsigned candidate = OPL3_MIDI_VOICE_COUNT;
    uint64_t oldest = UINT64_MAX;

    if (channel == 9u) {
        return;
    }
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        opl_voice *voice = &player->voices[i];
        if (voice->active && voice->key_down && voice->track == track
            && voice->channel == channel && voice->note == note
            && voice->serial < oldest) {
            oldest = voice->serial;
            candidate = i;
        }
    }
    if (candidate == OPL3_MIDI_VOICE_COUNT) {
        return;
    }
    player->voices[candidate].key_down = 0u;
    if (player->channels[channel].sustain) {
        player->voices[candidate].sustained = 1u;
    } else {
        release_voice(player, candidate);
    }
}

static void update_channel_levels(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel) {
            write_voice_levels(player, i);
        }
    }
}

static void update_channel_pan(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel) {
            write_voice_pan(player, i);
        }
    }
}

static void update_channel_modulation(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel) {
            write_voice_characteristics(player, i);
        }
    }
}

static void update_channel_timbre(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0u; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel) {
            write_voice_timbre(player, i);
        }
    }
}

static void update_channel_pitch(opl3_midi_player *player, unsigned channel)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active && player->voices[i].channel == channel
            && !player->voices[i].percussion) {
            write_voice_pitch(player, i);
        }
    }
}

static void reset_channel_controllers(opl3_midi_player *player, unsigned channel)
{
    midi_channel_state *state = &player->channels[channel];
    int had_sustain = state->sustain;

    /* MIDI RP-015 leaves program, bank, volume, pan, effect controllers,
       sound controllers, and parameter values unchanged. */
    state->expression = 127u;
    state->modulation = 0u;
    state->sustain = 0u;
    state->rpn_msb = 127u;
    state->rpn_lsb = 127u;
    state->pitch_bend = 0;
    if (had_sustain) {
        unsigned i;
        for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
            if (player->voices[i].active && player->voices[i].channel == channel
                && player->voices[i].sustained) {
                release_voice(player, i);
            }
        }
    }
    update_channel_levels(player, channel);
    update_channel_pan(player, channel);
    update_channel_modulation(player, channel);
    update_channel_pitch(player, channel);
}

static void control_change(opl3_midi_player *player,
                           uint8_t channel,
                           uint8_t controller,
                           uint8_t value)
{
    midi_channel_state *state = &player->channels[channel];
    unsigned i;

    switch (controller) {
    case 0u:
        state->pending_bank_msb = value;
        break;
    case 1u:
        state->modulation = value;
        update_channel_modulation(player, channel);
        break;
    case 6u:
        if (state->rpn_msb == 0u && state->rpn_lsb == 0u) {
            state->bend_semitones = value > 24u ? 24u : value;
            update_channel_pitch(player, channel);
        }
        break;
    case 7u:
        state->volume = value;
        update_channel_levels(player, channel);
        break;
    case 10u:
        state->pan = value;
        update_channel_pan(player, channel);
        break;
    case 11u:
        state->expression = value;
        update_channel_levels(player, channel);
        break;
    case 32u:
        state->pending_bank_lsb = value;
        break;
    case 38u:
        if (state->rpn_msb == 0u && state->rpn_lsb == 0u) {
            state->bend_cents = value > 99u ? 99u : value;
            update_channel_pitch(player, channel);
        }
        break;
    case 64u:
        if (value >= 64u) {
            state->sustain = 1u;
        } else {
            state->sustain = 0u;
            for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
                if (player->voices[i].active
                    && player->voices[i].channel == channel
                    && player->voices[i].sustained) {
                    release_voice(player, i);
                }
            }
        }
        break;
    case 71u:
        state->harmonic_content = value;
        update_channel_timbre(player, channel);
        break;
    case 72u:
        state->release_time = value;
        update_channel_timbre(player, channel);
        break;
    case 73u:
        state->attack_time = value;
        update_channel_timbre(player, channel);
        break;
    case 74u:
        state->brightness = value;
        update_channel_timbre(player, channel);
        break;
    case 75u:
        state->decay_time = value;
        update_channel_timbre(player, channel);
        break;
    case 96u:
        if (state->rpn_msb == 0u && state->rpn_lsb == 0u) {
            if (state->bend_cents < 99u) {
                ++state->bend_cents;
            } else if (state->bend_semitones < 24u) {
                state->bend_cents = 0u;
                ++state->bend_semitones;
            }
            update_channel_pitch(player, channel);
        }
        break;
    case 97u:
        if (state->rpn_msb == 0u && state->rpn_lsb == 0u) {
            if (state->bend_cents > 0u) {
                --state->bend_cents;
            } else if (state->bend_semitones > 0u) {
                --state->bend_semitones;
                state->bend_cents = 99u;
            }
            update_channel_pitch(player, channel);
        }
        break;
    case 98u:
    case 99u:
        state->rpn_msb = 127u;
        state->rpn_lsb = 127u;
        break;
    case 100u:
        state->rpn_lsb = value;
        break;
    case 101u:
        state->rpn_msb = value;
        break;
    case 120u:
        all_sound_off(player, channel);
        break;
    case 121u:
        reset_channel_controllers(player, channel);
        break;
    case 123u:
        release_channel_notes(player, channel, 1);
        break;
    case 124u:
    case 125u:
    case 126u:
    case 127u:
        /* This file player keeps a fixed per-channel polyphonic model, but the
           MIDI channel-mode messages also carry All Notes Off semantics. */
        release_channel_notes(player, channel, 1);
        break;
    default:
        break;
    }
}

static void handle_event(opl3_midi_player *player, const midi_event *event)
{
    midi_channel_state *channel;
    switch ((midi_event_kind)event->kind) {
    case MIDI_EVENT_NOTE_OFF:
        note_off(player, event->track, event->channel, event->a);
        break;
    case MIDI_EVENT_NOTE_ON:
        note_on(player, event);
        break;
    case MIDI_EVENT_CONTROL:
        control_change(player, event->channel, event->a, event->b);
        break;
    case MIDI_EVENT_PROGRAM:
        channel = &player->channels[event->channel];
        channel->program = event->a;
        channel->bank_msb = channel->pending_bank_msb;
        channel->bank_lsb = channel->pending_bank_lsb;
        break;
    case MIDI_EVENT_PITCH_BEND:
        channel = &player->channels[event->channel];
        channel->pitch_bend = (int16_t)((int)event->value - 8192);
        update_channel_pitch(player, event->channel);
        break;
    case MIDI_EVENT_RESET:
        engine_reset(player, (opl3_midi_mode)event->value);
        break;
    case MIDI_EVENT_END:
        release_all_notes(player);
        break;
    case MIDI_EVENT_POLY_PRESSURE:
    case MIDI_EVENT_CHANNEL_PRESSURE:
    case MIDI_EVENT_TEMPO:
        break;
    }
}

static void cleanup_expired_voices(opl3_midi_player *player)
{
    unsigned i;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        opl_voice *voice = &player->voices[i];
        if (voice->active && voice->deadline <= player->current_frame) {
            silence_voice(player, i);
        }
    }
}

static uint64_t next_voice_deadline(const opl3_midi_player *player)
{
    unsigned i;
    uint64_t deadline = UINT64_MAX;
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        if (player->voices[i].active
            && player->voices[i].deadline < deadline) {
            deadline = player->voices[i].deadline;
        }
    }
    return deadline;
}

static void restart_playback(opl3_midi_player *player)
{
    engine_reset(player, OPL3_MIDI_MODE_GM1);
    player->next_event = 0u;
    player->current_frame = 0u;
    player->finished = 0;
}

opl3_midi_player *opl3_midi_create(uint32_t sample_rate)
{
    opl3_midi_player *player;
    if (sample_rate < 8000u || sample_rate > 192000u) {
        return NULL;
    }
    player = (opl3_midi_player *)calloc(1u, sizeof(*player));
    if (player == NULL) {
        return NULL;
    }
    player->sample_rate = sample_rate;
    player->gain = 1.0f;
    engine_reset(player, OPL3_MIDI_MODE_GM1);
    return player;
}

void opl3_midi_destroy(opl3_midi_player *player)
{
    if (player == NULL) {
        return;
    }
    song_free(&player->song);
    free(player);
}

int opl3_midi_load_memory(opl3_midi_player *player,
                          const void *data,
                          size_t size)
{
    midi_song parsed;
    char parse_error[MIDI_ERROR_LENGTH];

    if (player == NULL) {
        return 0;
    }
    parse_error[0] = '\0';
    if (!parse_midi(data, size, player->sample_rate, &parsed, parse_error)) {
        set_error_text(player->error, parse_error);
        return 0;
    }

    song_free(&player->song);
    player->song = parsed;
    player->loaded = 1;
    player->error[0] = '\0';
    restart_playback(player);
    return 1;
}

int opl3_midi_load_file(opl3_midi_player *player, const char *path)
{
    FILE *file;
    long file_length;
    uint8_t *data;
    size_t length;
    size_t read_length;
    int result;

    if (player == NULL) {
        return 0;
    }
    if (path == NULL || path[0] == '\0') {
        set_error_text(player->error, "No MIDI file path was provided");
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        set_error_format(player->error, "Cannot open MIDI file: ", strerror(errno));
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (file_length = ftell(file)) < 0
        || fseek(file, 0, SEEK_SET) != 0) {
        set_error_format(player->error, "Cannot determine MIDI file size: ",
                         strerror(errno));
        (void)fclose(file);
        return 0;
    }
    if ((unsigned long)file_length > (unsigned long)MIDI_MAX_FILE_SIZE) {
        set_error_text(player->error, "MIDI file exceeds the 256 MiB safety limit");
        (void)fclose(file);
        return 0;
    }
    length = (size_t)file_length;
    data = (uint8_t *)malloc(length == 0u ? 1u : length);
    if (data == NULL) {
        set_error_text(player->error, "Out of memory while loading MIDI file");
        (void)fclose(file);
        return 0;
    }
    read_length = fread(data, 1u, length, file);
    if (read_length != length) {
        set_error_format(player->error, "Cannot read MIDI file: ",
                         ferror(file) ? strerror(errno) : "unexpected end of file");
        free(data);
        (void)fclose(file);
        return 0;
    }
    if (fclose(file) != 0) {
        set_error_format(player->error, "Cannot close MIDI file: ", strerror(errno));
        free(data);
        return 0;
    }
    result = opl3_midi_load_memory(player, data, length);
    free(data);
    return result;
}

void opl3_midi_unload(opl3_midi_player *player)
{
    if (player == NULL) {
        return;
    }
    song_free(&player->song);
    player->loaded = 0;
    player->next_event = 0u;
    player->current_frame = 0u;
    player->finished = 0;
    player->error[0] = '\0';
    engine_reset(player, OPL3_MIDI_MODE_GM1);
}

static int16_t scale_sample(int16_t sample, float gain)
{
    long scaled = lroundf((float)sample * gain);
    if (scaled > INT16_MAX) return INT16_MAX;
    if (scaled < INT16_MIN) return INT16_MIN;
    return (int16_t)scaled;
}

static void clear_capture_output(int16_t *output,
                                 unsigned channel_count,
                                 size_t frame_stride,
                                 size_t first_frame,
                                 size_t frame_count)
{
    unsigned channel;
    if (output == NULL) return;
    for (channel = 0u; channel < channel_count; ++channel) {
        memset(output + (size_t)channel * frame_stride + first_frame, 0,
               frame_count * sizeof(*output));
    }
}

static void generate_capture_output(opl3_midi_player *player,
                                    opl3_chip *chips,
                                    unsigned channel_count,
                                    int16_t *output,
                                    size_t frame_stride,
                                    size_t first_frame,
                                    size_t frame_count)
{
    int16_t scratch[MIDI_CAPTURE_RENDER_CHUNK * 2u];
    unsigned channel;

    for (channel = 0u; channel < channel_count; ++channel) {
        size_t i;
        OPL3_GenerateStream(&chips[channel], scratch,
                            (uint32_t)frame_count);
        if (output == NULL) continue;
        for (i = 0u; i < frame_count; ++i) {
            int mono = ((int)scratch[i * 2u] + (int)scratch[i * 2u + 1u]) / 2;
            int16_t sample = (int16_t)mono;
            if (player->gain != 1.0f) sample = scale_sample(sample, player->gain);
            output[(size_t)channel * frame_stride + first_frame + i] = sample;
        }
    }
}

static void render_internal(opl3_midi_player *player,
                            int16_t *stereo_output,
                            int16_t *voice_output,
                            int16_t *scope_output,
                            size_t frame_count)
{
    size_t rendered = 0u;

    if (stereo_output == NULL || frame_count == 0u) {
        return;
    }
    if (player == NULL || !player->loaded) {
        memset(stereo_output, 0, frame_count * 2u * sizeof(*stereo_output));
        clear_capture_output(voice_output, OPL3_MIDI_VOICE_COUNT,
                             frame_count, 0u, frame_count);
        clear_capture_output(scope_output, OPL3_MIDI_SCOPE_COUNT,
                             frame_count, 0u, frame_count);
        return;
    }

    while (rendered < frame_count) {
        uint64_t tail = (uint64_t)player->sample_rate * MIDI_TAIL_SECONDS;
        uint64_t playback_end = player->song.duration_frames;
        uint64_t boundary;
        uint64_t deadline;
        uint64_t available;
        size_t chunk;
        size_t i;

        if (!player->loop_enabled) {
            playback_end = UINT64_MAX - playback_end < tail
                         ? UINT64_MAX : playback_end + tail;
        }

        while (player->next_event < player->song.count
               && player->song.events[player->next_event].frame
                  <= player->current_frame) {
            handle_event(player, &player->song.events[player->next_event]);
            ++player->next_event;
        }
        cleanup_expired_voices(player);

        if (player->current_frame >= playback_end) {
            if (player->loop_enabled && player->song.duration_frames > 0u) {
                restart_playback(player);
                continue;
            }
            player->finished = 1;
            memset(stereo_output + rendered * 2u, 0,
                   (frame_count - rendered) * 2u * sizeof(*stereo_output));
            clear_capture_output(voice_output, OPL3_MIDI_VOICE_COUNT,
                                 frame_count, rendered, frame_count - rendered);
            clear_capture_output(scope_output, OPL3_MIDI_SCOPE_COUNT,
                                 frame_count, rendered, frame_count - rendered);
            return;
        }

        boundary = playback_end;
        if (player->next_event < player->song.count
            && player->song.events[player->next_event].frame < boundary) {
            boundary = player->song.events[player->next_event].frame;
        }
        deadline = next_voice_deadline(player);
        if (deadline < boundary) {
            boundary = deadline;
        }
        if (boundary <= player->current_frame) {
            cleanup_expired_voices(player);
            continue;
        }
        available = boundary - player->current_frame;
        chunk = frame_count - rendered;
        if ((uint64_t)chunk > available) chunk = (size_t)available;
        if (chunk > UINT32_MAX) chunk = UINT32_MAX;
        if ((player->voice_capture_enabled || player->scope_capture_enabled)
            && chunk > MIDI_CAPTURE_RENDER_CHUNK) {
            chunk = MIDI_CAPTURE_RENDER_CHUNK;
        }

        OPL3_GenerateStream(&player->chip, stereo_output + rendered * 2u,
                            (uint32_t)chunk);
        if (player->voice_capture_enabled) {
            generate_capture_output(player, player->voice_chips,
                                    OPL3_MIDI_VOICE_COUNT, voice_output,
                                    frame_count, rendered, chunk);
        }
        if (player->scope_capture_enabled) {
            generate_capture_output(player, player->scope_chips,
                                    OPL3_MIDI_SCOPE_COUNT, scope_output,
                                    frame_count, rendered, chunk);
        }
        if (player->gain != 1.0f) {
            for (i = 0; i < chunk * 2u; ++i) {
                stereo_output[rendered * 2u + i]
                    = scale_sample(stereo_output[rendered * 2u + i], player->gain);
            }
        }
        player->current_frame += (uint64_t)chunk;
        rendered += chunk;
    }
    if (!player->loop_enabled) {
        uint64_t tail = (uint64_t)player->sample_rate * MIDI_TAIL_SECONDS;
        uint64_t playback_end = UINT64_MAX - player->song.duration_frames < tail
                              ? UINT64_MAX : player->song.duration_frames + tail;
        if (player->current_frame >= playback_end) {
            player->finished = 1;
        }
    }
}

void opl3_midi_render(opl3_midi_player *player,
                      int16_t *stereo_output,
                      size_t frame_count)
{
    render_internal(player, stereo_output, NULL, NULL, frame_count);
}

void opl3_midi_render_voices(opl3_midi_player *player,
                             int16_t *stereo_output,
                             int16_t *voice_output,
                             size_t frame_count)
{
    if (player != NULL && voice_output != NULL
        && !player->voice_capture_enabled) {
        opl3_midi_set_voice_capture(player, 1);
    }
    render_internal(player, stereo_output, voice_output, NULL, frame_count);
}

void opl3_midi_render_scopes(opl3_midi_player *player,
                             int16_t *stereo_output,
                             int16_t *scope_output,
                             size_t frame_count)
{
    if (player != NULL && scope_output != NULL
        && !player->scope_capture_enabled) {
        opl3_midi_set_scope_capture(player, 1);
    }
    render_internal(player, stereo_output, NULL, scope_output, frame_count);
}

int opl3_midi_seek_frames(opl3_midi_player *player, uint64_t target_frame)
{
    if (player == NULL || !player->loaded) {
        return 0;
    }
    if (target_frame > player->song.duration_frames) {
        target_frame = player->song.duration_frames;
    }
    restart_playback(player);
    while (player->next_event < player->song.count
           && player->song.events[player->next_event].frame <= target_frame) {
        player->current_frame = player->song.events[player->next_event].frame;
        cleanup_expired_voices(player);
        handle_event(player, &player->song.events[player->next_event]);
        ++player->next_event;
    }
    player->current_frame = target_frame;
    cleanup_expired_voices(player);
    player->finished = 0;
    return 1;
}

int opl3_midi_seek_seconds(opl3_midi_player *player, double seconds)
{
    long double frames;
    if (player == NULL || !player->loaded || !isfinite(seconds) || seconds < 0.0) {
        return 0;
    }
    frames = (long double)seconds * (long double)player->sample_rate;
    if (frames > (long double)UINT64_MAX) {
        return 0;
    }
    return opl3_midi_seek_frames(player, (uint64_t)floorl(frames + 0.5L));
}

void opl3_midi_rewind(opl3_midi_player *player)
{
    if (player != NULL && player->loaded) {
        restart_playback(player);
    }
}

void opl3_midi_set_voice_capture(opl3_midi_player *player, int enabled)
{
    int requested;
    uint64_t position;
    if (player == NULL) return;
    requested = enabled != 0;
    if (requested == player->voice_capture_enabled) return;
    player->voice_capture_enabled = requested;
    if (!requested) return;

    position = player->current_frame;
    if (player->loaded) {
        (void)opl3_midi_seek_frames(player, position);
    } else {
        engine_reset(player, player->mode);
    }
}

void opl3_midi_set_scope_capture(opl3_midi_player *player, int enabled)
{
    int requested;
    uint64_t position;
    if (player == NULL) return;
    requested = enabled != 0;
    if (requested == player->scope_capture_enabled) return;
    player->scope_capture_enabled = requested;
    if (!requested) return;

    position = player->current_frame;
    if (player->loaded) {
        (void)opl3_midi_seek_frames(player, position);
    } else {
        engine_reset(player, player->mode);
    }
}

void opl3_midi_set_loop(opl3_midi_player *player, int enabled)
{
    if (player != NULL) {
        player->loop_enabled = enabled != 0;
    }
}

void opl3_midi_set_gain(opl3_midi_player *player, float gain)
{
    if (player == NULL || !isfinite(gain)) {
        return;
    }
    if (gain < 0.0f) gain = 0.0f;
    if (gain > 4.0f) gain = 4.0f;
    player->gain = gain;
}

int opl3_midi_get_info(const opl3_midi_player *player, opl3_midi_info *info)
{
    if (player == NULL || info == NULL || !player->loaded) {
        return 0;
    }
    memset(info, 0, sizeof(*info));
    info->format = player->song.format;
    info->track_count = player->song.track_count;
    info->division = player->song.division;
    info->event_count = player->song.count > 0u ? player->song.count - 1u : 0u;
    info->duration_frames = player->song.duration_frames;
    info->duration_seconds = (double)player->song.duration_frames
                             / (double)player->sample_rate;
    info->has_mode_reset = player->song.has_mode_reset;
    info->declared_mode = player->song.declared_mode;
    info->used_channel_mask = player->song.used_channel_mask;
    info->instrument_stream_count = player->song.instrument_stream_count;
    info->scope_count = player->song.scope_count;
    memcpy(info->scope_programs, player->song.scope_programs,
           sizeof(info->scope_programs));
    memcpy(info->scope_tracks, player->song.scope_tracks,
           sizeof(info->scope_tracks));
    memcpy(info->scope_channels, player->song.scope_channels,
           sizeof(info->scope_channels));
    memcpy(info->scope_lanes, player->song.scope_lanes,
           sizeof(info->scope_lanes));
    (void)snprintf(info->title, sizeof(info->title), "%s", player->song.title);
    return 1;
}

void opl3_midi_get_snapshot(const opl3_midi_player *player,
                            opl3_midi_snapshot *snapshot)
{
    unsigned i;
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (player == NULL) {
        return;
    }
    snapshot->position_frames = player->current_frame;
    snapshot->duration_frames = player->song.duration_frames;
    snapshot->sample_rate = player->sample_rate;
    snapshot->loaded = player->loaded;
    snapshot->finished = player->finished;
    snapshot->loop_enabled = player->loop_enabled;
    snapshot->gain = player->gain;
    snapshot->mode = player->mode;
    for (i = 0; i < OPL3_MIDI_CHANNEL_COUNT; ++i) {
        snapshot->programs[i] = player->channels[i].program;
        snapshot->bank_msb[i] = player->channels[i].bank_msb;
        snapshot->bank_lsb[i] = player->channels[i].bank_lsb;
        snapshot->pending_bank_msb[i] = player->channels[i].pending_bank_msb;
        snapshot->pending_bank_lsb[i] = player->channels[i].pending_bank_lsb;
        snapshot->channel_volume[i] = player->channels[i].volume;
        snapshot->channel_expression[i] = player->channels[i].expression;
        snapshot->channel_pan[i] = player->channels[i].pan;
        snapshot->channel_modulation[i] = player->channels[i].modulation;
        snapshot->channel_harmonic_content[i]
            = player->channels[i].harmonic_content;
        snapshot->channel_release_time[i] = player->channels[i].release_time;
        snapshot->channel_attack_time[i] = player->channels[i].attack_time;
        snapshot->channel_brightness[i] = player->channels[i].brightness;
        snapshot->channel_decay_time[i] = player->channels[i].decay_time;
        snapshot->channel_sustain[i] = player->channels[i].sustain;
        snapshot->bend_semitones[i] = player->channels[i].bend_semitones;
        snapshot->bend_cents[i] = player->channels[i].bend_cents;
        snapshot->pitch_bend[i] = player->channels[i].pitch_bend;
    }
    for (i = 0; i < OPL3_MIDI_VOICE_COUNT; ++i) {
        const opl_voice *voice = &player->voices[i];
        snapshot->voice_active[i] = voice->active;
        snapshot->voice_note[i] = voice->note;
        snapshot->voice_channel[i] = voice->channel;
        snapshot->voice_velocity[i] = voice->velocity;
        snapshot->voice_released[i] = voice->released;
        snapshot->voice_key_down[i] = voice->key_down;
        snapshot->voice_sustained[i] = voice->sustained;
        snapshot->voice_percussion[i] = voice->percussion;
        if (voice->active) {
            if (voice->scope_slot < OPL3_MIDI_SCOPE_COUNT) {
                snapshot->scope_active[voice->scope_slot] = 1u;
                snapshot->scope_note[voice->scope_slot] = voice->note;
                snapshot->scope_percussion[voice->scope_slot]
                    = voice->percussion;
            }
            ++snapshot->active_voice_count;
        }
    }
}

const char *opl3_midi_last_error(const opl3_midi_player *player)
{
    return player != NULL ? player->error : "Invalid OPL3 MIDI player";
}

const char *opl3_midi_mode_name(opl3_midi_mode mode)
{
    switch (mode) {
    case OPL3_MIDI_MODE_GM1: return "GM1";
    case OPL3_MIDI_MODE_GM2: return "GM2-compatible";
    case OPL3_MIDI_MODE_GS_COMPAT: return "GS reset / GM-compatible";
    case OPL3_MIDI_MODE_XG_COMPAT: return "XG reset / GM-compatible";
    default: return "Unknown";
    }
}

const char *opl3_midi_program_name(unsigned program)
{
    return program < 128u ? gm_program_names[program] : "Unknown program";
}

opl3_midi_program_family opl3_midi_program_family_for(unsigned program)
{
    if (program >= 128u) return OPL3_MIDI_FAMILY_SOUND_EFFECTS;
    return (opl3_midi_program_family)(program / 8u);
}

const char *opl3_midi_program_family_name(opl3_midi_program_family family)
{
    static const char *const names[] = {
        "Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass",
        "Strings", "Ensemble", "Brass", "Reed", "Pipe", "Synth Lead",
        "Synth Pad", "Synth Effects", "Ethnic", "Percussive",
        "Sound Effects", "Drums"
    };
    unsigned index = (unsigned)family;
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "Unknown";
}
