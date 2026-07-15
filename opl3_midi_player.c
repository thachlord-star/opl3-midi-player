#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "opl3_midi.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_WIDTH  960
#define APP_HEIGHT 640
#define APP_ERROR_LENGTH 256u
#define APP_PATH_LENGTH  1024u
#define APP_RENDER_FRAMES 1024u
#define APP_SCOPE_RING_SAMPLES 8192u
#define APP_SCOPE_INPUT_SAMPLES 4096u
#define APP_SCOPE_TRACE_SAMPLES 2048u
#define APP_SCOPE_COLUMNS 6u
#define APP_SCOPE_ROWS 3u

typedef struct scope_trigger_state {
    int16_t history[APP_SCOPE_TRACE_SAMPLES];
    float amplitude;
    int history_valid;
} scope_trigger_state;

typedef struct app_state {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_AudioDeviceID audio_device;
    opl3_midi_player *player;
    SDL_atomic_t paused;
    SDL_atomic_t peak_left;
    SDL_atomic_t peak_right;
    uint32_t sample_rate;
    int16_t scope_render[OPL3_MIDI_SCOPE_COUNT * APP_RENDER_FRAMES];
    int16_t scope_ring[OPL3_MIDI_SCOPE_COUNT][APP_SCOPE_RING_SAMPLES];
    size_t scope_write;
    size_t scope_count;
    int16_t scope_input[OPL3_MIDI_SCOPE_COUNT][APP_SCOPE_INPUT_SAMPLES];
    size_t scope_input_count;
    scope_trigger_state scope_triggers[OPL3_MIDI_SCOPE_COUNT];
    char path[APP_PATH_LENGTH];
    char message[APP_ERROR_LENGTH];
} app_state;

typedef struct glyph {
    char character;
    uint8_t rows[7];
} glyph;

static const glyph font_glyphs[] = {
    {'A',{14,17,17,31,17,17,17}}, {'B',{30,17,17,30,17,17,30}},
    {'C',{14,17,16,16,16,17,14}}, {'D',{30,17,17,17,17,17,30}},
    {'E',{31,16,16,30,16,16,31}}, {'F',{31,16,16,30,16,16,16}},
    {'G',{14,17,16,23,17,17,15}}, {'H',{17,17,17,31,17,17,17}},
    {'I',{14,4,4,4,4,4,14}},      {'J',{7,2,2,2,2,18,12}},
    {'K',{17,18,20,24,20,18,17}}, {'L',{16,16,16,16,16,16,31}},
    {'M',{17,27,21,21,17,17,17}}, {'N',{17,25,21,19,17,17,17}},
    {'O',{14,17,17,17,17,17,14}}, {'P',{30,17,17,30,16,16,16}},
    {'Q',{14,17,17,17,21,18,13}}, {'R',{30,17,17,30,20,18,17}},
    {'S',{15,16,16,14,1,1,30}},   {'T',{31,4,4,4,4,4,4}},
    {'U',{17,17,17,17,17,17,14}}, {'V',{17,17,17,17,17,10,4}},
    {'W',{17,17,17,17,21,21,10}}, {'X',{17,17,10,4,10,17,17}},
    {'Y',{17,17,10,4,4,4,4}},     {'Z',{31,1,2,4,8,16,31}},
    {'0',{14,17,19,21,25,17,14}}, {'1',{4,12,4,4,4,4,14}},
    {'2',{14,17,1,2,4,8,31}},     {'3',{30,1,1,14,1,1,30}},
    {'4',{2,6,10,18,31,2,2}},     {'5',{31,16,16,30,1,1,30}},
    {'6',{14,16,16,30,17,17,14}}, {'7',{31,1,2,4,8,8,8}},
    {'8',{14,17,17,14,17,17,14}}, {'9',{14,17,17,15,1,1,14}},
    {' ',{0,0,0,0,0,0,0}},       {':',{0,4,4,0,4,4,0}},
    {'.',{0,0,0,0,0,6,6}},       {'-',{0,0,0,31,0,0,0}},
    {'+',{0,4,4,31,4,4,0}},       {'/',{1,2,2,4,8,8,16}},
    {'(',{2,4,8,8,8,4,2}},        {')',{8,4,2,2,2,4,8}},
    {'[',{14,8,8,8,8,8,14}},      {']',{14,2,2,2,2,2,14}},
    {'_',{0,0,0,0,0,0,31}},       {'?',{14,17,1,2,4,0,4}},
    {'=',{0,0,31,0,31,0,0}},      {'%',{17,2,4,8,17,0,0}},
    {'#',{10,31,10,10,31,10,0}},  {',',{0,0,0,0,0,4,8}}
};

static void print_usage(FILE *stream, const char *program)
{
    fprintf(stream,
            "Usage: %s [options] [song.mid]\n"
            "\n"
            "Options:\n"
            "  --render FILE.wav  Render without opening an audio device\n"
            "  --info             Print parsed MIDI information and exit\n"
            "  --rate HZ          Output rate (8000..192000, default 48000)\n"
            "  --gain VALUE       Gain from 0.0 to 4.0 (default 1.0)\n"
            "  --loop             Start with looping enabled\n"
            "  --paused           Start paused\n"
            "  --gui-test         Exercise the SDL GUI and exit\n"
            "  -h, --help         Show this help\n"
            "\n"
            "GUI: drop a .mid/.midi file; Space pauses; Left/Right seeks;\n"
            "Home restarts; L loops; Up/Down changes gain; Esc quits.\n",
            program);
}

static const uint8_t *glyph_rows(char character)
{
    size_t i;
    static const uint8_t unknown[7] = {14, 17, 1, 2, 4, 0, 4};
    if (character >= 'a' && character <= 'z') {
        character = (char)(character - ('a' - 'A'));
    }
    for (i = 0; i < sizeof(font_glyphs) / sizeof(font_glyphs[0]); ++i) {
        if (font_glyphs[i].character == character) {
            return font_glyphs[i].rows;
        }
    }
    return unknown;
}

