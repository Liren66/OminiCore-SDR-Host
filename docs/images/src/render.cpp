// Renders och terminal views through the real presentation code, using values
// measured on the OmniCoreSDR board on 2026-10-24. Output: ANSI text on stdout.
#include "cli/help_view.hpp"
#include "cli/board_view.hpp"
#include <cstring>
#include <string>
using namespace och_cli;
namespace p = och_cli::presentation;

och_device_info device() {
    och_device_info i{};
    std::strcpy(i.name, "OmniCoreSDR"); std::strcpy(i.serial, "OMINICORE00000001");
    std::strcpy(i.driver, "libusbK"); std::strcpy(i.firmware, "SDK_CMD_1.0_20261024_IQ8");
    i.abi_version = 1; i.usb_speed_mbps = 5000; i.rx_channels = 2; i.tx_channels = 2; i.max_sample_rate = 61440000;
    return i;
}
och_config bench_config() {
    och_config c{};
    c.sample_rate = 30720000; c.rx_mask = 3; c.tx_mask = 3; c.rx_frequency = c.tx_frequency = 1000000000;
    c.rx_bandwidth = c.tx_bandwidth = 18000000; c.rx_gain[0] = c.rx_gain[1] = 30;
    c.tx_attenuation[0] = c.tx_attenuation[1] = 10; c.rx_agc = 2; c.usb_iq_bits = 12;
    return c;
}
och_event ev(uint32_t type, uint64_t seq, uint64_t epoch, uint64_t ns, const char* msg) {
    och_event e{}; e.type = type; e.sequence = seq; e.epoch = epoch; e.host_monotonic_ns = ns;
    std::strncpy(e.message, msg, sizeof e.message - 1); return e;
}
void bench_header(std::ostream& o) {
    o << "PS C:\\> och bench --rx --tx --sample-rate 30.72M --channels 1,2 --seconds 12\n";
    p::title("bench", "throughput benchmark, RX + TX", o);
    p::section("Stream benchmark", o);
    p::group("Session", o);
    p::field("Device", "OMINICORE00000001", o, p::tone::accent);
    p::field("Direction", "RX + TX", o);
    p::field("Duration", "12.00 s", o);
    p::field("Sample format", "packed12  (USB wire packed12)", o);
    p::group("Transport", o);
    p::field("Host buffer", "64 MiB per direction", o);
    p::field("USB requests", "32 x 196608 bytes", o);
    p::end_group(o);
    o << "\n  ...  (RF configuration)\n\n";
    p::status(p::level::prep, "Allocating buffers and preparing streams.", o);
    p::group("TX source", o);
    p::field("Source", "Generated complex sine", o);
    p::field("Actual tone", "1000000.000 Hz", o);
    p::field("Amplitude", "0.25 of full scale", o);
    p::end_group(o);
    std::cerr << '\n';
    p::status(p::level::run, "Streaming started. Press Ctrl+C to stop cleanly.", std::cerr);
    p::event(ev(OCH_EVENT_RX_SYNC, 1, 1, 91893800910800, "RX preamble complete; new epoch=1"), 0.000, std::cerr);
    p::event(ev(OCH_EVENT_DEVICE, 2, 0, 91893816281300, "EVT seq=5 stream=12 type=rx_overflow mask=3 src=fpga count=2 lost=-1 action=waiting_rx_resync status=0x21551301 header_bytes=0"), 0.000, std::cerr);
    p::event(ev(OCH_EVENT_RX_SYNC, 3, 2, 91893824053700, "RX preamble complete; new epoch=2"), 0.001, std::cerr);
    p::event(ev(OCH_EVENT_DEVICE, 4, 0, 91893834545800, "EVT seq=6 stream=12 type=rx_overflow mask=3 src=fpga count=2 lost=-1 action=rx_restarted status=0x235f1600 header_bytes=16384"), 0.001, std::cerr);
}
void bench_summary(std::ostream& o) {
    p::status(p::level::stop, "Draining workers and closing streams.", std::cerr);
    och_stats rx{}, tx{};
    rx.elapsed_ns = 12195700000; rx.usb_bytes = 2243297280; rx.samples = 373779114; rx.headers = 2; rx.resyncs = 1;
    rx.host_queue_bytes = 589824; rx.host_queue_peak_bytes = 966656; rx.device_overflows = 1;
    tx.elapsed_ns = 12143200000; tx.usb_bytes = 2220490752; tx.samples = 376671744;
    tx.host_queue_bytes = 33292288; tx.host_queue_peak_bytes = 33879490; tx.device_overflows = 1;
    p::section("Stream complete", o);
    p::field("Stop reason", "Requested duration reached", o);
    p::field("Events received", 4, o);
    o << '\n';
    auto row = [](const char* n, const och_stats& s, uint64_t faults) {
        const double secs = s.elapsed_ns / 1e9;
        return p::throughput_row{n, 2, 30720000, secs, s.usb_bytes / secs / 1e6, 184.32, faults};
    };
    p::throughput({row("RX", rx, 2), row("TX", tx, 1)}, o);
    p::stats(OCH_RX, rx, 2, 30720000, o, 3);
    o << "\n  ...  (TX stream summary)\n";
}
const char* status_raw =
    "system_status init=ready init_code=0 digital=ready rx_channel=0 tx_channel=0 tx_source=0 tx_request=0 tx_actual_mask=0 usb_data_en=0,0\n"
    "temperature valid=1 Celsius=47.368 source=ad9361\n"
    "rssi rx=1 valid=1 value=-97.750 unit=dB_AD9361_relative calibrated_input_power=0\n"
    "rssi rx=2 valid=1 value=-95.250 unit=dB_AD9361_relative calibrated_input_power=0\n"
    "rf ensm=3 tx_silence=zero_samples_and_max_attenuation_when_off\n"
    "core_temp source=zynq valid=1 milli_celsius=66413 age_ms=60 overtemp=0\n"
    "rgb_status available=1 enabled=1 brightness_pct=15 state=idle red=0 green=38 blue=0 blink_hz=0\n"
    "fan_status available=1 enabled=0 frequency_hz=25000 duty_permille=0 target_permille=0 boost=0 failsafe=0 temp_valid=1 temp_mC=66413 temp_age_ms=60 overtemp=0 source=zynq\n"
    "errors=0x00 cfg_error=0 irq=0 cfg_busy=0 clear_busy=0\n"
    "faults rx_overflow=0 rx_underflow=0 tx_overflow=0 tx_underflow=0 rx_protocol=0 tx_protocol=0 rx_pair=0 tx_pair=0 last=0x00 event_seq=0 event_dropped=0 lost=-1\n"
    "rx_resync completed=0 pending=0 failures=0 header_bytes=16384\n";

