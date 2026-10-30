/* Hardware-free tests for tools/dsi_diagnostics.c: substituted DSI function pointers, scratch outputs. */
#define main dsi_diagnostics_program_main
#include "../tools/dsi_diagnostics.c"
#undef main

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static char headset_storage;
static short channel_storage[2];
static const char *fake_error = NULL;
static DSI_SampleCallback registered = NULL;
static void *registered_data = NULL;
static int samples_per_idle = 1, start_fails = 0, idle_fail_after = -1, idle_calls = 0;
static int acq_started = 0, acq_stops = 0, deleted = 0, callback_at_delete = 1, forbidden_calls = 0;
static unsigned long long emitted = 0, emit_limit = 10; /* Fake Idle returns instantly; bound samples. */
static const double packet_times[] = { 1234.0000000001, 1234.0033333333333, -0.5, 1e-300 };

static const char *get_error(void) { return fake_error; }
static const char *clear_error(void) { const char *e = fake_error; fake_error = NULL; return e; }
static const char *api_version(void) { return "fake-1.2.3"; }
static DSI_Headset new_headset(const char *p) { (void)p; return &headset_storage; }
static void set_msg(DSI_Headset h, DSI_MessageCallback f) { (void)h; (void)f; }
static void set_verb(DSI_Headset h, int l) { (void)h; (void)l; }
static void do_connect(DSI_Headset h, const char *p) { (void)h; REQUIRE(p && !strcmp(p, "FAKEPORT")); }
static void choose(DSI_Headset h, const char *m, const char *r, int a) { (void)h; (void)m; (void)r; (void)a; }
static const char *model(DSI_Headset h) { (void)h; return "mock \"DSI\""; }
static const char *firmware(DSI_Headset h) { (void)h; return "fw-9"; }
static const char *reference(DSI_Headset h) { (void)h; return "Pz"; }
static const char *montage(DSI_Headset h) { (void)h; return "F3-Pz,TRG"; }
static const char *info(DSI_Headset h) { (void)h; return "info"; }
static unsigned int serial(DSI_Headset h) { (void)h; return 4321; }
static double rate(DSI_Headset h) { (void)h; return 300; }
static int filter(DSI_Headset h) { (void)h; return 0; }
static int channel_count = 2;
static int nchan(DSI_Headset h) { (void)h; return channel_count; }
static DSI_Channel channel(DSI_Headset h, unsigned int i) { (void)h; return &channel_storage[i % 2]; }
static const char *cname(DSI_Channel c) { return c == channel_storage ? "F3" : "TRG"; }
static const char *cexpr(DSI_Channel c) { return c == channel_storage ? "F3-Pz" : "TRG"; }
/* Values depend on emitted index and channel, with non-round doubles to prove no float narrowing. */
static double signal_value(DSI_Channel c) { return (c == channel_storage ? 0.1 : 1e10 + 0.5) + (double)emitted; }
static void set_cb(DSI_Headset h, DSI_SampleCallback f, void *d) { (void)h; registered = f; registered_data = d; }
static void start_acq(DSI_Headset h) { (void)h; ++acq_started; if (start_fails) fake_error = "start rejected"; }
static void stop_acq(DSI_Headset h) { (void)h; ++acq_stops; }
static void idle(DSI_Headset h, double s)
{
    int i;
    (void)s;
    if (idle_fail_after >= 0 && idle_calls++ >= idle_fail_after) { fake_error = "serial lost"; return; }
    for (i = 0; i < samples_per_idle && registered && acq_started && emitted < emit_limit; ++i) {
        registered(h, packet_times[emitted % 4], registered_data);
        ++emitted;
    }
}
static void del(DSI_Headset h) { (void)h; ++deleted; callback_at_delete = registered != NULL; }
static void forbidden(DSI_Headset h) { (void)h; ++forbidden_calls; }