static void draw_text(SDL_Renderer *renderer,
                      int x,
                      int y,
                      int scale,
                      int max_width,
                      const char *text,
                      SDL_Color color)
{
    int origin = x;
    SDL_Rect pixel;
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    pixel.w = scale;
    pixel.h = scale;
    while (*text != '\0') {
        const uint8_t *rows;
        int row;
        int column;
        if (x + 5 * scale > origin + max_width) {
            break;
        }
        rows = glyph_rows(*text++);
        for (row = 0; row < 7; ++row) {
            for (column = 0; column < 5; ++column) {
                if ((rows[row] & (uint8_t)(1u << (4 - column))) != 0u) {
                    pixel.x = x + column * scale;
                    pixel.y = y + row * scale;
                    (void)SDL_RenderFillRect(renderer, &pixel);
                }
            }
        }
        x += 6 * scale;
    }
}

static void format_time(char *buffer, size_t size, double seconds)
{
    uint64_t whole;
    if (!isfinite(seconds) || seconds < 0.0) seconds = 0.0;
    whole = (uint64_t)seconds;
    if (whole >= 3600u) {
        (void)snprintf(buffer, size, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64,
                       whole / 3600u, (whole / 60u) % 60u, whole % 60u);
    } else {
        (void)snprintf(buffer, size, "%02" PRIu64 ":%02" PRIu64,
                       whole / 60u, whole % 60u);
    }
}

static void reset_scope_samples(app_state *app)
{
    memset(app->scope_ring, 0, sizeof(app->scope_ring));
    memset(app->scope_input, 0, sizeof(app->scope_input));
    memset(app->scope_triggers, 0, sizeof(app->scope_triggers));
    app->scope_write = 0u;
    app->scope_count = 0u;
    app->scope_input_count = 0u;
}

static void capture_scope_samples(app_state *app,
                                  const int16_t *scope_samples,
                                  size_t frame_count)
{
    size_t frame;
    unsigned slot;
    for (frame = 0u; frame < frame_count; ++frame) {
        size_t destination = (app->scope_write + frame)
                           % APP_SCOPE_RING_SAMPLES;
        for (slot = 0u; slot < OPL3_MIDI_SCOPE_COUNT; ++slot) {
            app->scope_ring[slot][destination]
                = scope_samples[(size_t)slot * frame_count + frame];
        }
    }
    app->scope_write = (app->scope_write + frame_count)
                     % APP_SCOPE_RING_SAMPLES;
    if (frame_count >= APP_SCOPE_RING_SAMPLES - app->scope_count) {
        app->scope_count = APP_SCOPE_RING_SAMPLES;
    } else {
        app->scope_count += frame_count;
    }
}

static void snapshot_scope_input(app_state *app)
{
    size_t count = app->scope_count;
    size_t start;
    size_t i;
    unsigned slot;
    if (count > APP_SCOPE_INPUT_SAMPLES) count = APP_SCOPE_INPUT_SAMPLES;
    start = (app->scope_write + APP_SCOPE_RING_SAMPLES - count)
          % APP_SCOPE_RING_SAMPLES;
    for (slot = 0u; slot < OPL3_MIDI_SCOPE_COUNT; ++slot) {
        for (i = 0u; i < count; ++i) {
            app->scope_input[slot][i]
                = app->scope_ring[slot][(start + i) % APP_SCOPE_RING_SAMPLES];
        }
    }
    app->scope_input_count = count;
}

static size_t scope_trace_length(const app_state *app)
{
    size_t length = app->sample_rate / 50u;
    if (length < 256u) length = 256u;
    if (length > APP_SCOPE_TRACE_SAMPLES) length = APP_SCOPE_TRACE_SAMPLES;
    return length;
}

static double scope_correlation(const int16_t *samples,
                                size_t start,
                                const int16_t *history,
                                size_t length)
{
    const size_t points = 96u;
    double dot = 0.0;
    double sample_energy = 0.0;
    double history_energy = 0.0;
    size_t point;
    for (point = 0u; point < points; ++point) {
        size_t offset = point * (length - 1u) / (points - 1u);
        double sample = samples[start + offset];
        double previous = history[offset];
        dot += sample * previous;
        sample_energy += sample * sample;
        history_energy += previous * previous;
    }
    if (sample_energy < 1.0 || history_energy < 1.0) return -1.0;
    return dot / sqrt(sample_energy * history_energy);
}

