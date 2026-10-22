/* Wearable Sensing DSI -> LSL integration.
 * Copyright (C) 2014-2020 Syntrogi Inc dba Intheon.
 * Extended with electrode impedance, state markers and runtime controls.
 */
#include "DSI.h"
/* Use the header supplied by the selected modern liblsl, not the legacy local copy. */
/* Some source-build LSL targets propagate their own export macro to consumers. */
#ifdef LIBLSL_EXPORTS
#undef LIBLSL_EXPORTS
#endif
#include <lsl_c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <math.h>
#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <sys/select.h>
#endif

typedef struct {
    DSI_Headset h;
    lsl_outlet eeg, impedance, markers;
    DSI_Channel *channels;
    DSI_Source *sources;
    float *eeg_sample;
    double *impedance_sample;
    int channel_count, source_count;
    int driver_mode, reset_mode, requested_on, command_pending, failed;
    double settle_seconds, valid_after, next_impedance, next_snapshot;
    int gui_telemetry;
    unsigned long long sample_count;
    double last_impedance_time;
    int acquiring, acquisition_attempted, recording_events, startup_impedance;
} App;

static volatile sig_atomic_t keep_running = 1;
static void quit_handler(int sig) { (void)sig; keep_running = 0; }

/* Legacy option syntax: --name=value, -p=value, /port:COM4. */
static const char *option(int argc, const char *argv[], const char *name, const char *alias)
{
    const char *result = NULL;
    int i, j;
    for (i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (*arg != '-' && *arg != '/') continue;
        ++arg;
        if (*arg == '-' || *arg == '/') ++arg;
        for (j = 0; j < 2; ++j) {
            const char *key = j ? alias : name;
            size_t n;
            if (!key) continue;
            n = strlen(key);
            if (!strncmp(arg, key, n) &&
                (arg[n] == '=' || arg[n] == ':' || arg[n] == '\0'))
                result = arg + n + (arg[n] != '\0');
        }
    }
    return result;
}

static int check_dsi(const char *operation)
{
    if (!DSI_Error()) return 0;
    fprintf(stderr, "%s: %s\n", operation, DSI_ClearError());
    return -1;
}

/* Optional line-delimited GUI protocol. Never mix diagnostics into its JSON payload. */
static void json_string(const char *text)
{
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    putchar('"');
    for (; *p; ++p) {
        if (*p == '"' || *p == '\\') { putchar('\\'); putchar(*p); }
        else if (*p < 32) printf("\\u%04x", (unsigned int)*p);
        else putchar(*p);
    }
    putchar('"');
}

static void gui_state(App *a, double now)
{
    int i, ready;
    if (!a->gui_telemetry) return;
    ready = a->acquiring && a->driver_mode == 1 && a->reset_mode == 0 && !a->command_pending && now >= a->valid_after;
    printf("DSI_GUI {\"protocol\":1,\"type\":\"state\",\"timestamp\":%.6f,"
           "\"driver_mode\":%d,\"reset_mode\":%d,\"requested_on\":%d,"
           "\"command_pending\":%d,\"impedance_ready\":%d,\"sample_count\":%llu,"
           "\"impedance_timestamp\":%.6f,\"acquiring\":%d,"
           "\"eeg_subscribers\":%d,\"impedance_subscribers\":%d,\"marker_subscribers\":%d,\"impedance\":[",
           now, a->driver_mode, a->reset_mode, a->requested_on, a->command_pending,
           ready, a->sample_count, a->last_impedance_time, a->acquiring,
           a->eeg ? lsl_have_consumers(a->eeg) : 0,
           a->impedance ? lsl_have_consumers(a->impedance) : 0,
           a->markers ? lsl_have_consumers(a->markers) : 0);
    for (i = 0; i <= a->source_count; ++i) {
        if (i) putchar(',');
        if (ready && a->last_impedance_time > 0 && a->last_impedance_time >= a->valid_after &&
            a->impedance_sample && isfinite(a->impedance_sample[i]))
            printf("%.9g", a->impedance_sample[i]);
        else fputs("null", stdout);
    }
    fputs("]}\n", stdout);
    fflush(stdout);
}