static void install_fakes(void)
{
    DSI_Error = get_error; DSI_ClearError = clear_error; DSI_GetAPIVersion = api_version;
    DSI_Headset_New = new_headset; DSI_Headset_SetMessageCallback = set_msg; DSI_Headset_SetVerbosity = set_verb;
    DSI_Headset_Connect = do_connect; DSI_Headset_ChooseChannels = choose;
    DSI_Headset_GetHardwareModel = model; DSI_Headset_GetFirmwareRevision = firmware;
    DSI_Headset_GetReferenceString = reference; DSI_Headset_GetMontageString = montage;
    DSI_Headset_GetInfoString = info; DSI_Headset_GetSerialNumber = serial; DSI_Headset_GetSamplingRate = rate;
    DSI_Headset_GetFilterMode = filter; DSI_Headset_GetNumberOfChannels = nchan;
    DSI_Headset_GetChannelByIndex = channel; DSI_Channel_GetName = cname; DSI_Channel_GetString = cexpr;
    DSI_Channel_GetSignal = signal_value; DSI_Headset_SetSampleCallback = set_cb;
    DSI_Headset_StartDataAcquisition = start_acq; DSI_Headset_StopDataAcquisition = stop_acq;
    DSI_Headset_Idle = idle; DSI_Headset_Delete = del;
    /* The tool must never touch impedance/reset commands. */
    DSI_Headset_StartImpedanceDriver = forbidden; DSI_Headset_StopImpedanceDriver = forbidden;
    DSI_Headset_StartAnalogReset = forbidden; DSI_Headset_ReleaseAnalogReset = forbidden;
    DSI_Headset_LockAnalogReset = forbidden;
}

static void reset_fakes(void)
{
    fake_error = NULL; registered = NULL; registered_data = NULL;
    samples_per_idle = 1; start_fails = 0; idle_fail_after = -1; idle_calls = 0;
    acq_started = acq_stops = deleted = 0; callback_at_delete = 1; emitted = 0; channel_count = 2;
}

static char scratch[1024];
static void path(char *out, size_t n, const char *name, const char *ext) { snprintf(out, n, "%s/%s%s", scratch, name, ext); }

static char *slurp(const char *p)
{
    FILE *f = fopen(p, "rb");
    long n;
    char *s;
    REQUIRE(f != NULL);
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    s = (char *)calloc((size_t)n + 1, 1);
    REQUIRE(fread(s, 1, (size_t)n, f) == (size_t)n);
    fclose(f);
    return s;
}

static int file_exists(const char *p) { FILE *f = fopen(p, "rb"); if (f) fclose(f); return f != NULL; }

static void cleanup_prefix(const char *name)
{
    char p[1200];
    path(p, sizeof(p), name, ".csv"); remove(p);
    path(p, sizeof(p), name, ".meta.json"); remove(p);
}

static int run(const char *name, const char *duration, const char *rows)
{
    char out[1100], dur[64], mr[64];
    const char *argv[6];
    int argc = 0;
    DiagOptions o;
    path(out, sizeof(out), name, "");
    snprintf(dur, sizeof(dur), "--duration=%s", duration);
    argv[argc++] = "dsi_diagnostics"; argv[argc++] = "--port=FAKEPORT";
    argv[argc++] = out; memmove(out + 9, out, strlen(out) + 1); memcpy(out, "--output=", 9);
    argv[argc++] = dur;
    if (rows) { snprintf(mr, sizeof(mr), "--max-rows=%s", rows); argv[argc++] = mr; }
    REQUIRE(diag_parse(argc, argv, &o) == 0);
    return diag_run(&o);
}

static void test_invalid_args(void)
{
    DiagOptions o;
    const char *none[] = { "x" };
    const char *no_port[] = { "x", "--output=a", "--duration=1" };
    const char *no_out[] = { "x", "--port=P", "--duration=1" };
    const char *no_dur[] = { "x", "--port=P", "--output=a" };
    const char *bad_dur[] = { "x", "--port=P", "--output=a", "--duration=abc" };
    const char *zero_dur[] = { "x", "--port=P", "--output=a", "--duration=0" };
    const char *long_dur[] = { "x", "--port=P", "--output=a", "--duration=3601" };
    const char *nan_dur[] = { "x", "--port=P", "--output=a", "--duration=nan" };
    const char *bad_rows[] = { "x", "--port=P", "--output=a", "--duration=1", "--max-rows=0" };
    const char *neg_rows[] = { "x", "--port=P", "--output=a", "--duration=1", "--max-rows=-5" };
    const char *big_rows[] = { "x", "--port=P", "--output=a", "--duration=1", "--max-rows=10000001" };
    const char *unknown[] = { "x", "--port=P", "--output=a", "--duration=1", "--impedance=on" };
    const char *bad_verb[] = { "x", "--port=P", "--output=a", "--duration=1", "--verbosity=-1" };
    const char *ok[] = { "x", "--port=P", "--output=a", "--duration=3600", "--max-rows=10000000", "--montage=F3", "--reference=Pz", "--verbosity=0" };
    REQUIRE(diag_parse(1, none, &o) != 0);
    REQUIRE(diag_parse(3, no_port, &o) != 0);
    REQUIRE(diag_parse(3, no_out, &o) != 0);
    REQUIRE(diag_parse(3, no_dur, &o) != 0);
    REQUIRE(diag_parse(4, bad_dur, &o) != 0);
    REQUIRE(diag_parse(4, zero_dur, &o) != 0);
    REQUIRE(diag_parse(4, long_dur, &o) != 0);
    REQUIRE(diag_parse(4, nan_dur, &o) != 0);
    REQUIRE(diag_parse(5, bad_rows, &o) != 0);
    REQUIRE(diag_parse(5, neg_rows, &o) != 0);
    REQUIRE(diag_parse(5, big_rows, &o) != 0);
    REQUIRE(diag_parse(5, unknown, &o) != 0);
    REQUIRE(diag_parse(5, bad_verb, &o) != 0);
    REQUIRE(diag_parse(8, ok, &o) == 0);
    REQUIRE(o.duration == 3600 && o.max_rows == 10000000ULL && !strcmp(o.montage, "F3") && o.verbosity == 0);
}