int main(int argc, char** argv) {
    p::style st; st.color = st.unicode = true;
    p::configure(std::cout, st); p::configure(std::cerr, st);
    std::cerr.rdbuf(std::cout.rdbuf());
    auto& o = std::cout;
    const std::string mode = argc > 1 ? argv[1] : "info";
    if (mode == "info") {
        o << "PS C:\\> och info\n";
        p::device(device(), true, o);
        och_power_status pw{}; pw.log_endpoint_present = 1; p::power(pw, o);
    } else if (mode == "running") {
        bench_header(o);
        o << p::progress(st, 6.2, 12, {{"RX", 184.3}, {"TX", 184.2}}).first;
        return 0;
    } else if (mode == "summary") {
        o << "  ...\n";
        bench_summary(o);
    } else if (mode == "status") {
        o << "PS C:\\> och get system.status\n";
        p::reply("system.status", status_raw, o);
    } else if (mode == "find") {
        o << "PS C:\\> och find\n";
        p::devices({device()}, o);
        o << "\nPS C:\\> och get nonsense.key\n\n";
        p::status(p::level::fail, "Invalid argument: unknown canonical command: nonsense.key; use och commands", std::cerr);
        p::note("Exit code 2. See och help, or och commands for control names.", std::cerr);
    } else if (mode == "help") {
        o << "PS C:\\> och help\n";
        help_view::print_help(o);
    }
    o << "\nPS C:\\> ";
}