static size_t make_scope_trace(app_state *app,
                               unsigned slot,
                               int16_t *trace)
{
    const int16_t *samples = app->scope_input[slot];
    scope_trigger_state *state = &app->scope_triggers[slot];
    size_t count = app->scope_input_count;
    size_t length = scope_trace_length(app);
    size_t pretrigger = length / 4u;
    size_t search;
    size_t first_candidate;
    size_t last_candidate;
    size_t best_start;
    size_t candidate;
    size_t i;
    int peak = 0;
    double best_score = -2.0;
    double best_match = -1.0;
    int found = 0;

    if (count < length) {
        size_t padding = length - count;
        memset(trace, 0, padding * sizeof(*trace));
        memcpy(trace + padding, samples, count * sizeof(*trace));
        state->history_valid = 0;
        if (state->amplitude < 64.0f) state->amplitude = 64.0f;
        return length;
    }
    for (i = 0u; i < count; ++i) {
        int value = samples[i] == INT16_MIN ? 32768 : abs(samples[i]);
        if (value > peak) peak = value;
    }
    if (peak < 8) {
        memset(trace, 0, length * sizeof(*trace));
        state->history_valid = 0;
        state->amplitude *= 0.92f;
        if (state->amplitude < 64.0f) state->amplitude = 64.0f;
        return length;
    }

    search = length / 2u;
    if (search > count - length) search = count - length;
    last_candidate = count - (length - pretrigger);
    first_candidate = last_candidate - search;
    best_start = count - length;
    for (candidate = first_candidate + 2u;
         candidate + 2u <= last_candidate; ++candidate) {
        if (samples[candidate - 1u] <= 0 && samples[candidate] > 0) {
            size_t start = candidate - pretrigger;
            double slope = (double)samples[candidate + 2u]
                         - (double)samples[candidate - 2u];
            double edge = slope > 0.0 ? slope / ((double)peak * 2.0) : 0.0;
            double match = state->history_valid
                         ? scope_correlation(samples, start, state->history, length)
                         : 0.0;
            double recency = search != 0u
                           ? (double)(candidate - first_candidate) / search : 0.0;
            double score = state->history_valid
                         ? match * 0.88 + edge * 0.10 + recency * 0.02
                         : edge + recency * 0.05;
            if (!found || score > best_score) {
                found = 1;
                best_score = score;
                best_match = match;
                best_start = start;
            }
        }
    }

    memcpy(trace, samples + best_start, length * sizeof(*trace));
    if (!state->history_valid || !found || best_match < 0.10) {
        memcpy(state->history, trace, length * sizeof(*trace));
        state->history_valid = 1;
    } else {
        for (i = 0u; i < length; ++i) {
            int blended = (int)state->history[i]
                        + ((int)trace[i] - (int)state->history[i]) * 3 / 10;
            state->history[i] = (int16_t)blended;
        }
    }
    peak = 0;
    for (i = 0u; i < length; ++i) {
        int value = trace[i] == INT16_MIN ? 32768 : abs(trace[i]);
        if (value > peak) peak = value;
    }
    if ((float)peak > state->amplitude) {
        state->amplitude = (float)peak;
    } else {
        state->amplitude = state->amplitude * 0.96f + (float)peak * 0.04f;
    }
    if (state->amplitude < 64.0f) state->amplitude = 64.0f;
    return length;
}

static const char *scope_family_label(opl3_midi_program_family family)
{
    static const char *const labels[] = {
        "PIANO", "CHROM PERC", "ORGAN", "GUITAR", "BASS", "STRINGS",
        "ENSEMBLE", "BRASS", "REED", "PIPE", "SYNTH LEAD", "SYNTH PAD",
        "SYNTH FX", "ETHNIC", "PERCUSSIVE", "SOUND FX", "DRUMS"
    };
    unsigned index = (unsigned)family;
    return index < sizeof(labels) / sizeof(labels[0]) ? labels[index] : "UNKNOWN";
}

static SDL_Color scope_family_color(opl3_midi_program_family family, int active)
{
    static const SDL_Color colors[] = {
        {83u,214u,145u,255u}, {245u,198u,91u,255u},
        {91u,191u,245u,255u}, {229u,145u,83u,255u},
        {163u,130u,245u,255u}, {100u,210u,204u,255u},
        {131u,202u,112u,255u}, {245u,119u,119u,255u},
        {111u,174u,240u,255u}, {103u,219u,178u,255u},
        {230u,103u,210u,255u}, {179u,135u,240u,255u},
        {126u,157u,245u,255u}, {224u,172u,92u,255u},
        {240u,139u,91u,255u}, {145u,164u,194u,255u},
        {255u,157u,91u,255u}
    };
    unsigned index = (unsigned)family;
    SDL_Color color = index < sizeof(colors) / sizeof(colors[0])
                    ? colors[index] : (SDL_Color){145u,164u,194u,255u};
    if (!active) {
        color.r = (uint8_t)((unsigned)color.r * 5u / 8u);
        color.g = (uint8_t)((unsigned)color.g * 5u / 8u);
        color.b = (uint8_t)((unsigned)color.b * 5u / 8u);
    }
    return color;
}

static void draw_scope_panel(app_state *app,
                             const opl3_midi_snapshot *snapshot,
                             const opl3_midi_info *info,
                             unsigned slot,
                             SDL_Rect panel)
{
    int16_t trace[APP_SCOPE_TRACE_SAMPLES];
    size_t length = make_scope_trace(app, slot, trace);
    SDL_Rect plot = {panel.x + 4, panel.y + 29, panel.w - 8, panel.h - 34};
    SDL_Color line_color;
    char identity_label[64];
    char category_label[32];
    opl3_midi_program_family family = OPL3_MIDI_FAMILY_SOUND_EFFECTS;
    int assigned = info != NULL && slot < info->scope_count;
    int active = assigned && snapshot->scope_active[slot];
    int center = plot.y + plot.h / 2;
    int half_height = plot.h / 2 - 3;
    int previous_x = plot.x;
    int previous_y = center;
    int x;

    SDL_SetRenderDrawColor(app->renderer, 25u, 32u, 45u, 255u);
    (void)SDL_RenderFillRect(app->renderer, &panel);
    SDL_SetRenderDrawColor(app->renderer, 45u, 56u, 75u, 255u);
    (void)SDL_RenderDrawRect(app->renderer, &panel);
    SDL_SetRenderDrawColor(app->renderer, 48u, 61u, 80u, 255u);
    (void)SDL_RenderDrawLine(app->renderer, plot.x, center,
                            plot.x + plot.w - 1, center);
    (void)SDL_RenderDrawLine(app->renderer,
                            plot.x + plot.w / 4, plot.y,
                            plot.x + plot.w / 4, plot.y + plot.h - 1);

    if (assigned) {
        family = info->scope_channels[slot] == 9u
               ? OPL3_MIDI_FAMILY_DRUMS
               : opl3_midi_program_family_for(info->scope_programs[slot]);
        line_color = scope_family_color(family, active);
        (void)snprintf(identity_label, sizeof(identity_label),
                       "S%02u P%03u T%u M%02u", slot + 1u,
                       (unsigned)info->scope_programs[slot],
                       (unsigned)info->scope_tracks[slot] + 1u,
                       (unsigned)info->scope_channels[slot] + 1u);
        (void)snprintf(category_label, sizeof(category_label), "L%u %s",
                       (unsigned)info->scope_lanes[slot] + 1u,
                       scope_family_label(family));
    } else {
        line_color = (SDL_Color){95, 113, 140, 255};
        (void)snprintf(identity_label, sizeof(identity_label),
                       "S%02u EMPTY", slot + 1u);
        category_label[0] = '\0';
    }
    draw_text(app->renderer, panel.x + 5, panel.y + 5, 1,
              panel.w - 10, identity_label, line_color);
    draw_text(app->renderer, panel.x + 5, panel.y + 15, 1,
              panel.w - 10, category_label, line_color);

    SDL_SetRenderDrawColor(app->renderer, line_color.r, line_color.g,
                           line_color.b, line_color.a);
    for (x = 0; x < plot.w; ++x) {
        size_t sample_index = plot.w > 1
                            ? (size_t)x * (length - 1u) / (size_t)(plot.w - 1)
                            : 0u;
        float normalized = (float)trace[sample_index]
                         / app->scope_triggers[slot].amplitude;
        int y;
        if (normalized > 1.0f) normalized = 1.0f;
        if (normalized < -1.0f) normalized = -1.0f;
        y = center - (int)lroundf(normalized * half_height);
        if (x != 0) {
            (void)SDL_RenderDrawLine(app->renderer, previous_x, previous_y,
                                    plot.x + x, y);
        }
        previous_x = plot.x + x;
        previous_y = y;
    }
}

