/* Hardware-free tests: real liblsl outlets/inlets, substituted DSI function pointers. */
#define main dsi2lsl_program_main
#include "../dsi2lsl.c"
#undef main

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int fake_driver = 0, fake_reset = 0, reset_calls = 0, release_calls = 0, release_works = 1;
static const char *fake_error = NULL;
static char headset_storage;
static short channel_storage[2];
static long source_storage[3];
static const char *no_error(void) { return fake_error; }
static const char *clear_error(void) { const char *e = fake_error; fake_error = NULL; return e; }
static int two_channels(DSI_Headset h) { (void)h; return 2; }
static int three_sources(DSI_Headset h) { (void)h; return 3; }
static double rate(DSI_Headset h) { (void)h; return 300; }
static unsigned int serial(DSI_Headset h) { (void)h; return 123; }
static const char *model(DSI_Headset h) { (void)h; return "mock-DSI"; }
static const char *reference(DSI_Headset h) { (void)h; return "Pz"; }
static const char *version(void) { return "test"; }
static DSI_Channel channel(DSI_Headset h, unsigned int i) { (void)h; return &channel_storage[i]; }
static DSI_Source source(DSI_Headset h, unsigned int i) { (void)h; return &source_storage[i]; }
static const char *channel_name(DSI_Channel c) { return c == channel_storage ? "F3" : "TRG"; }
static const char *channel_expression(DSI_Channel c) { return c == channel_storage ? "F3-Pz" : "TRG"; }
/* Real library returns garbage upper bytes in bool_t; only the low byte is meaningful. */
static int is_trigger(DSI_Channel c) { return c == &channel_storage[1] ? 0x12345601 : 0x12345600; }
static int is_eeg_channel(DSI_Channel c) { return c == channel_storage ? 0x7F000001 : 0x7F000000; }
static double signal_value(DSI_Channel c) { return c == channel_storage ? 42 : 7; }
static int is_eeg_source(DSI_Source s) { return s != &source_storage[2]; }
static int is_factory(DSI_Source s) { return s == &source_storage[1]; }
static const char *source_name(DSI_Source s) { (void)s; return "F3"; }
static double electrode_impedance(DSI_Source s) { (void)s; return 123000; }
static double cmf_impedance(DSI_Headset h) { (void)h; return 456000; }
static int driver_mode(DSI_Headset h) { (void)h; return fake_driver; }
static int reset_mode(DSI_Headset h) { (void)h; return fake_reset; }
static void driver_on(DSI_Headset h) { (void)h; fake_driver = 1; }
static void driver_off(DSI_Headset h) { (void)h; fake_driver = 0; }
static void reset(DSI_Headset h) { (void)h; ++reset_calls; fake_reset = 1; }
static void release(DSI_Headset h) { (void)h; ++release_calls; if (release_works) fake_reset = 0; }
static void lock_forbidden(DSI_Headset h) { (void)h; REQUIRE(!"LockAnalogReset must never be called"); }
static void fail_driver(DSI_Headset h) { (void)h; fake_error = "simulated command rejection"; }
static int adc_starts = 0, adc_stops = 0;
static void start_adc(DSI_Headset h) { (void)h; ++adc_starts; }
static void stop_adc(DSI_Headset h) { (void)h; ++adc_stops; }
static void idle_adc(DSI_Headset h, double duration) { (void)h; (void)duration; }

static lsl_inlet open_inlet(lsl_outlet outlet)
{
    int error = 0;
    lsl_streaminfo info = lsl_get_info(outlet);
    lsl_streaminfo resolved = NULL;
    lsl_inlet inlet;
    REQUIRE(lsl_resolve_byprop(&resolved, 1, "source_id", lsl_get_source_id(info), 1, 5) == 1);
    inlet = lsl_create_inlet(resolved, 360, 0, 1);
    lsl_destroy_streaminfo(resolved);
    lsl_destroy_streaminfo(info);
    REQUIRE(inlet != NULL);
    lsl_open_stream(inlet, 5.0, &error);
    REQUIRE(error == 0);
    REQUIRE(lsl_wait_for_consumers(outlet, 5.0));
    return inlet;
}

static void expect_marker(lsl_inlet inlet, const char *expected);