static void gui_config(App *a, const char *name)
{
    int i;
    char cmf_label[128];
    if (!a->gui_telemetry) return;
    fputs("DSI_GUI {\"protocol\":1,\"type\":\"config\",\"stream_name\":", stdout);
    json_string(name);
    fputs(",\"model\":", stdout); json_string(DSI_Headset_GetHardwareModel(a->h));
    printf(",\"serial\":%u,\"sampling_rate\":%.9g,\"eeg_channels\":%d,\"labels\":[",
           DSI_Headset_GetSerialNumber(a->h), DSI_Headset_GetSamplingRate(a->h), a->channel_count);
    for (i = 0; i < a->source_count; ++i) {
        if (i) putchar(',');
        json_string(DSI_Source_GetName(a->sources[i]));
    }
    if (a->source_count) putchar(',');
    snprintf(cmf_label, sizeof(cmf_label), "CMF=%s", DSI_Headset_GetFactoryReferenceString(a->h));
    json_string(cmf_label);
    fputs("]}\n", stdout);
    fflush(stdout);
}

static void marker(App *a, const char *event, double now)
{
    const char *sample[] = { event };
    /* Advertise outlets immediately, but do not push any LSL samples before Start. */
    if (a->markers && a->recording_events && lsl_push_sample_strt(a->markers, sample, now) != 0) {
        fprintf(stderr, "Could not publish LSL marker: %s\n", event);
        a->failed = 1;
    }
    fprintf(stderr, "[%.6f] %s\n", now, event);
    if (a->gui_telemetry) {
        printf("DSI_GUI {\"protocol\":1,\"type\":\"event\",\"timestamp\":%.6f,\"event\":", now);
        json_string(event);
        fputs("}\n", stdout);
        fflush(stdout);
    }
}

static void snapshot(App *a, double now)
{
    char text[160];
    snprintf(text, sizeof(text),
             "state driver_mode=%d reset_mode=%d requested_on=%d command_pending=%d impedance_ready=%d",
             a->driver_mode, a->reset_mode, a->requested_on, a->command_pending,
              a->acquiring && a->driver_mode == 1 && a->reset_mode == 0 && !a->command_pending && now >= a->valid_after);
    marker(a, text, now);
    gui_state(a, now);
}

/* These getters expose the API's observed state, not a sample-exact hardware edge. */
static int observe_state(App *a)
{
    int changed = 0;
    int driver = DSI_Headset_GetImpedanceDriverMode(a->h);
    int reset = DSI_Headset_GetAnalogResetMode(a->h);
    double now = lsl_local_clock();
    char text[100];
    if (check_dsi("Read hardware state")) return -1;
    if (driver != a->driver_mode) {
        snprintf(text, sizeof(text), "impedance_state mode=%d", driver);
        marker(a, text, now);
        a->driver_mode = driver;
        a->valid_after = now + a->settle_seconds;
        changed = 1;
    }
    if (reset != a->reset_mode) {
        snprintf(text, sizeof(text), "analog_reset_state mode=%d", reset);
        marker(a, text, now);
        a->reset_mode = reset;
        a->valid_after = now + a->settle_seconds;
        changed = 1;
    }
    if (changed) gui_state(a, now);
    return 0;
}

/* Serial commands stay on the acquisition thread; never run them in a sample callback. */
static int set_impedance(App *a, int on)
{
    double now = lsl_local_clock();
    a->requested_on = on;
    a->command_pending = 1;
    a->valid_after = HUGE_VAL;
    marker(a, on ? "impedance_on_requested" : "impedance_off_requested", now);
    gui_state(a, now);
    if (on) DSI_Headset_StartImpedanceDriver(a->h);
    else DSI_Headset_StopImpedanceDriver(a->h);
    a->command_pending = 0;
    if (check_dsi("Set impedance driver")) {
        marker(a, "impedance_command_failed", lsl_local_clock());
        return -1;
    }
    a->valid_after = lsl_local_clock() + a->settle_seconds;
    marker(a, on ? "impedance_on_command_returned" : "impedance_off_command_returned", lsl_local_clock());
    return observe_state(a);
}