static void draw_scopes(app_state *app,
                        const opl3_midi_snapshot *snapshot,
                        const opl3_midi_info *info,
                        int x,
                        int y,
                        int width,
                        int height)
{
    int cell_width = width / (int)APP_SCOPE_COLUMNS;
    int cell_height = height / (int)APP_SCOPE_ROWS;
    unsigned slot;
    for (slot = 0u; slot < OPL3_MIDI_SCOPE_COUNT; ++slot) {
        unsigned column = slot % APP_SCOPE_COLUMNS;
        unsigned row = slot / APP_SCOPE_COLUMNS;
        SDL_Rect panel = {
            x + (int)column * cell_width,
            y + (int)row * cell_height,
            cell_width - 4,
            cell_height - 4
        };
        draw_scope_panel(app, snapshot, info, slot, panel);
    }
}

static const char *display_name(const app_state *app, const opl3_midi_info *info)
{
    const char *slash;
    const char *backslash;
    const char *base;
    if (info != NULL && info->title[0] != '\0') return info->title;
    if (app->path[0] == '\0') return "DROP A MIDI FILE INTO THIS WINDOW";
    slash = strrchr(app->path, '/');
    backslash = strrchr(app->path, '\\');
    base = slash != NULL ? slash + 1 : app->path;
    if (backslash != NULL && backslash + 1 > base) base = backslash + 1;
    return base;
}

static void draw_scene(app_state *app,
                       const opl3_midi_snapshot *snapshot,
                       const opl3_midi_info *info)
{
    double position = snapshot->sample_rate != 0u
                    ? (double)snapshot->position_frames / snapshot->sample_rate : 0.0;
    double duration = snapshot->sample_rate != 0u
                    ? (double)snapshot->duration_frames / snapshot->sample_rate : 0.0;
    double ratio = duration > 0.0 ? position / duration : 0.0;
    SDL_Rect progress_background = {40, 118, 880, 14};
    SDL_Rect progress = progress_background;
    char time_text[80];
    char position_text[24];
    char duration_text[24];
    char status_text[160];
    int peak_left = SDL_AtomicGet(&app->peak_left);
    int peak_right = SDL_AtomicGet(&app->peak_right);
    SDL_Rect left_meter = {40, 146, peak_left * 420 / 32768, 7};
    SDL_Rect right_meter = {500, 146, peak_right * 420 / 32768, 7};

    if (ratio < 0.0) ratio = 0.0;
    if (ratio > 1.0) ratio = 1.0;
    progress.w = (int)(progress_background.w * ratio);

    SDL_SetRenderDrawColor(app->renderer, 14u, 18u, 27u, 255u);
    (void)SDL_RenderClear(app->renderer);
    draw_text(app->renderer, 40, 28, 3, 880, "OPL3 MIDI PLAYER",
              (SDL_Color){83, 214, 145, 255});
    draw_text(app->renderer, 40, 72, 2, 880, display_name(app, info),
              (SDL_Color){231, 236, 244, 255});

    SDL_SetRenderDrawColor(app->renderer, 39u, 48u, 65u, 255u);
    (void)SDL_RenderFillRect(app->renderer, &progress_background);
    SDL_SetRenderDrawColor(app->renderer, 83u, 214u, 145u, 255u);
    (void)SDL_RenderFillRect(app->renderer, &progress);
    SDL_SetRenderDrawColor(app->renderer, 83u, 214u, 145u, 255u);
    (void)SDL_RenderFillRect(app->renderer, &left_meter);
    SDL_SetRenderDrawColor(app->renderer, 75u, 176u, 238u, 255u);
    (void)SDL_RenderFillRect(app->renderer, &right_meter);

    format_time(position_text, sizeof(position_text), position);
    format_time(duration_text, sizeof(duration_text), duration);
    (void)snprintf(time_text, sizeof(time_text), "%s / %s", position_text,
                   duration_text);
    draw_text(app->renderer, 40, 168, 2, 300, time_text,
              (SDL_Color){190, 200, 218, 255});
    (void)snprintf(status_text, sizeof(status_text),
                   "%s  VOICES %u/18  %s  GAIN %.1f",
                   snapshot->finished ? "FINISHED"
                   : SDL_AtomicGet(&app->paused) ? "PAUSED" : "PLAYING",
                   snapshot->active_voice_count,
                   snapshot->loop_enabled ? "LOOP ON" : "LOOP OFF",
                   snapshot->gain);
    draw_text(app->renderer, 350, 168, 2, 570, status_text,
              (SDL_Color){190, 200, 218, 255});

    draw_scopes(app, snapshot, info, 40, 202, 880, 386);

    if (app->message[0] != '\0') {
        draw_text(app->renderer, 40, 600, 1, 880, app->message,
                  (SDL_Color){255, 120, 120, 255});
    } else {
        draw_text(app->renderer, 40, 600, 1, 880,
                  "SPACE PLAY/PAUSE   LEFT/RIGHT SEEK   L LOOP   UP/DOWN GAIN",
                  (SDL_Color){127, 141, 164, 255});
    }
    SDL_RenderPresent(app->renderer);
}