static const char *channel_child(lsl_outlet outlet, int index, const char *name, char *out, size_t size)
{
    lsl_streaminfo info = lsl_get_info(outlet);
    lsl_xml_ptr ch = lsl_child(lsl_child(lsl_get_desc(info), "channels"), "channel");
    while (index-- > 0) ch = lsl_next_sibling(ch);
    snprintf(out, size, "%s", lsl_child_value_n(ch, name));
    lsl_destroy_streaminfo(info);
    return out;
}

/* Next marker must be exactly `expected` among non-snapshot markers. */
static void expect_next_marker(lsl_inlet inlet, const char *expected)
{
    int error = 0;
    for (;;) {
        char *text = NULL;
        int skip, match;
        REQUIRE(lsl_pull_sample_str(inlet, &text, 1, 5, &error) > 0 && error == 0);
        skip = !strncmp(text, "state ", 6) || !strncmp(text, "impedance_state", 15) ||
               !strcmp(text, "sample_gap"); /* Test packet_times are arbitrary. */
        match = !strcmp(text, expected);
        if (!skip && !match) fprintf(stderr, "unexpected marker: %s (wanted %s)\n", text, expected);
        lsl_destroy_string(text);
        if (match) return;
        REQUIRE(skip);
    }
}

static void drain(lsl_inlet inlet)
{
    int error = 0;
    char *text = NULL;
    while (lsl_pull_sample_str(inlet, &text, 1, 0.2, &error) > 0) { lsl_destroy_string(text); text = NULL; }
}

static void test_timestamps(App *a, lsl_inlet marker_inlet)
{
    double t, prev = 0, pt = 1000.0, base = 50.0;
    int p, j, n = 0;
    const double fs = 300, jitter[] = {0.004, 0.0, 0.012, 0.001, 0.020, 0.003};
    a->ts_fs = fs;
    timestamps_reset(a);
    drain(marker_inlet);
    /* 9 samples share packet_time, all delivered with one jittered receipt time. */
    for (p = 0; p < 200; ++p, pt += 9 / fs) {
        double receipt = base + pt + 9 / fs + jitter[p % 6];
        for (j = 0; j < 9; ++j, ++n) {
            t = sample_time(a, pt, receipt);
            if (n) REQUIRE(fabs((t - prev) - 1 / fs) < 0.0001 && t > prev);
            prev = t;
        }
    }
    REQUIRE(!a->ts_fallback);
    /* Gap: 3 packets missing. */
    pt += 3 * 9 / fs;
    t = sample_time(a, pt, base + pt + 9 / fs);
    REQUIRE(t - prev > 3 * 9 / fs);
    expect_marker(marker_inlet, "sample_gap");
    /* Non-monotonic packet_time: receipt time from then on. */
    REQUIRE(sample_time(a, pt - 1, 77.0) == 77.0 && a->ts_fallback);
    REQUIRE(sample_time(a, pt + 1, 78.0) == 78.0);
    expect_marker(marker_inlet, "timestamp_fallback");
    timestamps_reset(a);
    REQUIRE(!a->ts_fallback && a->ts_k == 0);
}

static void expect_marker(lsl_inlet inlet, const char *expected)
{
    int i, error = 0;
    for (i = 0; i < 50; ++i) {
        char *text = NULL;
        int match;
        REQUIRE(lsl_pull_sample_str(inlet, &text, 1, 5, &error) > 0 && error == 0);
        match = !strcmp(text, expected);
        lsl_destroy_string(text);
        if (match) return;
    }
    REQUIRE(0);
}

