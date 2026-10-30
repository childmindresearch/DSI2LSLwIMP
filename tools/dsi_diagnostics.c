/* Standalone DSI SDK timing/numeric diagnostic capture (explicit opt-in; not used by dsi2lsl/GUI).
 * Records per-callback raw SDK values in memory; files are written only after acquisition stops.
 * Never enables impedance, never issues analog reset, never creates LSL streams.
 */
#include "DSI.h"
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
#include <fcntl.h>
#ifdef _WIN32
#include <io.h>
#include <sys/stat.h>
#define DIAG_OPEN_EXCL(p) _open((p), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE)
#define DIAG_FDOPEN _fdopen
#define DIAG_CLOSE _close
#else
#include <unistd.h>
#define DIAG_OPEN_EXCL(p) open((p), O_WRONLY | O_CREAT | O_EXCL, 0644)
#define DIAG_FDOPEN fdopen
#define DIAG_CLOSE close
#endif

#define DIAG_MAX_DURATION 3600.0
#define DIAG_MAX_ROWS 10000000ULL
#define DIAG_MAX_BYTES (1024ULL * 1024ULL * 1024ULL)
#define DIAG_MAX_PREFIX 4000
#define DIAG_BOOL(r) ((((unsigned int)(r)) & 0xFFu) != 0)

typedef struct {
    const char *port, *output, *montage, *reference, *library;
    double duration;
    unsigned long long max_rows; /* 0 = derive from sampling rate and duration */
    int verbosity;
} DiagOptions;

/* One row: index, packet_time, receipt, read_complete, then channel_count values. */
typedef struct {
    DSI_Headset h;
    DSI_Channel *channels;
    int channel_count;
    double *rows;
    unsigned long long capacity, used, overflow_callbacks;
    int capturing, error;
    char error_text[256];
} DiagCapture;

static volatile sig_atomic_t diag_keep_running = 1;
static void diag_quit(int sig) { (void)sig; diag_keep_running = 0; }

static const char *diag_option(int argc, const char *argv[], const char *name, int *seen)
{
    const char *result = NULL;
    size_t n = strlen(name);
    int i;
    for (i = 1; i < argc; ++i)
        if (!strncmp(argv[i], "--", 2) && !strncmp(argv[i] + 2, name, n) &&
            (argv[i][2 + n] == '=' || argv[i][2 + n] == '\0')) {
            result = argv[i][2 + n] ? argv[i] + 3 + n : "";
            seen[i] = 1;
        }
    return result;
}

static void diag_help(const char *program)
{
    fprintf(stderr,
        "Usage: %s --port=PORT --output=PREFIX --duration=SECONDS [options]\n"
        "Explicit opt-in DSI SDK timing/numeric diagnostic capture. Connects to the headset ONLY\n"
        "when run with --port/--output/--duration; --help never loads libraries or connects.\n"
        "  --help                    Show help and exit\n"
        "  --port=PORT               Serial port (required)\n"
        "  --output=PREFIX           Owner-chosen path prefix (required); creates PREFIX.csv and\n"
        "                            PREFIX.meta.json; fails if either exists (never overwrites)\n"
        "  --duration=SECONDS        Capture length, 0 < SECONDS <= 3600 (required)\n"
        "  --montage=SPEC            DSI_Headset_ChooseChannels montage (default: vendor selection)\n"
        "  --reference=SPEC          DSI_Headset_ChooseChannels reference (default: vendor selection)\n"
        "  --max-rows=N              In-memory row capacity (default: ceil(rate*duration*1.5)+ceil(rate))\n"
        "  --verbosity=N             DSI verbosity (default: 2)\n"
        "  --dsi-library=PATH        Override bundled DSI shared library\n"
        "CSV columns: sample_index, callback_packet_time (untouched SDK callback argument),\n"
        "  host_receipt_lsl_local_clock, host_read_complete_lsl_local_clock, then one\n"
        "  DSI_Channel_GetSignal value per configured channel in montage order.\n"
        "Rows are buffered in memory; files are written after acquisition stops. Capacity\n"
        "exhaustion, SDK errors or interruption produce status != complete and exit code 1.\n"
        "Does not enable impedance, issue analog reset, or create LSL streams.\n", program);
}