static void test_capture_preserves_values_and_order(void)
{
    char p[1200], *csv, *meta, expect[512], *line;
    unsigned long long r;
    reset_fakes(); cleanup_prefix("ok");
    REQUIRE(run("ok", "0.05", NULL) == 0);
    REQUIRE(deleted == 1 && !callback_at_delete && acq_stops >= 1 && forbidden_calls == 0);
    path(p, sizeof(p), "ok", ".csv"); csv = slurp(p);
    path(p, sizeof(p), "ok", ".meta.json"); meta = slurp(p);
    REQUIRE(!strncmp(csv, "sample_index,callback_packet_time,host_receipt_lsl_local_clock,"
                          "host_read_complete_lsl_local_clock,\"F3\",\"TRG\"\n", 102));
    REQUIRE(emitted == emit_limit);
    line = strchr(csv, '\n') + 1;
    for (r = 0; r < emitted; ++r) {
        unsigned long long idx;
        double pt, rec, done, v0, v1;
        REQUIRE(sscanf(line, "%llu,%lf,%lf,%lf,%lf,%lf", &idx, &pt, &rec, &done, &v0, &v1) == 6);
        REQUIRE(idx == r);
        REQUIRE(pt == packet_times[r % 4]);          /* Untouched callback argument, full precision. */
        REQUIRE(rec > 0 && done >= rec);
        REQUIRE(v0 == 0.1 + (double)r && v1 == 1e10 + 0.5 + (double)r); /* Column order = montage order. */
        line = strchr(line, '\n') + 1;
    }
    REQUIRE(*line == '\0');
    snprintf(expect, sizeof(expect), "\"rows_captured\": %llu,", emitted);
    REQUIRE(strstr(meta, expect));
    REQUIRE(strstr(meta, "\"status\": \"complete\""));
    REQUIRE(strstr(meta, "\"dsi_api_runtime_version\": \"fake-1.2.3\""));
    REQUIRE(strstr(meta, "\"headset_model\": \"mock \\\"DSI\\\"\""));
    REQUIRE(strstr(meta, "\"headset_serial\": 4321"));
    REQUIRE(strstr(meta, "\"firmware_revision\": \"fw-9\""));
    REQUIRE(strstr(meta, "\"sampling_rate_hz\": 300"));
    REQUIRE(strstr(meta, "\"filter_mode\": 0"));
    REQUIRE(strstr(meta, "\"channel_expressions\": [\"F3-Pz\", \"TRG\"]"));
    REQUIRE(strstr(meta, "\"liblsl_library_version\""));
    REQUIRE(strstr(meta, "not a guarantee of identity"));
    free(csv); free(meta);
}

static void test_never_overwrites(void)
{
    char p[1200], *csv;
    FILE *f;
    reset_fakes(); cleanup_prefix("exists");
    path(p, sizeof(p), "exists", ".meta.json");
    f = fopen(p, "wb"); REQUIRE(f); fputs("ORIGINAL", f); fclose(f);
    REQUIRE(run("exists", "0.05", NULL) != 0);
    REQUIRE(deleted == 0 && acq_started == 0);       /* Fails before connecting. */
    csv = slurp(p); REQUIRE(!strcmp(csv, "ORIGINAL")); free(csv);
    path(p, sizeof(p), "exists", ".csv");
    REQUIRE(!file_exists(p));                        /* Reserved CSV rolled back. */
    reset_fakes(); cleanup_prefix("exists");
    path(p, sizeof(p), "exists", ".csv");
    f = fopen(p, "wb"); REQUIRE(f); fputs("ORIGINAL", f); fclose(f);
    REQUIRE(run("exists", "0.05", NULL) != 0);
    csv = slurp(p); REQUIRE(!strcmp(csv, "ORIGINAL")); free(csv);
    path(p, sizeof(p), "exists", ".meta.json");
    REQUIRE(!file_exists(p));
}