int main(void)
{
    App a = {0};
    lsl_inlet eeg_inlet, imp_inlet, marker_inlet;
    lsl_streaminfo info;
    double values[2], timestamp;
    int error = 0;
    char stream_name[64];
    const char *args[] = {"dsi2lsl", "-m=F3", "--impedance=on", "/port:COM4"};
    REQUIRE(!strcmp(option(4, args, "montage", "m"), "F3"));
    REQUIRE(option(4, args, "lsl-stream-name", NULL) == NULL);
    REQUIRE(!strcmp(option(4, args, "port", "p"), "COM4"));
    DSI_Error = no_error; DSI_ClearError = clear_error;
    DSI_GetAPIVersion = version;
    DSI_Headset_GetNumberOfChannels = two_channels;
    DSI_Headset_GetNumberOfSources = three_sources;
    DSI_Headset_GetSamplingRate = rate;
    DSI_Headset_GetSerialNumber = serial;
    DSI_Headset_GetHardwareModel = model;
    DSI_Headset_GetFirmwareRevision = model;
    DSI_Headset_GetReferenceString = reference;
    DSI_Headset_GetFactoryReferenceString = reference;
    DSI_Headset_GetChannelByIndex = channel;
    DSI_Headset_GetSourceByIndex = source;
    DSI_Channel_GetName = channel_name;
    DSI_Channel_GetString = channel_expression;
    DSI_Channel_IsTrigger = is_trigger;
    DSI_Channel_IsReferentialEEG = is_eeg_channel;
    DSI_Channel_GetSignal = signal_value;
    DSI_Source_IsReferentialEEG = is_eeg_source;
    DSI_Source_IsFactoryReference = is_factory;
    DSI_Source_GetName = source_name;
    DSI_Source_GetImpedanceEEG = electrode_impedance;
    DSI_Headset_GetImpedanceCMF = cmf_impedance;
    DSI_Headset_GetImpedanceDriverMode = driver_mode;
    DSI_Headset_GetAnalogResetMode = reset_mode;
    DSI_Headset_StartImpedanceDriver = driver_on;
    DSI_Headset_StopImpedanceDriver = driver_off;
    DSI_Headset_StartAnalogReset = reset;
    DSI_Headset_ReleaseAnalogReset = release;
    DSI_Headset_LockAnalogReset = lock_forbidden;
    DSI_Headset_StartDataAcquisition = start_adc;
    DSI_Headset_StopDataAcquisition = stop_adc;
    DSI_Headset_Idle = idle_adc;
    a.h = &headset_storage;
    a.driver_mode = a.reset_mode = -999;
    a.settle_seconds = 2;
    a.reset_hold_seconds = 1;
#ifdef _WIN32
    snprintf(stream_name, sizeof(stream_name), "DSI-test-%lu", (unsigned long)GetCurrentProcessId());
#else
    snprintf(stream_name, sizeof(stream_name), "DSI-test-%lu", (unsigned long)getpid());
#endif
    REQUIRE(init_streams(&a, stream_name) == 0);
    a.gui_telemetry = 1;
    gui_config(&a, stream_name);
    REQUIRE(a.source_count == 1); /* Exclude factory reference and trigger; append CMF separately. */
    info = lsl_get_info(a.impedance);
    REQUIRE(lsl_get_channel_count(info) == 2);
    REQUIRE(lsl_get_nominal_srate(info) == 0);
    REQUIRE(!strcmp(lsl_child_value_n(lsl_child(lsl_child(lsl_get_desc(info), "channels"), "channel"), "unit"), "ohms"));
    REQUIRE(strstr(lsl_child_value_n(lsl_get_desc(info), "cmf_note"), "unitless") != NULL);
    lsl_destroy_streaminfo(info);
    {
        char v[64];
        REQUIRE(!strcmp(channel_child(a.eeg, 0, "type", v, sizeof v), "EEG"));
        REQUIRE(!strcmp(channel_child(a.eeg, 0, "unit", v, sizeof v), "microvolts"));
        REQUIRE(!strcmp(channel_child(a.eeg, 1, "type", v, sizeof v), "Trigger"));
        REQUIRE(!strcmp(channel_child(a.eeg, 1, "unit", v, sizeof v), "integer"));
        REQUIRE(!strcmp(channel_child(a.impedance, 0, "type", v, sizeof v), "Impedance"));
        REQUIRE(!strcmp(channel_child(a.impedance, 1, "type", v, sizeof v), "CommonModeFault"));
        REQUIRE(!strcmp(channel_child(a.impedance, 1, "unit", v, sizeof v), "")); /* No unit element. */
        info = lsl_get_info(a.eeg);
        REQUIRE(!strcmp(lsl_child_value_n(lsl_get_desc(info), "timestamp_method"), "nominal_rate_anchored_to_packet_time"));
        lsl_destroy_streaminfo(info);
    }
    /* Connect with a reset left held: released once and confirmed by the main-loop tick. */
    fake_reset = 1;
    clear_reset_on_connect(&a);
    REQUIRE(release_calls == 1 && a.reset_phase == RESET_RELEASING);
    REQUIRE(observe_state(&a) == 0);
    reset_tick(&a, lsl_local_clock());
    REQUIRE(a.reset_mode == 0 && a.reset_phase == RESET_IDLE);
    eeg_inlet = open_inlet(a.eeg);
    imp_inlet = open_inlet(a.impedance);
    marker_inlet = open_inlet(a.markers);
    REQUIRE(observe_state(&a) == 0);
    snapshot(&a, lsl_local_clock());
    /* Discovery and all three inlet subscriptions work without publishing any samples. */
    on_sample(a.h, 0, &a);
    REQUIRE(a.sample_count == 0 && adc_starts == 0);
    REQUIRE(lsl_pull_sample_d(eeg_inlet, values, 2, 0.05, &error) == 0);
    REQUIRE(lsl_pull_sample_d(imp_inlet, values, 2, 0.05, &error) == 0);
    {
        char *text = NULL;
        REQUIRE(lsl_pull_sample_str(marker_inlet, &text, 1, 0.05, &error) == 0);
    }
    REQUIRE(command(&a, "start") == 0 && a.acquiring && adc_starts == 1);
    on_sample(a.h, 0, &a);
    timestamp = lsl_pull_sample_d(eeg_inlet, values, 2, 5, &error);
    REQUIRE(timestamp > 0 && error == 0 && values[0] == 42 && values[1] == 7);
    REQUIRE(lsl_pull_sample_d(imp_inlet, values, 2, 5, &error) > 0);
    REQUIRE(isnan(values[0]) && isnan(values[1]));
    REQUIRE(command(&a, "on") == 0 && a.driver_mode == 1);
    expect_marker(marker_inlet, "impedance_on_requested");
    expect_marker(marker_inlet, "impedance_on_command_returned");
    expect_marker(marker_inlet, "impedance_state mode=1");
    a.next_impedance = 0;
    on_sample(a.h, 0.1, &a);
    REQUIRE(isnan(a.impedance_sample[0])); /* Warmup cannot publish stale estimates. */
    REQUIRE(lsl_pull_sample_d(imp_inlet, values, 2, 5, &error) > 0 && isnan(values[0]));
    a.valid_after = 0; a.next_impedance = 0;
    on_sample(a.h, 0.2, &a);
    REQUIRE(lsl_pull_sample_d(imp_inlet, values, 2, 5, &error) > 0);
    REQUIRE(values[0] == 123000 && values[1] == 456000);
    gui_state(&a, lsl_local_clock());
    drain(marker_inlet);
    /* Reset auto-release: mode stays 1 until Release. */
    {
        double t0 = lsl_local_clock();
        int releases = release_calls;
        REQUIRE(command(&a, "reset") == 0 && reset_calls == 1 && a.reset_phase == RESET_HOLDING);
        reset_tick(&a, t0 + 0.5); /* Still holding. */
        REQUIRE(release_calls == releases && fake_reset == 1);
        a.valid_after = 0; a.next_impedance = 0;
        on_sample(a.h, 0.3, &a);
        REQUIRE(isnan(a.impedance_sample[0]));
        reset_tick(&a, t0 + 1.1 + (lsl_local_clock() - t0));
        REQUIRE(release_calls == releases + 1 && a.reset_phase == RESET_RELEASING);
        REQUIRE(observe_state(&a) == 0 && a.reset_mode == 0);
        reset_tick(&a, lsl_local_clock());
        REQUIRE(a.reset_phase == RESET_IDLE && a.valid_after > lsl_local_clock());
        expect_next_marker(marker_inlet, "analog_reset_requested");
        expect_next_marker(marker_inlet, "analog_reset_started");
        expect_next_marker(marker_inlet, "analog_reset_state mode=1");
        expect_next_marker(marker_inlet, "analog_reset_released");
        expect_next_marker(marker_inlet, "analog_reset_state mode=0");
        a.next_impedance = 0;
        on_sample(a.h, 0.31, &a);
        REQUIRE(isnan(a.impedance_sample[0])); /* Settling after release. */
        a.valid_after = 0; a.next_impedance = 0;
        on_sample(a.h, 0.32, &a);
        REQUIRE(a.impedance_sample[0] == 123000 && a.impedance_sample[1] == 456000);
    }
    /* Stuck reset: release does not clear the mode. */
    {
        double t0;
        release_works = 0;
        REQUIRE(command(&a, "reset") == 0);
        t0 = lsl_local_clock();
        reset_tick(&a, t0 + 1.5);
        REQUIRE(observe_state(&a) == 0 && a.reset_mode == 1 && a.reset_phase == RESET_RELEASING);
        reset_tick(&a, t0 + 1.5 + 3.5);
        REQUIRE(a.reset_phase == RESET_FAILED);
        expect_marker(marker_inlet, "analog_reset_release_failed");
        a.valid_after = 0; a.next_impedance = 0;
        on_sample(a.h, 0.33, &a);
        REQUIRE(isnan(a.impedance_sample[0]) && isnan(a.impedance_sample[1]) && !a.failed);
        {   /* Telemetry carries reset_phase. */
            fflush(stdout);
            gui_state(&a, lsl_local_clock());
            snapshot(&a, lsl_local_clock());
        }
        expect_marker(marker_inlet,
            "state driver_mode=1 reset_mode=1 requested_on=1 command_pending=0 impedance_ready=0 reset_phase=failed");
        REQUIRE(command(&a, "status") == 0); /* Backend continues. */
        release_works = 1;
        fake_reset = 0; /* Hardware eventually clears: phase returns to idle. */
        REQUIRE(observe_state(&a) == 0);
        reset_tick(&a, lsl_local_clock());
        REQUIRE(a.reset_phase == RESET_IDLE);
    }
    /* Unexpected hold while idle: one marker, one release attempt. */
    {
        int releases = release_calls;
        fake_reset = 1;
        release_works = 0;
        REQUIRE(observe_state(&a) == 0);
        reset_tick(&a, lsl_local_clock());
        expect_marker(marker_inlet, "analog_reset_unexpectedly_held");
        REQUIRE(release_calls == releases + 1 && a.reset_phase == RESET_RELEASING);
        reset_tick(&a, lsl_local_clock() + 4);
        reset_tick(&a, lsl_local_clock() + 5);
        REQUIRE(release_calls == releases + 1 && a.reset_phase == RESET_FAILED);
        release_works = 1;
        fake_reset = 0;
        REQUIRE(observe_state(&a) == 0);
        reset_tick(&a, lsl_local_clock());
        REQUIRE(a.reset_phase == RESET_IDLE);
    }
    REQUIRE(command(&a, "off") == 0 && a.driver_mode == 0);
    a.next_impedance = 0;
    on_sample(a.h, 0.4, &a);
    REQUIRE(isnan(a.impedance_sample[0]));
    {
        lsl_outlet original_eeg = a.eeg, original_impedance = a.impedance, original_markers = a.markers;
        unsigned long long count = a.sample_count;
        REQUIRE(command(&a, "stop") == 0 && !a.acquiring && adc_stops == 1);
        on_sample(a.h, 0.5, &a);
        REQUIRE(a.sample_count == count); /* Trailing packets are suppressed. */
        REQUIRE(command(&a, "start") == 0 && a.acquiring && adc_starts == 2);
        REQUIRE(a.eeg == original_eeg && a.impedance == original_impedance && a.markers == original_markers);
    }
    test_timestamps(&a, marker_inlet);
    DSI_Headset_StartImpedanceDriver = fail_driver;
    REQUIRE(command(&a, "on") != 0 && a.driver_mode == 0 && isinf(a.valid_after));
    expect_marker(marker_inlet, "impedance_command_failed");
    REQUIRE(!a.failed); /* Expected rejection was handled and marked, not a push failure. */
    lsl_destroy_inlet(eeg_inlet); lsl_destroy_inlet(imp_inlet); lsl_destroy_inlet(marker_inlet);
    lsl_destroy_outlet(a.eeg); lsl_destroy_outlet(a.impedance); lsl_destroy_outlet(a.markers);
    free(a.channels); free(a.sources); free(a.eeg_sample); free(a.impedance_sample);
    puts("PASS: real LSL EEG/impedance transport, metadata, bool masking, warmup, reset hold/release/stuck/connect, timestamps, off, rejection, options");
    return 0;
}