static int analog_reset(App *a)
{
    a->command_pending = 1;
    a->valid_after = HUGE_VAL;
    marker(a, "analog_reset_requested", lsl_local_clock());
    gui_state(a, lsl_local_clock());
    /* One-shot reset. Do NOT LockAnalogReset: that invalidates signal data indefinitely. */
    DSI_Headset_StartAnalogReset(a->h);
    a->command_pending = 0;
    if (check_dsi("Analog reset")) {
        marker(a, "analog_reset_command_failed", lsl_local_clock());
        return -1;
    }
    a->valid_after = lsl_local_clock() + a->settle_seconds;
    marker(a, "analog_reset_command_returned", lsl_local_clock());
    return observe_state(a);
}

static void on_sample(DSI_Headset h, double packet_time, void *userdata)
{
    App *a = (App *)userdata;
    double now = lsl_local_clock();
    int i, ready;
    (void)h;
    (void)packet_time; /* Preserve upstream receipt-time stamping; see README timing limitations. */
    if (!a->acquiring) return; /* Also discard trailing packets during a pause/shutdown. */
    ++a->sample_count;
    for (i = 0; i < a->channel_count; ++i)
        a->eeg_sample[i] = (float)DSI_Channel_GetSignal(a->channels[i]);
    if (DSI_Error()) { a->failed = 1; return; }
    if (lsl_push_sample_ft(a->eeg, a->eeg_sample, now) != 0) a->failed = 1;

    /* Poll estimates at 1 Hz. This is NOT an independent 1 Hz hardware sampler. */
    if (now >= a->next_impedance) {
        ready = a->driver_mode == 1 && a->reset_mode == 0 &&
                !a->command_pending && now >= a->valid_after;
        for (i = 0; i < a->source_count; ++i)
            a->impedance_sample[i] = ready ? DSI_Source_GetImpedanceEEG(a->sources[i]) : NAN;
        a->impedance_sample[a->source_count] = ready ? DSI_Headset_GetImpedanceCMF(a->h) : NAN;
        if (DSI_Error()) { a->failed = 1; return; }
        for (i = 0; i <= a->source_count; ++i)
            if (!isfinite(a->impedance_sample[i]) || a->impedance_sample[i] < 0)
                a->impedance_sample[i] = NAN;
        if (lsl_push_sample_dt(a->impedance, a->impedance_sample, now) != 0) a->failed = 1;
        a->next_impedance = now + 1.0;
        a->last_impedance_time = now;
    }
    /* A late-starting recorder receives the state without needing the initial edge. */
    if (now >= a->next_snapshot) {
        snapshot(a, now);
        a->next_snapshot = now + 1.0;
    }
}

static int message(const char *text, int level)
{
    return fprintf(stderr, "DSI Message (level %d): %s\n", level, text);
}

static lsl_streaminfo stream_info(App *a, const char *name, const char *type,
                                 int count, double rate, lsl_channel_format_t format,
                                 const char *id, const char *group)
{
    lsl_streaminfo info = lsl_create_streaminfo(name, type, count, rate, format, id);
    lsl_xml_ptr desc;
    char serial[32], settling[32];
    if (!info) return NULL;
    desc = lsl_get_desc(info);
    snprintf(serial, sizeof(serial), "%u", DSI_Headset_GetSerialNumber(a->h));
    snprintf(settling, sizeof(settling), "%.3f", a->settle_seconds);
    lsl_append_child_value(desc, "manufacturer", "WearableSensing");
    lsl_append_child_value(desc, "serial_number", serial);
    lsl_append_child_value(desc, "device_model", DSI_Headset_GetHardwareModel(a->h));
    lsl_append_child_value(desc, "firmware_version", DSI_Headset_GetFirmwareRevision(a->h));
    lsl_append_child_value(desc, "dsi_api_version", DSI_GetAPIVersion());
    lsl_append_child_value(desc, "stream_group", group);
    lsl_append_child_value(desc, "timestamp_method", "host_callback_receipt_lsl_local_clock");
    lsl_append_child_value(desc, "impedance_test_frequencies_hz", "110,130");
    lsl_append_child_value(desc, "impedance_settle_seconds", settling);
    return info;
}

static void channel_metadata(lsl_xml_ptr channels, const char *label, const char *type, const char *unit)
{
    lsl_xml_ptr ch = lsl_append_child(channels, "channel");
    lsl_append_child_value(ch, "label", label);
    lsl_append_child_value(ch, "type", type);
    lsl_append_child_value(ch, "unit", unit);
}