static void SDLCALL audio_callback(void *userdata, Uint8 *stream, int length)
{
    app_state *app = (app_state *)userdata;
    int16_t *samples = (int16_t *)(void *)stream;
    size_t frames = length > 0 ? (size_t)length / (2u * sizeof(int16_t)) : 0u;
    size_t rendered = 0u;
    size_t i;
    int left_peak = 0;
    int right_peak = 0;

    if (app->player == NULL || SDL_AtomicGet(&app->paused)) {
        SDL_memset(stream, 0, (size_t)(length > 0 ? length : 0));
    } else {
        while (rendered < frames) {
            size_t chunk = frames - rendered;
            if (chunk > APP_RENDER_FRAMES) chunk = APP_RENDER_FRAMES;
            opl3_midi_render_scopes(app->player, samples + rendered * 2u,
                                    app->scope_render, chunk);
            capture_scope_samples(app, app->scope_render, chunk);
            rendered += chunk;
        }
        for (i = 0; i < frames; ++i) {
            int left = samples[i * 2u] == INT16_MIN ? 32768 : abs(samples[i * 2u]);
            int right = samples[i * 2u + 1u] == INT16_MIN
                      ? 32768 : abs(samples[i * 2u + 1u]);
            if (left > left_peak) left_peak = left;
            if (right > right_peak) right_peak = right;
        }
    }
    SDL_AtomicSet(&app->peak_left, left_peak);
    SDL_AtomicSet(&app->peak_right, right_peak);
}

static int load_song(app_state *app, const char *path)
{
    int result;
    SDL_LockAudioDevice(app->audio_device);
    result = opl3_midi_load_file(app->player, path);
    if (result) {
        reset_scope_samples(app);
        (void)snprintf(app->path, sizeof(app->path), "%s", path);
        app->message[0] = '\0';
        SDL_AtomicSet(&app->paused, 0);
    } else {
        (void)snprintf(app->message, sizeof(app->message), "%s",
                       opl3_midi_last_error(app->player));
    }
    SDL_UnlockAudioDevice(app->audio_device);
    if (result && app->window != NULL) {
        const char *name = strrchr(path, '/');
        char title[APP_PATH_LENGTH + 32u];
        name = name != NULL ? name + 1 : path;
        (void)snprintf(title, sizeof(title), "OPL3 MIDI Player - %s", name);
        SDL_SetWindowTitle(app->window, title);
    }
    return result;
}

static void seek_relative(app_state *app, double delta)
{
    opl3_midi_snapshot snapshot;
    double target;
    SDL_LockAudioDevice(app->audio_device);
    opl3_midi_get_snapshot(app->player, &snapshot);
    target = snapshot.sample_rate != 0u
           ? (double)snapshot.position_frames / snapshot.sample_rate + delta : 0.0;
    if (target < 0.0) target = 0.0;
    if (opl3_midi_seek_seconds(app->player, target)) reset_scope_samples(app);
    SDL_UnlockAudioDevice(app->audio_device);
}

static void set_gain_relative(app_state *app, float delta)
{
    opl3_midi_snapshot snapshot;
    SDL_LockAudioDevice(app->audio_device);
    opl3_midi_get_snapshot(app->player, &snapshot);
    opl3_midi_set_gain(app->player, snapshot.gain + delta);
    SDL_UnlockAudioDevice(app->audio_device);
}

static void handle_key(app_state *app, const SDL_KeyboardEvent *key, int *running)
{
    double seek_amount = (key->keysym.mod & KMOD_SHIFT) != 0 ? 30.0 : 5.0;
    switch (key->keysym.sym) {
    case SDLK_ESCAPE:
    case SDLK_q:
        *running = 0;
        break;
    case SDLK_SPACE:
        SDL_AtomicSet(&app->paused, !SDL_AtomicGet(&app->paused));
        break;
    case SDLK_LEFT:
        seek_relative(app, -seek_amount);
        break;
    case SDLK_RIGHT:
        seek_relative(app, seek_amount);
        break;
    case SDLK_HOME:
    case SDLK_r:
        SDL_LockAudioDevice(app->audio_device);
        opl3_midi_rewind(app->player);
        reset_scope_samples(app);
        SDL_UnlockAudioDevice(app->audio_device);
        break;
    case SDLK_END:
        SDL_LockAudioDevice(app->audio_device);
        {
            opl3_midi_snapshot snapshot;
            opl3_midi_get_snapshot(app->player, &snapshot);
            if (opl3_midi_seek_frames(app->player, snapshot.duration_frames)) {
                reset_scope_samples(app);
            }
        }
        SDL_UnlockAudioDevice(app->audio_device);
        break;
    case SDLK_l:
        SDL_LockAudioDevice(app->audio_device);
        {
            opl3_midi_snapshot snapshot;
            opl3_midi_get_snapshot(app->player, &snapshot);
            opl3_midi_set_loop(app->player, !snapshot.loop_enabled);
        }
        SDL_UnlockAudioDevice(app->audio_device);
        break;
    case SDLK_UP:
        set_gain_relative(app, 0.1f);
        break;
    case SDLK_DOWN:
        set_gain_relative(app, -0.1f);
        break;
    default:
        break;
    }
}