static int diag_parse(int argc, const char *argv[], DiagOptions *o)
{
    int seen[256] = {0}, i;
    const char *duration, *rows, *verbosity;
    char *end;
    if (argc > 256) { fprintf(stderr, "Too many arguments\n"); return -1; }
    memset(o, 0, sizeof(*o));
    o->verbosity = 2;
    o->port = diag_option(argc, argv, "port", seen);
    o->output = diag_option(argc, argv, "output", seen);
    duration = diag_option(argc, argv, "duration", seen);
    o->montage = diag_option(argc, argv, "montage", seen);
    o->reference = diag_option(argc, argv, "reference", seen);
    rows = diag_option(argc, argv, "max-rows", seen);
    verbosity = diag_option(argc, argv, "verbosity", seen);
    o->library = diag_option(argc, argv, "dsi-library", seen);
    for (i = 1; i < argc; ++i)
        if (!seen[i]) { fprintf(stderr, "Unknown argument: %s\n", argv[i]); return -1; }
    if (!o->port || !*o->port) { fprintf(stderr, "--port is required\n"); return -1; }
    if (!o->output || !*o->output) { fprintf(stderr, "--output is required\n"); return -1; }
    if (strlen(o->output) > DIAG_MAX_PREFIX) { fprintf(stderr, "--output is too long\n"); return -1; }
    if (!duration) { fprintf(stderr, "--duration is required\n"); return -1; }
    errno = 0; o->duration = strtod(duration, &end);
    if (end == duration || *end || errno || !isfinite(o->duration) ||
        o->duration <= 0 || o->duration > DIAG_MAX_DURATION) {
        fprintf(stderr, "--duration must be > 0 and <= %.0f seconds\n", DIAG_MAX_DURATION); return -1;
    }
    if (rows) {
        errno = 0; o->max_rows = strtoull(rows, &end, 10);
        if (end == rows || *end || errno || *rows == '-' || o->max_rows == 0 || o->max_rows > DIAG_MAX_ROWS) {
            fprintf(stderr, "--max-rows must be 1..%llu\n", DIAG_MAX_ROWS); return -1;
        }
    }
    if (verbosity) {
        long level;
        errno = 0; level = strtol(verbosity, &end, 10);
        if (end == verbosity || *end || errno || level < 0 || level > INT_MAX) {
            fprintf(stderr, "Invalid --verbosity\n"); return -1;
        }
        o->verbosity = (int)level;
    }
    return 0;
}

/* Exclusive creation: never truncates or replaces an existing capture or metadata file. */
static FILE *diag_create_exclusive(const char *path)
{
    FILE *f;
    int fd = DIAG_OPEN_EXCL(path);
    if (fd < 0) { fprintf(stderr, "Cannot create %s: %s\n", path, strerror(errno)); return NULL; }
    f = DIAG_FDOPEN(fd, "wb");
    if (!f) DIAG_CLOSE(fd);
    return f;
}

static void diag_fail(DiagCapture *c, const char *what, const char *detail)
{
    if (c->error) return; /* Keep the first cause. */
    c->error = 1;
    snprintf(c->error_text, sizeof(c->error_text), "%s%s%s", what, detail ? ": " : "", detail ? detail : "");
}

static int diag_check(DiagCapture *c, const char *operation)
{
    if (!DSI_Error()) return 0;
    diag_fail(c, operation, DSI_ClearError());
    return 1;
}

/* Callback: record untouched packet time, host receipt, read-completion, calibrated GetSignal values. */
static void diag_on_sample(DSI_Headset h, double packet_time, void *userdata)
{
    DiagCapture *c = (DiagCapture *)userdata;
    double receipt = lsl_local_clock(), *row;
    size_t width = 4 + (size_t)c->channel_count;
    int i;
    (void)h;
    if (!c->capturing) return;
    if (c->used >= c->capacity) { ++c->overflow_callbacks; c->capturing = 0; diag_fail(c, "Row capacity exhausted", NULL); return; }
    row = c->rows + c->used * width;
    row[0] = (double)c->used;
    row[1] = packet_time;
    row[2] = receipt;
    for (i = 0; i < c->channel_count; ++i) row[4 + i] = DSI_Channel_GetSignal(c->channels[i]);
    row[3] = lsl_local_clock();
    if (DSI_Error()) { c->capturing = 0; diag_check(c, "Read channel signal"); return; }
    ++c->used;
}

static int diag_message(const char *msg, int level) { fprintf(stderr, "DSI(%d): %s\n", level, msg); return 1; }

static void diag_json_string(FILE *f, const char *s)
{
    if (!s) { fputs("null", f); return; }
    fputc('"', f);
    for (; *s; ++s) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') fprintf(f, "\\%c", ch);
        else if (ch < 0x20) fprintf(f, "\\u%04x", ch);
        else fputc(ch, f);
    }
    fputc('"', f);
}

