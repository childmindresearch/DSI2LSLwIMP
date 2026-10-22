/* Hardware-free tests: real liblsl outlets/inlets, substituted DSI function pointers. */
#define main dsi2lsl_program_main
#include "../dsi2lsl.c"
#undef main

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static int fake_driver = 0, fake_reset = 0, reset_calls = 0;
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
static int is_trigger(DSI_Channel c) { return c == &channel_storage[1]; }
static int is_eeg_channel(DSI_Channel c) { return c == channel_storage; }
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
    DSI_Headset_StartDataAcquisition = start_adc;
    DSI_Headset_StopDataAcquisition = stop_adc;
    DSI_Headset_Idle = idle_adc;
    a.h = &headset_storage;
    a.driver_mode = a.reset_mode = -999;
    a.settle_seconds = 2;
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
    lsl_destroy_streaminfo(info);
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
    REQUIRE(command(&a, "reset") == 0 && reset_calls == 1);
    expect_marker(marker_inlet, "analog_reset_requested");
    expect_marker(marker_inlet, "analog_reset_command_returned");
    a.valid_after = 0; a.next_impedance = 0;
    on_sample(a.h, 0.3, &a);
    REQUIRE(isnan(a.impedance_sample[0]));
    fake_reset = 0;
    REQUIRE(observe_state(&a) == 0 && a.valid_after > lsl_local_clock());
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
    DSI_Headset_StartImpedanceDriver = fail_driver;
    REQUIRE(command(&a, "on") != 0 && a.driver_mode == 0 && isinf(a.valid_after));
    expect_marker(marker_inlet, "impedance_command_failed");
    REQUIRE(!a.failed); /* Expected rejection was handled and marked, not a push failure. */
    lsl_destroy_inlet(eeg_inlet); lsl_destroy_inlet(imp_inlet); lsl_destroy_inlet(marker_inlet);
    lsl_destroy_outlet(a.eeg); lsl_destroy_outlet(a.impedance); lsl_destroy_outlet(a.markers);
    free(a.channels); free(a.sources); free(a.eeg_sample); free(a.impedance_sample);
    puts("PASS: real LSL EEG/impedance transport, metadata, warmup, reset, off, rejection, options");
    return 0;
}