static int init_streams(App *a, const char *name)
{
    char group[256], id[280], imp_name[280], marker_name[280], cmf_label[128];
    lsl_streaminfo info;
    lsl_xml_ptr desc, channels;
    int i, total = DSI_Headset_GetNumberOfSources(a->h);
    double rate = DSI_Headset_GetSamplingRate(a->h);
    a->channel_count = DSI_Headset_GetNumberOfChannels(a->h);
    if (check_dsi("Get channel counts") || total < 0 || a->channel_count <= 0 || rate <= 0) return -1;
    a->channels = (DSI_Channel *)calloc((size_t)a->channel_count, sizeof(*a->channels));
    a->sources = (DSI_Source *)calloc((size_t)total + 1, sizeof(*a->sources));
    a->eeg_sample = (float *)calloc((size_t)a->channel_count, sizeof(float));
    a->impedance_sample = (double *)calloc((size_t)total + 1, sizeof(double));
    if (!a->channels || !a->sources || !a->eeg_sample || !a->impedance_sample) return -1;
    for (i = 0; i < a->channel_count; ++i) {
        a->channels[i] = DSI_Headset_GetChannelByIndex(a->h, (unsigned int)i);
        if (!a->channels[i] || check_dsi("Get EEG channel")) return -1;
    }
    /* Match the vendor demo: physical referential sources, excluding the virtual factory reference. */
    for (i = 0; i < total; ++i) {
        DSI_Source s = DSI_Headset_GetSourceByIndex(a->h, (unsigned int)i);
        if (!s || check_dsi("Get electrode source")) return -1;
        if (DSI_Source_IsReferentialEEG(s) && !DSI_Source_IsFactoryReference(s))
            a->sources[a->source_count++] = s;
    }
    if (strlen(name) > 180) { fprintf(stderr, "Stream name is too long\n"); return -1; }
    snprintf(group, sizeof(group), "WearableSensing-%u-%s", DSI_Headset_GetSerialNumber(a->h), name);
    snprintf(id, sizeof(id), "%s-eeg", group);
    info = stream_info(a, name, "EEG", a->channel_count, rate, cft_float32, id, group);
    if (!info) return -1;
    desc = lsl_get_desc(info);
    lsl_append_child_value(desc, "reference", DSI_Headset_GetReferenceString(a->h));
    channels = lsl_append_child(desc, "channels");
    for (i = 0; i < a->channel_count; ++i) {
        DSI_Channel c = a->channels[i];
        int trigger = DSI_Channel_IsTrigger(c);
        channel_metadata(channels, DSI_Channel_GetName(c),
                         trigger ? "Trigger" : (DSI_Channel_IsReferentialEEG(c) ? "EEG" : "Auxiliary"),
                         trigger ? "integer" : (DSI_Channel_IsReferentialEEG(c) ? "microvolts" : "vendor_units"));
        /* Store the full montage expression without modifying API-owned strings. */
        lsl_append_child_value(lsl_last_child(channels), "montage", DSI_Channel_GetString(c));
    }
    a->eeg = lsl_create_outlet(info, 0, 360);
    lsl_destroy_streaminfo(info);
    if (!a->eeg) return -1;

    snprintf(id, sizeof(id), "%s-impedance", group);
    snprintf(imp_name, sizeof(imp_name), "%s-Impedance", name);
    /* Irregular rate: no samples when EEG stops, no invented samples after stalls. */
    info = stream_info(a, imp_name, "Impedance", a->source_count + 1, 0, cft_double64, id, group);
    if (!info) return -1;
    desc = lsl_get_desc(info);
    lsl_append_child_value(desc, "poll_interval_seconds", "1");
    lsl_append_child_value(desc, "invalid_value", "NaN: off, resetting, settling, negative or nonfinite API estimate");
    lsl_append_child_value(desc, "measurement", "physical_sources_not_montage_channels");
    channels = lsl_append_child(desc, "channels");
    for (i = 0; i < a->source_count; ++i)
        channel_metadata(channels, DSI_Source_GetName(a->sources[i]), "Impedance", "ohms");
    snprintf(cmf_label, sizeof(cmf_label), "CMF=%s", DSI_Headset_GetFactoryReferenceString(a->h));
    channel_metadata(channels, cmf_label, "Impedance", "ohms");
    a->impedance = lsl_create_outlet(info, 0, 360);
    lsl_destroy_streaminfo(info);
    if (!a->impedance) return -1;

    snprintf(id, sizeof(id), "%s-impedance-markers", group);
    snprintf(marker_name, sizeof(marker_name), "%s-ImpedanceMarkers", name);
    info = stream_info(a, marker_name, "Markers", 1, 0, cft_string, id, group);
    if (!info) return -1;
    desc = lsl_get_desc(info);
    lsl_append_child_value(desc, "state_semantics", "API observed modes, not sample-exact hardware transitions; 0=off, 1=on, other=unknown/unsupported");
    lsl_append_child_value(desc, "snapshot_interval_seconds", "1");
    channels = lsl_append_child(desc, "channels");
    channel_metadata(channels, "impedance_event", "Marker", "string");
    a->markers = lsl_create_outlet(info, 0, 360);
    lsl_destroy_streaminfo(info);
    return a->markers && !check_dsi("Create stream metadata") ? 0 : -1;
}