static void diag_kv(FILE *f, const char *key, const char *value, int last)
{
    fprintf(f, "  \"%s\": ", key);
    diag_json_string(f, value);
    fputs(last ? "\n" : ",\n", f);
}

/* Provenance strings are copied before the headset is deleted. */
typedef struct {
    char api_runtime[64], model[128], firmware[128], reference[256], montage[2048], info[2048];
    char **names, **expressions;
    unsigned int serial;
    double rate;
    int filter_mode, have_headset;
} DiagProvenance;

static void diag_copy(char *dst, size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }
static char *diag_dup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *d = (char *)malloc(n);
    if (d) memcpy(d, s ? s : "", n);
    return d;
}

/* Test seam: CSV close is injectable so persistence failures can be simulated without hardware. */
static int (*diag_csv_close)(FILE *) = fclose;

/* Writes, flushes and closes the CSV; returns 0 only if every byte reached the stream successfully. */
static int diag_write_csv(FILE *csv, const DiagCapture *c, const DiagProvenance *p)
{
    size_t width = 4 + (size_t)c->channel_count;
    unsigned long long r;
    int i, failed;
    fputs("sample_index,callback_packet_time,host_receipt_lsl_local_clock,host_read_complete_lsl_local_clock", csv);
    for (i = 0; i < c->channel_count; ++i) {
        const char *name = p->names && p->names[i] ? p->names[i] : "";
        fputs(",\"", csv);
        for (; *name; ++name) { if (*name == '"') fputc('"', csv); fputc(*name, csv); }
        fputc('"', csv);
    }
    fputc('\n', csv);
    for (r = 0; r < c->used; ++r) {
        const double *row = c->rows + r * width;
        size_t k;
        fprintf(csv, "%llu", (unsigned long long)row[0]);
        for (k = 1; k < width; ++k) fprintf(csv, ",%.17g", row[k]);
        fputc('\n', csv);
    }
    failed = fflush(csv) != 0 || ferror(csv);
    if (diag_csv_close(csv)) failed = 1;
    return failed ? -1 : 0;
}

static int diag_write_meta(FILE *meta, const DiagOptions *o, const DiagCapture *c,
                           const DiagProvenance *p, const char *status, double started, double stopped)
{
    int i;
    fputs("{\n", meta);
    diag_kv(meta, "format", "dsi_diagnostics/1", 0);
    diag_kv(meta, "status", status, 0);
    diag_kv(meta, "error", c->error ? c->error_text : NULL, 0);
    fprintf(meta, "  \"rows_captured\": %llu,\n  \"row_capacity\": %llu,\n  \"callbacks_after_capacity\": %llu,\n",
            c->used, c->capacity, c->overflow_callbacks);
    fprintf(meta, "  \"requested_duration_seconds\": %.17g,\n", o->duration);
    fprintf(meta, "  \"acquisition_start_lsl_local_clock\": %.17g,\n  \"acquisition_stop_lsl_local_clock\": %.17g,\n",
            started, stopped);
    diag_kv(meta, "port", o->port, 0);
    diag_kv(meta, "requested_montage", o->montage, 0);
    diag_kv(meta, "requested_reference", o->reference, 0);
    diag_kv(meta, "dsi_api_runtime_version", p->api_runtime, 0);
    diag_kv(meta, "dsi_api_header_version", DSI_API_VERSION, 0);
    fprintf(meta, "  \"liblsl_library_version\": %d,\n  \"liblsl_protocol_version\": %d,\n",
            lsl_library_version(), lsl_protocol_version());
    diag_kv(meta, "liblsl_library_info", lsl_library_info(), 0);
    diag_kv(meta, "version_note", "Versions reported by the loaded libraries at capture time; not a guarantee of identity with any previously installed or historical binary.", 0);
    if (p->have_headset) {
        diag_kv(meta, "headset_model", p->model, 0);
        fprintf(meta, "  \"headset_serial\": %u,\n", p->serial);
        diag_kv(meta, "firmware_revision", p->firmware, 0);
        diag_kv(meta, "headset_info", p->info, 0);
        fprintf(meta, "  \"sampling_rate_hz\": %.17g,\n  \"filter_mode\": %d,\n", p->rate, p->filter_mode);
        diag_kv(meta, "reference", p->reference, 0);
        diag_kv(meta, "montage", p->montage, 0);
    }
    fputs("  \"columns\": [\"sample_index\", \"callback_packet_time\", \"host_receipt_lsl_local_clock\", \"host_read_complete_lsl_local_clock\"", meta);
    for (i = 0; i < c->channel_count; ++i) { fputs(", ", meta); diag_json_string(meta, p->names ? p->names[i] : ""); }
    fputs("],\n  \"channel_expressions\": [", meta);
    for (i = 0; i < c->channel_count; ++i) {
        if (i) fputs(", ", meta);
        diag_json_string(meta, p->expressions ? p->expressions[i] : "");
    }
    fputs("],\n", meta);
    diag_kv(meta, "timestamp_semantics", "callback_packet_time is the unmodified DSI_SampleCallback argument (semantics not interpreted); host_* columns are lsl_local_clock at callback entry and after all GetSignal reads.", 0);
    diag_kv(meta, "value_semantics", "Unmodified double from DSI_Channel_GetSignal for each configured channel.", 1);
    fputs("}\n", meta);
    return fflush(meta) != 0 || ferror(meta) ? -1 : 0;
}