static void seek_from_mouse(app_state *app, int window_x, int window_y)
{
    float logical_x;
    float logical_y;
    opl3_midi_snapshot snapshot;
    SDL_RenderWindowToLogical(app->renderer, window_x, window_y,
                              &logical_x, &logical_y);
    if (logical_y < 118.0f || logical_y > 152.0f) return;
    if (logical_x < 40.0f) logical_x = 40.0f;
    if (logical_x > 920.0f) logical_x = 920.0f;
    SDL_LockAudioDevice(app->audio_device);
    opl3_midi_get_snapshot(app->player, &snapshot);
    if (opl3_midi_seek_frames(
            app->player,
            (uint64_t)((double)snapshot.duration_frames
                       * ((double)logical_x - 40.0) / 880.0))) {
        reset_scope_samples(app);
    }
    SDL_UnlockAudioDevice(app->audio_device);
}

static int run_gui(const char *input_path,
                   uint32_t requested_rate,
                   float gain,
                   int loop,
                   int initially_paused)
{
    app_state app;
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;
    int running = 1;
    int result = EXIT_SUCCESS;

    memset(&app, 0, sizeof(app));
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL initialization failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    app.window = SDL_CreateWindow("OPL3 MIDI Player",
                                  SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                  APP_WIDTH, APP_HEIGHT,
                                  SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (app.window == NULL) {
        fprintf(stderr, "Cannot create SDL window: %s\n", SDL_GetError());
        SDL_Quit();
        return EXIT_FAILURE;
    }
    app.renderer = SDL_CreateRenderer(app.window, -1,
                                      SDL_RENDERER_ACCELERATED
                                      | SDL_RENDERER_PRESENTVSYNC);
    if (app.renderer == NULL) {
        app.renderer = SDL_CreateRenderer(app.window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (app.renderer == NULL) {
        fprintf(stderr, "Cannot create SDL renderer: %s\n", SDL_GetError());
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    (void)SDL_RenderSetLogicalSize(app.renderer, APP_WIDTH, APP_HEIGHT);

    SDL_zero(desired);
    desired.freq = (int)requested_rate;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2u;
    desired.samples = APP_RENDER_FRAMES;
    desired.callback = audio_callback;
    desired.userdata = &app;
    app.audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained,
                                           SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (app.audio_device == 0u || obtained.format != AUDIO_S16SYS
        || obtained.channels != 2u) {
        fprintf(stderr, "Cannot open signed 16-bit stereo SDL audio: %s\n",
                SDL_GetError());
        if (app.audio_device != 0u) SDL_CloseAudioDevice(app.audio_device);
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    app.sample_rate = (uint32_t)obtained.freq;
    app.player = opl3_midi_create(app.sample_rate);
    if (app.player == NULL) {
        fprintf(stderr, "Cannot create OPL3 MIDI player at %u Hz\n", app.sample_rate);
        SDL_CloseAudioDevice(app.audio_device);
        SDL_DestroyRenderer(app.renderer);
        SDL_DestroyWindow(app.window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    opl3_midi_set_scope_capture(app.player, 1);
    opl3_midi_set_gain(app.player, gain);
    opl3_midi_set_loop(app.player, loop);
    SDL_AtomicSet(&app.paused, initially_paused || input_path == NULL);
    if (input_path != NULL && !load_song(&app, input_path)) {
        fprintf(stderr, "%s: %s\n", input_path, app.message);
        SDL_AtomicSet(&app.paused, 1);
        result = EXIT_FAILURE;
    } else if (initially_paused) {
        SDL_AtomicSet(&app.paused, 1);
    }
    SDL_PauseAudioDevice(app.audio_device, 0);

    while (running) {
        SDL_Event event;
        opl3_midi_snapshot snapshot;
        opl3_midi_info info;
        int have_info;

        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = 0;
                break;
            case SDL_KEYDOWN:
                if (event.key.repeat == 0) handle_key(&app, &event.key, &running);
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (event.button.button == SDL_BUTTON_LEFT) {
                    seek_from_mouse(&app, event.button.x, event.button.y);
                }
                break;
            case SDL_DROPFILE:
                if (event.drop.file != NULL) {
                    (void)load_song(&app, event.drop.file);
                    SDL_free(event.drop.file);
                }
                break;
            default:
                break;
            }
        }
        SDL_LockAudioDevice(app.audio_device);
        opl3_midi_get_snapshot(app.player, &snapshot);
        have_info = opl3_midi_get_info(app.player, &info);
        snapshot_scope_input(&app);
        SDL_UnlockAudioDevice(app.audio_device);
        draw_scene(&app, &snapshot, have_info ? &info : NULL);
        SDL_Delay(16u);
    }

    SDL_PauseAudioDevice(app.audio_device, 1);
    SDL_CloseAudioDevice(app.audio_device);
    opl3_midi_destroy(app.player);
    SDL_DestroyRenderer(app.renderer);
    SDL_DestroyWindow(app.window);
    SDL_Quit();
    return result;
}

static int gui_test(void)
{
    SDL_Window *window;
    SDL_Renderer *renderer;
    app_state app;
    opl3_midi_snapshot snapshot;
    opl3_midi_info info;
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;
    int have_info;
    int found_scope = 0;
    size_t scope_sample;
    static const uint8_t test_midi[] = {
        'M','T','h','d', 0,0,0,6, 0,0, 0,1, 0,96,
        'M','T','r','k', 0,0,0,12,
        0,0x90,60,100, 96,0x80,60,0, 0,0xff,0x2f,0
    };

    memset(&app, 0, sizeof(app));
    memset(&snapshot, 0, sizeof(snapshot));
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL GUI test initialization failed: %s\n", SDL_GetError());
        return EXIT_FAILURE;
    }
    window = SDL_CreateWindow("OPL3 MIDI GUI Test", SDL_WINDOWPOS_UNDEFINED,
                              SDL_WINDOWPOS_UNDEFINED, APP_WIDTH, APP_HEIGHT,
                              SDL_WINDOW_HIDDEN);
    renderer = window != NULL
             ? SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE) : NULL;
    if (renderer == NULL) {
        fprintf(stderr, "SDL GUI test setup failed: %s\n", SDL_GetError());
        if (window != NULL) SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    app.window = window;
    app.renderer = renderer;
    SDL_zero(desired);
    desired.freq = 48000;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2u;
    desired.samples = 256u;
    desired.callback = audio_callback;
    desired.userdata = &app;
    app.audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained,
                                           SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (app.audio_device == 0u || obtained.format != AUDIO_S16SYS
        || obtained.channels != 2u) {
        fprintf(stderr, "SDL GUI test audio setup failed: %s\n", SDL_GetError());
        if (app.audio_device != 0u) SDL_CloseAudioDevice(app.audio_device);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    app.sample_rate = (uint32_t)obtained.freq;
    app.player = opl3_midi_create(app.sample_rate);
    if (app.player != NULL) opl3_midi_set_scope_capture(app.player, 1);
    if (app.player == NULL
        || !opl3_midi_load_memory(app.player, test_midi, sizeof(test_midi))) {
        fprintf(stderr, "SDL GUI test MIDI setup failed\n");
        opl3_midi_destroy(app.player);
        SDL_CloseAudioDevice(app.audio_device);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_AtomicSet(&app.paused, 0);
    SDL_PauseAudioDevice(app.audio_device, 0);
    SDL_Delay(100u);
    SDL_LockAudioDevice(app.audio_device);
    opl3_midi_get_snapshot(app.player, &snapshot);
    have_info = opl3_midi_get_info(app.player, &info);
    snapshot_scope_input(&app);
    SDL_UnlockAudioDevice(app.audio_device);
    draw_scene(&app, &snapshot, have_info ? &info : NULL);
    if (snapshot.position_frames == 0u) {
        fprintf(stderr, "SDL GUI test audio callback did not advance playback\n");
        SDL_PauseAudioDevice(app.audio_device, 1);
        SDL_CloseAudioDevice(app.audio_device);
        opl3_midi_destroy(app.player);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    for (scope_sample = 0u; scope_sample < app.scope_input_count;
         ++scope_sample) {
        if (app.scope_input[0][scope_sample] != 0) {
            found_scope = 1;
            break;
        }
    }
    if (!found_scope) {
        fprintf(stderr, "SDL GUI test did not capture arranged scope data\n");
        SDL_PauseAudioDevice(app.audio_device, 1);
        SDL_CloseAudioDevice(app.audio_device);
        opl3_midi_destroy(app.player);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return EXIT_FAILURE;
    }
    SDL_PauseAudioDevice(app.audio_device, 1);
    SDL_CloseAudioDevice(app.audio_device);
    opl3_midi_destroy(app.player);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    puts("SDL GUI and audio-callback test passed");
    return EXIT_SUCCESS;
}

static void print_info(const opl3_midi_info *info)
{
    unsigned scope;
    if (info->title[0] != '\0') printf("Title: %s\n", info->title);
    printf("Format: %u\n", (unsigned)info->format);
    printf("Tracks: %u\n", (unsigned)info->track_count);
    if ((info->division & 0x8000u) != 0u) {
        unsigned fps_byte = info->division >> 8;
        printf("Division: SMPTE %d fps code, %u ticks/frame\n",
               fps_byte >= 128u ? (int)fps_byte - 256 : (int)fps_byte,
               info->division & 0xffu);
    } else {
        printf("Division: %u ticks/quarter-note\n", (unsigned)info->division);
    }
    printf("Scheduled events: %zu\n", info->event_count);
    printf("Duration: %.3f seconds\n", info->duration_seconds);
    if (info->has_mode_reset) {
        printf("Last recognized reset: %s\n",
               opl3_midi_mode_name(info->declared_mode));
    } else {
        puts("Last recognized reset: none (GM1 mapping is the playback default)");
    }
    printf("Instrument streams: %" PRIu32 "\n", info->instrument_stream_count);
    printf("Assigned scope lanes: %u/%u\n", (unsigned)info->scope_count,
           (unsigned)OPL3_MIDI_SCOPE_COUNT);
    for (scope = 0u; scope < info->scope_count; ++scope) {
        opl3_midi_program_family family
            = info->scope_channels[scope] == 9u
            ? OPL3_MIDI_FAMILY_DRUMS
            : opl3_midi_program_family_for(info->scope_programs[scope]);
        printf("Scope %02u: P%03u T%u M%02u L%u %s\n", scope + 1u,
               (unsigned)info->scope_programs[scope],
               (unsigned)info->scope_tracks[scope] + 1u,
               (unsigned)info->scope_channels[scope] + 1u,
               (unsigned)info->scope_lanes[scope] + 1u,
               opl3_midi_program_family_name(family));
    }
}

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)(value & 0xffu);
    destination[1] = (uint8_t)((value >> 8) & 0xffu);
    destination[2] = (uint8_t)((value >> 16) & 0xffu);
    destination[3] = (uint8_t)(value >> 24);
}

static int render_wav(opl3_midi_player *player,
                      const opl3_midi_info *info,
                      const char *path,
                      uint32_t sample_rate)
{
    uint64_t tail_frames = (uint64_t)sample_rate * 3u;
    uint64_t total_frames;
    uint64_t data_bytes;
    uint8_t header[44];
    int16_t samples[APP_RENDER_FRAMES * 2u];
    uint8_t little_endian[APP_RENDER_FRAMES * 4u];
    uint64_t rendered = 0u;
    FILE *file;

    if (info->duration_frames > UINT64_MAX - tail_frames) {
        fprintf(stderr, "MIDI duration is too large for WAV rendering\n");
        return 0;
    }
    total_frames = info->duration_frames + tail_frames;
    if (total_frames > (UINT32_MAX - 36u) / 4u) {
        fprintf(stderr, "WAV output would exceed the classic 4 GiB RIFF limit\n");
        return 0;
    }
    data_bytes = total_frames * 4u;
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno));
        return 0;
    }
    memset(header, 0, sizeof(header));
    memcpy(header, "RIFF", 4u);
    put_le32(header + 4u, (uint32_t)(36u + data_bytes));
    memcpy(header + 8u, "WAVEfmt ", 8u);
    put_le32(header + 16u, 16u);
    put_le16(header + 20u, 1u);
    put_le16(header + 22u, 2u);
    put_le32(header + 24u, sample_rate);
    put_le32(header + 28u, sample_rate * 4u);
    put_le16(header + 32u, 4u);
    put_le16(header + 34u, 16u);
    memcpy(header + 36u, "data", 4u);
    put_le32(header + 40u, (uint32_t)data_bytes);
    if (fwrite(header, 1u, sizeof(header), file) != sizeof(header)) {
        fprintf(stderr, "Cannot write WAV header: %s\n", strerror(errno));
        (void)fclose(file);
        return 0;
    }

    while (rendered < total_frames) {
        size_t frames = (size_t)(total_frames - rendered);
        size_t i;
        if (frames > APP_RENDER_FRAMES) frames = APP_RENDER_FRAMES;
        opl3_midi_render(player, samples, frames);
        for (i = 0; i < frames * 2u; ++i) {
            put_le16(little_endian + i * 2u, (uint16_t)samples[i]);
        }
        if (fwrite(little_endian, 4u, frames, file) != frames) {
            fprintf(stderr, "Cannot write WAV samples: %s\n", strerror(errno));
            (void)fclose(file);
            return 0;
        }
        rendered += frames;
    }
    if (fclose(file) != 0) {
        fprintf(stderr, "Cannot finish WAV file: %s\n", strerror(errno));
        return 0;
    }
    printf("Rendered %.3f seconds to %s\n",
           (double)total_frames / sample_rate, path);
    return 1;
}

static int parse_rate(const char *text, uint32_t *rate)
{
    char *end;
    unsigned long value;
    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0'
        || value < 8000ul || value > 192000ul) {
        return 0;
    }
    *rate = (uint32_t)value;
    return 1;
}

static int parse_gain(const char *text, float *gain)
{
    char *end;
    float value;
    errno = 0;
    value = strtof(text, &end);
    if (errno != 0 || end == text || *end != '\0'
        || !isfinite(value) || value < 0.0f || value > 4.0f) {
        return 0;
    }
    *gain = value;
    return 1;
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *render_path = NULL;
    uint32_t sample_rate = 48000u;
    float gain = 1.0f;
    int loop = 0;
    int paused = 0;
    int info_only = 0;
    int run_gui_test = 0;
    int i;

    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(stdout, argv[0]);
            return EXIT_SUCCESS;
        } else if (strcmp(argv[i], "--render") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "--render requires a WAV path\n");
                return EXIT_FAILURE;
            }
            render_path = argv[i];
        } else if (strcmp(argv[i], "--rate") == 0) {
            if (++i >= argc || !parse_rate(argv[i], &sample_rate)) {
                fprintf(stderr, "--rate must be an integer from 8000 to 192000\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--gain") == 0) {
            if (++i >= argc || !parse_gain(argv[i], &gain)) {
                fprintf(stderr, "--gain must be a number from 0.0 to 4.0\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "--loop") == 0) {
            loop = 1;
        } else if (strcmp(argv[i], "--paused") == 0) {
            paused = 1;
        } else if (strcmp(argv[i], "--info") == 0) {
            info_only = 1;
        } else if (strcmp(argv[i], "--gui-test") == 0) {
            run_gui_test = 1;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(stderr, argv[0]);
            return EXIT_FAILURE;
        } else if (input_path == NULL) {
            input_path = argv[i];
        } else {
            fprintf(stderr, "Only one input MIDI file can be played at a time\n");
            return EXIT_FAILURE;
        }
    }

    if (run_gui_test) {
        return gui_test();
    }
    if (render_path != NULL || info_only) {
        opl3_midi_player *player;
        opl3_midi_info info;
        int result = EXIT_SUCCESS;
        if (input_path == NULL) {
            fprintf(stderr, "A MIDI input path is required for --info or --render\n");
            return EXIT_FAILURE;
        }
        player = opl3_midi_create(sample_rate);
        if (player == NULL) {
            fprintf(stderr, "Cannot create OPL3 MIDI player at %u Hz\n", sample_rate);
            return EXIT_FAILURE;
        }
        if (!opl3_midi_load_file(player, input_path)) {
            fprintf(stderr, "%s: %s\n", input_path,
                    opl3_midi_last_error(player));
            opl3_midi_destroy(player);
            return EXIT_FAILURE;
        }
        (void)opl3_midi_get_info(player, &info);
        if (info_only) print_info(&info);
        if (render_path != NULL) {
            opl3_midi_set_gain(player, gain);
            result = render_wav(player, &info, render_path, sample_rate)
                   ? EXIT_SUCCESS : EXIT_FAILURE;
        }
        opl3_midi_destroy(player);
        return result;
    }
    return run_gui(input_path, sample_rate, gain, loop, paused);
}