static int start_acquisition(App *a)
{
    if (a->acquiring) return 0;
    a->recording_events = 1;
    marker(a, "acquisition_start_requested", lsl_local_clock());
    if (set_impedance(a, a->startup_impedance)) return -1;
    a->acquisition_attempted = 1;
    DSI_Headset_StartDataAcquisition(a->h);
    if (check_dsi("Start acquisition")) {
        marker(a, "acquisition_start_failed", lsl_local_clock());
        return -1;
    }
    a->acquiring = 1;
    a->next_impedance = a->next_snapshot = 0;
    marker(a, "acquisition_started", lsl_local_clock());
    gui_state(a, lsl_local_clock());
    return 0;
}

static int pause_acquisition(App *a)
{
    if (!a->acquiring && !a->acquisition_attempted) return 0;
    marker(a, "acquisition_stop_requested", lsl_local_clock());
    a->acquiring = 0; /* Gate publishing before commands can deliver trailing callbacks. */
    a->last_impedance_time = 0;
    if (set_impedance(a, 0)) return -1;
    DSI_Headset_StopDataAcquisition(a->h);
    if (check_dsi("Stop acquisition")) return -1;
    DSI_Headset_Idle(a->h, 0.1);
    if (check_dsi("Drain serial data")) return -1;
    a->acquisition_attempted = 0;
    marker(a, "acquisition_stopped", lsl_local_clock());
    gui_state(a, lsl_local_clock());
    return 0;
}

static int command(App *a, const char *text)
{
    if (!strcmp(text, "start")) return start_acquisition(a);
    if (!strcmp(text, "stop")) return pause_acquisition(a);
    if (!a->acquiring && (!strcmp(text, "on") || !strcmp(text, "i"))) {
        fprintf(stderr, "Start acquisition before enabling impedance.\n"); return 0;
    }
    if (!strcmp(text, "on") || !strcmp(text, "i")) return set_impedance(a, 1);
    if (!strcmp(text, "off") || !strcmp(text, "o")) return set_impedance(a, 0);
    if (!strcmp(text, "reset") || !strcmp(text, "r")) return analog_reset(a);
    if (!strcmp(text, "status") || !strcmp(text, "s")) { snapshot(a, lsl_local_clock()); return 0; }
    if (!strcmp(text, "quit") || !strcmp(text, "q")) { keep_running = 0; return 0; }
    if (*text) fprintf(stderr, "Commands (then Enter): start, stop, on/i, off/o, reset/r, status/s, quit/q\n");
    return 0;
}

/* Nonblocking stdin, both terminals and subprocess pipes. EOF does not stop EEG. */
static int poll_commands(App *a)
{
    static char line[128];
    static size_t used = 0;
    static int closed = 0, overflow = 0;
    char input[256];
    int count = 0, i;
    if (closed) return 0;
#ifdef _WIN32
    HANDLE handle = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode, available, n;
    if (GetConsoleMode(handle, &mode)) {
        while (count < (int)sizeof(input) && _kbhit()) {
            int c = _getch();
            if (c == 0 || c == 224) { (void)_getch(); continue; }
            if (c == 3) { keep_running = 0; break; }
            input[count++] = (char)c;
            if (c == '\r') fputs("\n", stderr);
            else fputc(c, stderr);
        }
    } else if (GetFileType(handle) == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(handle, NULL, 0, NULL, &available, NULL)) closed = 1;
        else if (available && ReadFile(handle, input, sizeof(input), &n, NULL)) count = (int)n;
    } else closed = 1; /* Redirected disk file is not an interactive control channel. */