/* Runs after the DSI library is loaded. Hardware is touched only here. */
static int diag_run(const DiagOptions *o)
{
    DiagCapture c;
    DiagProvenance p;
    FILE *csv = NULL, *meta = NULL;
    char csv_path[DIAG_MAX_PREFIX + 16], meta_path[DIAG_MAX_PREFIX + 16];
    const char *status = "complete";
    double started = 0, stopped = 0, deadline;
    int result = 1, connected = 0, acquisition_attempted = 0, i;
    memset(&c, 0, sizeof(c));
    memset(&p, 0, sizeof(p));
    snprintf(csv_path, sizeof(csv_path), "%s.csv", o->output);
    snprintf(meta_path, sizeof(meta_path), "%s.meta.json", o->output);
    /* Reserve both outputs before connecting so an existing capture fails fast, untouched. */
    csv = diag_create_exclusive(csv_path);
    if (!csv) return 1;
    meta = diag_create_exclusive(meta_path);
    if (!meta) { fclose(csv); remove(csv_path); return 1; }
    diag_copy(p.api_runtime, sizeof(p.api_runtime), DSI_GetAPIVersion());

    c.h = DSI_Headset_New(NULL);
    if (diag_check(&c, "Create headset") || !c.h) { diag_fail(&c, "Create headset", NULL); goto cleanup; }
    DSI_Headset_SetMessageCallback(c.h, diag_message);
    DSI_Headset_SetVerbosity(c.h, o->verbosity);
    if (diag_check(&c, "Configure logging")) goto cleanup;
    DSI_Headset_Connect(c.h, o->port);
    if (diag_check(&c, "Connect")) goto cleanup;
    connected = 1;
    DSI_Headset_StopDataAcquisition(c.h); /* Ensure no stale stream during setup. */
    if (diag_check(&c, "Ensure acquisition is stopped")) goto cleanup;
    DSI_Headset_ChooseChannels(c.h, o->montage, o->reference, 1);
    if (diag_check(&c, "Configure montage")) goto cleanup;

    p.have_headset = 1;
    diag_copy(p.model, sizeof(p.model), DSI_Headset_GetHardwareModel(c.h));
    diag_copy(p.firmware, sizeof(p.firmware), DSI_Headset_GetFirmwareRevision(c.h));
    diag_copy(p.reference, sizeof(p.reference), DSI_Headset_GetReferenceString(c.h));
    diag_copy(p.montage, sizeof(p.montage), DSI_Headset_GetMontageString(c.h));
    diag_copy(p.info, sizeof(p.info), DSI_Headset_GetInfoString(c.h));
    p.serial = DSI_Headset_GetSerialNumber(c.h);
    p.rate = DSI_Headset_GetSamplingRate(c.h);
    p.filter_mode = DSI_Headset_GetFilterMode(c.h);
    c.channel_count = DSI_Headset_GetNumberOfChannels(c.h);
    if (diag_check(&c, "Read headset provenance")) goto cleanup;
    if (c.channel_count <= 0 || c.channel_count > 4096 || !(p.rate > 0) || !isfinite(p.rate)) {
        diag_fail(&c, "Invalid channel count or sampling rate", NULL); goto cleanup;
    }
    c.channels = (DSI_Channel *)calloc((size_t)c.channel_count, sizeof(*c.channels));
    p.names = (char **)calloc((size_t)c.channel_count, sizeof(char *));
    p.expressions = (char **)calloc((size_t)c.channel_count, sizeof(char *));
    if (!c.channels || !p.names || !p.expressions) { diag_fail(&c, "Out of memory", NULL); goto cleanup; }
    for (i = 0; i < c.channel_count; ++i) {
        c.channels[i] = DSI_Headset_GetChannelByIndex(c.h, (unsigned int)i);
        if (diag_check(&c, "Get channel") || !c.channels[i]) { diag_fail(&c, "Get channel", NULL); goto cleanup; }
        p.names[i] = diag_dup(DSI_Channel_GetName(c.channels[i]));
        p.expressions[i] = diag_dup(DSI_Channel_GetString(c.channels[i]));
        if (!p.names[i] || !p.expressions[i]) { diag_fail(&c, "Out of memory", NULL); goto cleanup; }
    }
    if (diag_check(&c, "Read channel metadata")) goto cleanup;

    c.capacity = o->max_rows ? o->max_rows
                             : (unsigned long long)ceil(p.rate * o->duration * 1.5) + (unsigned long long)ceil(p.rate);
    if (c.capacity > DIAG_MAX_ROWS ||
        c.capacity * (4ULL + (unsigned long long)c.channel_count) > DIAG_MAX_BYTES / sizeof(double)) {
        diag_fail(&c, "Requested buffer exceeds limit; lower --duration or --max-rows", NULL); goto cleanup;
    }
    c.rows = (double *)malloc((size_t)c.capacity * (4 + (size_t)c.channel_count) * sizeof(double));
    if (!c.rows) { diag_fail(&c, "Out of memory for row buffer", NULL); goto cleanup; }

    DSI_Headset_SetSampleCallback(c.h, diag_on_sample, &c);
    if (diag_check(&c, "Register sample callback")) goto cleanup;
    c.capturing = 1;
    acquisition_attempted = 1;
    started = lsl_local_clock();
    DSI_Headset_StartDataAcquisition(c.h);
    if (diag_check(&c, "Start acquisition")) goto cleanup;
    deadline = started + o->duration;
    while (diag_keep_running && c.capturing && lsl_local_clock() < deadline) {
        DSI_Headset_Idle(c.h, 0.01);
        if (diag_check(&c, "Receive samples")) break;
    }
    if (!diag_keep_running && !c.error) status = "interrupted";
cleanup:
    /* Quiesce callbacks before stopping/deleting the headset or touching the buffer. */
    c.capturing = 0;
    if (c.h) {
        DSI_Headset_SetSampleCallback(c.h, NULL, NULL);
        diag_check(&c, "Remove callback");
        if (connected && acquisition_attempted) {
            DSI_Headset_StopDataAcquisition(c.h);
            diag_check(&c, "Stop acquisition");
            DSI_Headset_Idle(c.h, 0.1);
            diag_check(&c, "Drain serial data");
        }
        stopped = lsl_local_clock();
        DSI_Headset_Delete(c.h);
        diag_check(&c, "Delete headset");
        c.h = NULL;
    }
    /* CSV must be fully persisted before the sidecar records the final status. */
    if (diag_write_csv(csv, &c, &p)) diag_fail(&c, "CSV write/flush/close failed", csv_path);
    if (c.error) status = "incomplete";
    if (diag_write_meta(meta, o, &c, &p, status, started, stopped)) {
        fprintf(stderr, "Error writing %s\n", meta_path); status = "incomplete";
    }
    if (fclose(meta)) status = "incomplete";
    if (c.error) fprintf(stderr, "Capture %s: %s\n", status, c.error_text);
    fprintf(stderr, "Wrote %llu rows (%s) to %s and %s\n", c.used, status, csv_path, meta_path);
    result = strcmp(status, "complete") ? 1 : 0;
    for (i = 0; p.names && i < c.channel_count; ++i) free(p.names[i]);
    for (i = 0; p.expressions && i < c.channel_count; ++i) free(p.expressions[i]);
    free(p.names); free(p.expressions); free(c.channels); free(c.rows);
    return result;
}

int main(int argc, const char *argv[])
{
    DiagOptions o;
    const char *library;
    int load, i;
    for (i = 1; i < argc; ++i)
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { diag_help(argv[0]); return 0; }
    if (diag_parse(argc, argv, &o)) { diag_help(argv[0]); return 2; }
    library = o.library;
#ifdef DSI_BUNDLED_LIBRARY
    if (!library) library = DSI_BUNDLED_LIBRARY;
#endif
    load = Load_DSI_API(library);
    if (load) {
        fprintf(stderr, "Cannot load DSI library %s (loader result %d)\n", DSI_DYLIB_NAME(library), load);
        return 1;
    }
    signal(SIGINT, diag_quit);
    signal(SIGTERM, diag_quit);
    return diag_run(&o);
}