static void test_capacity_exhaustion(void)
{
    char p[1200], *meta, *csv, *line;
    int rows = 0;
    reset_fakes(); cleanup_prefix("cap");
    samples_per_idle = 5;
    REQUIRE(run("cap", "1", "3") != 0);
    REQUIRE(deleted == 1 && !callback_at_delete);
    path(p, sizeof(p), "cap", ".meta.json"); meta = slurp(p);
    REQUIRE(strstr(meta, "\"status\": \"incomplete\""));
    REQUIRE(strstr(meta, "Row capacity exhausted"));
    REQUIRE(strstr(meta, "\"rows_captured\": 3,"));
    REQUIRE(strstr(meta, "\"callbacks_after_capacity\": 1,"));
    path(p, sizeof(p), "cap", ".csv"); csv = slurp(p);
    for (line = csv; (line = strchr(line, '\n')) != NULL; ++line) ++rows;
    REQUIRE(rows == 4);
    free(meta); free(csv);
}

static void test_acquisition_errors_cleanup(void)
{
    char p[1200], *meta;
    reset_fakes(); cleanup_prefix("startfail");
    start_fails = 1;
    REQUIRE(run("startfail", "1", NULL) != 0);
    REQUIRE(deleted == 1 && !callback_at_delete && acq_stops >= 2);
    path(p, sizeof(p), "startfail", ".meta.json"); meta = slurp(p);
    REQUIRE(strstr(meta, "\"status\": \"incomplete\"") && strstr(meta, "start rejected"));
    free(meta);

    reset_fakes(); cleanup_prefix("idlefail");
    idle_fail_after = 2;
    REQUIRE(run("idlefail", "5", NULL) != 0);
    REQUIRE(deleted == 1 && !callback_at_delete);
    path(p, sizeof(p), "idlefail", ".meta.json"); meta = slurp(p);
    REQUIRE(strstr(meta, "serial lost") && strstr(meta, "\"rows_captured\": 2,"));
    free(meta);

    reset_fakes(); cleanup_prefix("buffer");
    channel_count = 4000; /* 10M rows x 4004 doubles exceeds the 1 GiB limit: refuse before acquiring. */
    REQUIRE(run("buffer", "1", "10000000") != 0);
    REQUIRE(deleted == 1 && acq_started == 0);
    path(p, sizeof(p), "buffer", ".meta.json"); meta = slurp(p);
    REQUIRE(strstr(meta, "exceeds limit") && strstr(meta, "\"status\": \"incomplete\""));
    free(meta);
}

static int failing_close(FILE *f) { fclose(f); return EOF; }

static void test_csv_persistence_failure(void)
{
    char p[1200], *meta;
    reset_fakes(); cleanup_prefix("csvfail");
    diag_csv_close = failing_close;
    REQUIRE(run("csvfail", "0.05", NULL) != 0);
    diag_csv_close = fclose;
    path(p, sizeof(p), "csvfail", ".meta.json"); meta = slurp(p);
    REQUIRE(!strstr(meta, "\"status\": \"complete\""));
    REQUIRE(strstr(meta, "\"status\": \"incomplete\"") && strstr(meta, "CSV write/flush/close failed"));
    free(meta);
}

int main(int argc, char **argv)
{
    REQUIRE(argc == 2);
    snprintf(scratch, sizeof(scratch), "%s", argv[1]);
    install_fakes();
    test_invalid_args();
    test_capture_preserves_values_and_order();
    test_never_overwrites();
    test_capacity_exhaustion();
    test_acquisition_errors_cleanup();
    test_csv_persistence_failure();
    cleanup_prefix("ok"); cleanup_prefix("exists"); cleanup_prefix("cap");
    cleanup_prefix("startfail"); cleanup_prefix("idlefail"); cleanup_prefix("buffer"); cleanup_prefix("csvfail");
    puts("dsi_diagnostics tests passed");
    return 0;
}