#else
    fd_set ready;
    struct timeval timeout = {0, 0};
    FD_ZERO(&ready);
    FD_SET(STDIN_FILENO, &ready);
    if (select(STDIN_FILENO + 1, &ready, NULL, NULL, &timeout) > 0) {
        ssize_t n = read(STDIN_FILENO, input, sizeof(input));
        if (n > 0) count = (int)n;
        else if (n == 0 || (errno != EINTR && errno != EAGAIN)) closed = 1;
    }
#endif
    for (i = 0; i < count && keep_running; ++i) {
        char c = input[i];
        if (c == '\r' || c == '\n') {
            line[used] = '\0';
            if (!overflow && command(a, line)) return -1;
            used = 0; overflow = 0;
        } else if (c == '\b' || c == 127) {
            if (used) --used;
        } else if (used < sizeof(line) - 1) line[used++] = c;
        else overflow = 1; /* Discard a truncated command rather than executing its prefix. */
    }
    return 0;
}

static void help(const char *program)
{
    fprintf(stderr,
        "Usage: %s [--NAME=VALUE ...]\n"
        "  --help, -h                  Show help without loading libraries/headset\n"
        "  --port=PORT, -p=PORT        COM4 (Windows), /dev/cu.* (macOS), or DSISerialPort\n"
        "  --montage=SPEC, -m=SPEC     DSI channel montage (default: vendor selection)\n"
        "  --reference=SPEC, -r=SPEC   DSI reference (default: vendor selection)\n"
        "  --verbosity=N, -v=N        DSI verbosity (default: 2)\n"
        "  --lsl-stream-name=NAME      EEG stream name (default: WS-default)\n"
        "  --impedance=off|on         Startup driver state (default: off)\n"
        "  --impedance-settle=SECONDS Suppress estimates after transitions (default: 2)\n"
        "  --dsi-library=PATH         Override bundled DSI shared library\n"
        "  --check-libraries          Load DSI/LSL and report versions, without connecting\n"
        "  --gui-telemetry            Emit DSI_GUI JSON records on stdout (GUI integration)\n"
        "  --auto-start               Start acquisition immediately (default: wait for start)\n"
        "Runtime stdin commands + Enter: start, stop, on/i, off/o, reset/r, status/s, quit/q\n"
        "Creates EEG, NAME-Impedance (ohms), NAME-ImpedanceMarkers streams.\n"
        "Default: advertise all outlets, then wait for start; stop keeps outlets alive.\n"
        "EEG remains unfiltered. Reset is an analog amplifier reset, not impedance calibration.\n", program);
}

int main(int argc, const char *argv[])
{
    App a = {0};
    const char *name, *imp, *settle, *verbosity, *library;
    char *end;
    long level = 2;
    int result = 1, connected = 0, load;
    if (option(argc, argv, "help", "h")) { help(argv[0]); return 0; }
    a.gui_telemetry = option(argc, argv, "gui-telemetry", NULL) != NULL;
    name = option(argc, argv, "lsl-stream-name", NULL); /* -m belongs only to montage. */
    if (!name) name = "WS-default";
    imp = option(argc, argv, "impedance", NULL);
    if (imp && strcmp(imp, "off") && strcmp(imp, "on")) {
        fprintf(stderr, "--impedance must be off or on\n"); return 1;
    }
    a.settle_seconds = 2.0;
    settle = option(argc, argv, "impedance-settle", NULL);
    if (settle) {
        errno = 0; a.settle_seconds = strtod(settle, &end);
        if (end == settle || *end || errno || !isfinite(a.settle_seconds) || a.settle_seconds < 0) {
            fprintf(stderr, "Invalid --impedance-settle\n"); return 1;
        }
    }
    verbosity = option(argc, argv, "verbosity", "v");
    if (verbosity) {
        errno = 0; level = strtol(verbosity, &end, 10);
        if (end == verbosity || *end || errno || level < 0 || level > INT_MAX) {
            fprintf(stderr, "Invalid --verbosity\n"); return 1;
        }
    }
    library = option(argc, argv, "dsi-library", NULL);
#ifdef DSI_BUNDLED_LIBRARY
    if (!library) library = DSI_BUNDLED_LIBRARY;
#endif
    load = Load_DSI_API(library);
    if (load) {
        fprintf(stderr, "Cannot load DSI library %s (loader result %d). Use --dsi-library=PATH.\n",
                DSI_DYLIB_NAME(library), load); return 1;
    }
    fprintf(stderr, "DSI API %s loaded (header %s); liblsl %d\n",
            DSI_GetAPIVersion(), DSI_API_VERSION, lsl_library_version());
    if (strcmp(DSI_GetAPIVersion(), DSI_API_VERSION))
        fprintf(stderr, "WARNING: DSI header/library version mismatch; obtain matching files from Wearable Sensing.\n");
    if (option(argc, argv, "check-libraries", NULL)) return 0;
    signal(SIGINT, quit_handler);
    signal(SIGTERM, quit_handler);
    a.driver_mode = a.reset_mode = -999;
    a.h = DSI_Headset_New(NULL);
    if (check_dsi("Create headset") || !a.h) goto cleanup;
    DSI_Headset_SetMessageCallback(a.h, message);
    DSI_Headset_SetVerbosity(a.h, (int)level);
    if (check_dsi("Configure logging")) goto cleanup;
    DSI_Headset_Connect(a.h, option(argc, argv, "port", "p"));
    if (check_dsi("Connect")) goto cleanup;
    connected = 1;
    /* Ensure a previously active headset cannot feed data during preparation. */
    DSI_Headset_StopDataAcquisition(a.h);
    if (check_dsi("Ensure acquisition is stopped")) goto cleanup;
    DSI_Headset_ChooseChannels(a.h, option(argc, argv, "montage", "m"),
                             option(argc, argv, "reference", "r"), 1);
    if (check_dsi("Configure montage") || init_streams(&a, name)) goto cleanup;
    gui_config(&a, name);
    if (check_dsi("GUI metadata")) goto cleanup;
    a.startup_impedance = imp && !strcmp(imp, "on");
    if (observe_state(&a) || set_impedance(&a, 0)) goto cleanup;
    DSI_Headset_SetSampleCallback(a.h, on_sample, &a);
    if (check_dsi("Register sample callback")) goto cleanup;
    marker(&a, "outlets_ready", lsl_local_clock());
    snapshot(&a, lsl_local_clock());
    fprintf(stderr, "Outlets advertised; subscribe/record in LabRecorder, then send start.\n");
    if (option(argc, argv, "auto-start", NULL) && start_acquisition(&a)) goto cleanup;
    while (keep_running && !a.failed) {
        if (poll_commands(&a) || observe_state(&a)) { a.failed = 1; break; }
        if (!keep_running) break;
        DSI_Headset_Idle(a.h, 0.01);
        if (check_dsi("Receive samples")) { a.failed = 1; break; }
        if (!a.acquiring && lsl_local_clock() >= a.next_snapshot) {
            snapshot(&a, lsl_local_clock());
            a.next_snapshot = lsl_local_clock() + 1.0;
        }
    }
    result = a.failed ? 1 : 0;
cleanup:
    /* Keep outlets alive until serial shutdown is complete. Never skip Delete on errors. */
    if (a.h) {
        a.acquiring = 0;
        DSI_Headset_SetSampleCallback(a.h, NULL, NULL);
        if (check_dsi("Remove callback")) result = 1;
        if (connected && set_impedance(&a, 0)) result = 1;
        if (a.acquisition_attempted) {
            DSI_Headset_StopDataAcquisition(a.h);
            if (check_dsi("Stop acquisition")) result = 1;
            DSI_Headset_Idle(a.h, 0.1);
            if (check_dsi("Drain serial data")) result = 1;
            marker(&a, "acquisition_stopped", lsl_local_clock());
        }
        marker(&a, "backend_disconnected", lsl_local_clock());
        DSI_Headset_Delete(a.h);
        if (check_dsi("Delete headset")) result = 1;
    }
    if (a.eeg) lsl_destroy_outlet(a.eeg);
    if (a.impedance) lsl_destroy_outlet(a.impedance);
    if (a.markers) lsl_destroy_outlet(a.markers);
    free(a.channels); free(a.sources); free(a.eeg_sample); free(a.impedance_sample);
    return result;
}
