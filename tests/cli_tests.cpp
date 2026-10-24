// SPDX-License-Identifier: MIT
// Offline CLI tests: argument parsing, numeric input, FTR files and the
// human-readable presentation layer. No device, driver or Windows API needed.
#include "cli/help_view.hpp"
#include "cli/options.hpp"
#include "cli/fir_file.hpp"
#include "cli/board_view.hpp"
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace och_cli;
namespace p = och_cli::presentation;

void check(bool condition, const char* expression, int line) {
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(value) check(bool(value), #value, __LINE__)
template<class F> bool throws(F f) {
    try { f(); } catch (const std::invalid_argument&) { return true; }
    return false;
}
bool has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

options parse_words(std::vector<std::string> words) {
    words.insert(words.begin(), "och");
    std::vector<char*> argv;
    for (auto& w : words) argv.push_back(w.data());
    return parse(static_cast<int>(argv.size()), argv.data());
}

// Capture what a view writes to std::cout with the given style, so the
// colour/Unicode code paths are exercised exactly as on a real console.
template<class F> std::string styled(F render, bool color = true, bool unicode = true) {
    std::ostringstream sink;
    auto* old = std::cout.rdbuf(sink.rdbuf());
    p::style st; st.color = color; st.unicode = unicode;
    p::configure(std::cout, st);
    try { render(std::cout); } catch (...) { std::cout.rdbuf(old); p::configure(std::cout, {}); throw; }
    std::cout.rdbuf(old);
    p::configure(std::cout, {});
    return sink.str();
}
template<class F> std::string plain(F render) { std::ostringstream out; render(out); return out.str(); }

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines; std::istringstream in(text); std::string line;
    while (std::getline(in, line)) lines.push_back(line);
    return lines;
}
void check_layout(const std::string& text, bool allow_escape, int line) {
    for (const auto& l : split_lines(text)) {
        if (p::detail::columns(l) > p::detail::width)
            throw std::runtime_error("line " + std::to_string(line) + ": wider than 78 columns: " + l);
        if (!allow_escape) {
            for (unsigned char c : l)
                if (c == 0x1B || c >= 0x80)
                    throw std::runtime_error("line " + std::to_string(line) + ": non-ASCII byte in plain output: " + l);
        }
    }
    if (allow_escape) {
        size_t open = 0, reset = 0;
        for (size_t i = text.find("\x1b["); i != std::string::npos; i = text.find("\x1b[", i + 1))
            (text.compare(i, 4, "\x1b[0m") == 0 ? reset : open)++;
        if (open != reset) throw std::runtime_error("line " + std::to_string(line) + ": unbalanced SGR sequences");
    }
}
#define CHECK_LAYOUT(text, escape) check_layout(text, escape, __LINE__)

// --- sample data -----------------------------------------------------------
och_device_info sample_device() {
    och_device_info i{};
    std::strcpy(i.name, "OmniCoreSDR"); std::strcpy(i.serial, "OMINICORE00000001");
    std::strcpy(i.driver, "libusbK"); std::strcpy(i.firmware, "SDK_CMD_1.0_20261024_IQ8");
    i.abi_version = 1; i.usb_speed_mbps = 5000; i.rx_channels = 2; i.tx_channels = 2; i.max_sample_rate = 61440000;
    return i;
}
och_config sample_config() {
    och_config c{};
    c.sample_rate = 30720000; c.rx_mask = 3; c.tx_mask = 3; c.rx_frequency = c.tx_frequency = 1000000000;
    c.rx_bandwidth = c.tx_bandwidth = 18000000; c.rx_gain[0] = c.rx_gain[1] = 30;
    c.tx_attenuation[0] = c.tx_attenuation[1] = 10; c.rx_agc = 2;
    return c;
}
och_stats sample_stats() {
    och_stats s{};
    s.elapsed_ns = 2000000000; s.usb_bytes = 368640000; s.samples = 61440000; s.headers = 1;
    s.host_queue_bytes = 393216; s.host_queue_peak_bytes = 966654;
    return s;
}
const char* status_dump =
    "system_status init=ready init_code=0 digital=ready rx_channel=0 tx_channel=0\n"
    "core_temp source=zynq valid=1 milli_celsius=66413 age_ms=60 overtemp=0\n"
    "faults rx_overflow=0 rx_underflow=3 tx_overflow=0 last_status=0x00000000 lost=-1\n"
    "adf4002_status source=0 visual_confirmation_required=1 reference_hz=10000000\n"
    "free text line without keys\n";

std::vector<std::function<void(std::ostream&)>> all_views() {
    return {
        [](std::ostream& o) { p::device(sample_device(), true, o); },
        [](std::ostream& o) { auto d=sample_device(); d.usb_speed_mbps=480; p::device(d, true, o); },
        [](std::ostream& o) { p::device(sample_device(), false, o); },
        [](std::ostream& o) { och_power_status s{}; s.log_endpoint_present = 1; p::power(s, o); },
        [](std::ostream& o) { och_power_status s{}; s.snapshot_valid = 1; s.attached_state = 2; s.supports_3a = 1; s.source_current_ma = 3000; p::power(s, o); },
        [](std::ostream& o) { p::devices({}, o); },
        [](std::ostream& o) { p::devices({sample_device(), sample_device()}, o); },
        [](std::ostream& o) { p::config(sample_config(), o); },
        [](std::ostream& o) { p::stats(OCH_RX, sample_stats(), 2, 30720000, o); },
        [](std::ostream& o) { p::stats(OCH_TX, sample_stats(), 2, 30720000, o); },
        [](std::ostream& o) { p::throughput({{"RX", 2, 61440000, 5.0, 366.9, 368.64, 0}, {"TX", 2, 61440000, 5.0, 360.1, 368.64, 3}}, o); },
        [](std::ostream& o) { p::reply("system.status", status_dump, o); },
        [](std::ostream& o) { p::reply("rx.lo-frequency", "rx.lo-frequency=1000000000\n", o); },
        [](std::ostream& o) { och_fir_config f{}; f.direction = OCH_TX; f.tap_count = 64; f.verified = 1; f.channel_mask = 3; f.factor = 2;
                              for (int i = 0; i < 64; ++i) { f.coefficients[i] = static_cast<int16_t>(i * 500 - 16000); } p::fir(f, o); },
        [](std::ostream& o) { och_clock_config c{}; c.reference_hz = 40000000;
                              for (int i = 0; i < 6; ++i) { c.rx_hz[i] = c.tx_hz[i] = 983040000u >> i; } p::clocks(c, o); },
        [](std::ostream& o) { och_event e{}; e.type = OCH_EVENT_DEVICE; e.sequence = 4;
                              std::strcpy(e.message, "EVT seq=2 stream=1 type=rx_overflow mask=3 src=fpga count=1 lost=-1 action=rx_restarted status=0x235f1600 header_bytes=16384");
                              p::event(e, 1234.5678, o); },
        [](std::ostream& o) { p::status(p::level::fail, "Invalid argument: a deliberately long message that has to wrap onto a second line cleanly", o); },
        [](std::ostream& o) { help_view::print_help(o); },
        [](std::ostream& o) { help_view::print_catalog(o); },
    };
}

// --- numeric and option parsing ---------------------------------------------
void numeric_suffixes() {
    CHECK(number("30.72M") == 30720000.0);
    CHECK(number("1G") == 1e9);
    CHECK(number("10k") == 10000.0 && number("10K") == 10000.0);
    CHECK(number("1e9") == 1e9 && number("-6") == -6.0 && number("+2.5") == 2.5);
    for (const char* bad : {"", "1x", "nan", "inf", "0x10", " 1", "1 ", "1e", "M", ".", "1..2", "1e400"})
        CHECK(throws([&] { number(bad); }));
}
void integer_ranges() {
    CHECK(integer("61.44M", 61440000) == 61440000);
    CHECK(throws([] { integer("1.5", 10); }));
    CHECK(throws([] { integer("-1", 10); }));
    CHECK(throws([] { integer("11", 10); }));
    CHECK(channels("1") == 1 && channels("2") == 2 && channels("1,2") == 3);
    for (const char* bad : {"3", "2,1", "1,", "", "12"}) CHECK(throws([&] { channels(bad); }));
}
void option_parsing() {
    CHECK(parse_words({}).command == "help");
    auto o = parse_words({"bench", "--rx", "--tx", "--rate=61.44M", "--channels", "1,2", "--json", "--no-color"});
    CHECK(o.command == "bench" && o.has("rx") && o.has("tx") && o.has("json") && o.has("no-color"));
    CHECK(o.get("sample-rate") == "61.44M" && o.get("channels") == "1,2");
    CHECK(throws([] { parse_words({"bench", "--rate", "1M", "--sample-rate", "2M"}); }));
    CHECK(throws([] { parse_words({"bench", "--bogus"}); }));
    CHECK(throws([] { parse_words({"bench", "--seconds"}); }));
    CHECK(throws([] { parse_words({"bench", "--seconds="}); }));
    CHECK(throws([] { parse_words({"bench", "--json=1"}); }));
    auto positional = parse_words({"set", "--", "--literal", "x"});
    CHECK(positional.positional.size() == 2 && positional.positional[0] == "--literal");
    CHECK(!throws([&] { allowed(o, {"rx", "tx", "sample-rate", "channels"}); }));
    CHECK(throws([&] { allowed(o, {"rx", "tx"}); }));
    CHECK(throws([] { allowed(parse_words({"version", "extra"}), {}); }));
}
void json_escaping() {
    CHECK(json("plain") == "\"plain\"");
    CHECK(json("a\"b\\c") == "\"a\\\"b\\\\c\"");
    CHECK(json("line\nnext\ttab\r") == "\"line\\nnext\\ttab\\r\"");
    CHECK(json(std::string("\x01", 1)) == "\"\\u0001\"");
}

// --- FTR files --------------------------------------------------------------
std::string taps(int count, int base = 100) {
    std::string out; for (int i = 0; i < count; ++i) out += std::to_string(base + i) + "\n"; return out;
}
void ftr_single_column() {
    const auto f = parse_ftr("# test\nRX 3 GAIN -6 DEC 2\n" + taps(16), OCH_RX);
    CHECK(f.config.direction == OCH_RX && f.config.tap_count == 16 && f.config.factor == 2);
    CHECK(f.config.gain_db == -6 && f.config.channel_mask == 3 && f.config.enabled == 0);
    CHECK(f.config.coefficients[0] == 100 && f.config.coefficients[15] == 115);
    CHECK(f.ignored_profile_fields.empty());
}
void ftr_two_columns_select_direction() {
    std::string text = "TX 3 GAIN 0 INT 1\nRX 3 GAIN 0 DEC 1\n";
    for (int i = 0; i < 16; ++i) text += std::to_string(i) + "," + std::to_string(-i) + "\n";
    CHECK(parse_ftr(text, OCH_TX).config.coefficients[5] == 5);
    CHECK(parse_ftr(text, OCH_RX).config.coefficients[5] == -5);
}
void ftr_rejections() {
    const std::string rx = "RX 3 GAIN 0 DEC 1\n";
    CHECK(throws([] { parse_ftr(taps(16), OCH_RX); }));                               // no header
    CHECK(throws([&] { parse_ftr(rx + taps(15), OCH_RX); }));                          // not multiple of 16
    CHECK(throws([] { parse_ftr("RX 1 GAIN 0 DEC 1\n" + taps(16), OCH_RX); }));      // mask must be 3
    CHECK(throws([] { parse_ftr("RX 3 GAIN 3 DEC 1\n" + taps(16), OCH_RX); }));      // invalid gain
    CHECK(throws([] { parse_ftr("TX 3 GAIN 0 INT 1\n" + taps(80), OCH_TX); }));      // >64 taps at INT 1
    CHECK(throws([&] { parse_ftr(rx + taps(16), OCH_TX); }));                          // direction missing
    CHECK(throws([&] { parse_ftr(rx + "1\x01\n" + taps(15), OCH_RX); }));              // control character
    CHECK(throws([&] { parse_ftr(rx + taps(15) + "40000\n", OCH_RX); }));              // out of int16 range
    const std::string profile = "RTX 983040000 245760000 122880000 61440000 30720000 30720000\n" + rx + taps(16);
    CHECK(throws([&] { parse_ftr(profile, OCH_RX); }));
    const auto accepted = parse_ftr(profile, OCH_RX, true);
    CHECK(accepted.ignored_profile_fields.size() == 1 && accepted.ignored_profile_fields[0] == "RTX");
}

// --- presentation -----------------------------------------------------------
void plain_output_is_ascii_and_fits() {
    for (const auto& view : all_views()) CHECK_LAYOUT(plain(view), false);
}
void styled_output_fits_and_balances() {
    for (const auto& view : all_views()) {
        const auto text = styled(view);
        CHECK_LAYOUT(text, true);
    }
    CHECK(has(styled([](std::ostream& o) { p::section("x", o); }), "\x1b["));
    CHECK(!has(styled([](std::ostream& o) { p::section("x", o); }, false, false), "\x1b["));
}
void string_streams_never_styled() {
    // Only std::cout / std::cerr may be styled; an ostringstream stays plain
    // even while std::cout is configured for colour.
    p::style st; st.color = st.unicode = true;
    p::configure(std::cout, st);
    std::ostringstream out; p::stats(OCH_RX, sample_stats(), 2, 30720000, out);
    p::configure(std::cout, {});
    CHECK(!has(out.str(), "\x1b[") && has(out.str(), "[ OK ]"));
}
void stats_verdicts() {
    auto s = sample_stats();
    auto text = plain([&](std::ostream& o) { p::stats(OCH_RX, s, 2, 30720000, o); });
    CHECK(has(text, "[ OK ]") && has(text, "Measured / expected       100.0 %"));
    CHECK(has(text, "368,640,000 bytes") && has(text, "30.720 MSPS per channel"));
    s.device_overflows = 1;
    CHECK(has(plain([&](std::ostream& o) { p::stats(OCH_RX, s, 2, 30720000, o); }), "[WARN]"));
    s.transfer_errors = 2;
    text = plain([&](std::ostream& o) { p::stats(OCH_RX, s, 2, 30720000, o); });
    CHECK(has(text, "[FAIL]") && has(text, "USB transfer errors       2"));
    auto tx = plain([&](std::ostream& o) { p::stats(OCH_TX, sample_stats(), 2, 30720000, o); });
    CHECK(has(tx, "Accepted samples/ch") && has(tx, "TX padding samples/ch") && has(tx, "USB completion does not confirm"));
    auto stalled = sample_stats(); stalled.usb_bytes /= 2;
    const auto low = plain([&](std::ostream& o) { p::stats(OCH_RX, stalled, 2, 30720000, o); });
    CHECK(has(low, "[WARN]") && has(low, "below 98%") && !has(low, "[ OK ]"));
    CHECK_LAYOUT(low, false);
    auto padded = sample_stats(); padded.tx_padding_samples = 16;
    CHECK(has(plain([&](std::ostream& o) { p::stats(OCH_TX, padded, 2, 30720000, o); }), "[WARN]"));
}
void throughput_table() {
    const auto text = plain([](std::ostream& o) {
        p::throughput({{"RX", 2, 61440000, 5.0, 368.64, 368.64, 0}, {"TX", 1, 61440000, 5.0, 175.0, 184.32, 7}}, o); });
    CHECK(has(text, "USB MB/s") && has(text, "100.0 %") && has(text, "94.9 %"));
    const auto lines = split_lines(text);
    CHECK(lines.size() == 4 && lines[2].rfind("  RX", 0) == 0 && lines[3].size() == lines[2].size());
    // Only the Faults cell carries the warning colour; the row text stays plain.
    const auto coloured = styled([](std::ostream& o) { p::throughput({{"TX", 1, 1, 1, 1, 1, 7}}, o); });
    CHECK(has(coloured, "\x1b[33m7\x1b[0m") && has(coloured, "\n  TX "));
    const auto clean = styled([](std::ostream& o) { p::throughput({{"RX", 1, 1, 1, 1, 1, 0}}, o); });
    CHECK(has(clean, "\x1b[90m0\x1b[0m") && !has(clean, "\x1b[33m"));
}
void reply_layout() {
    const auto text = plain([](std::ostream& o) { p::reply("system.status", status_dump, o); });
    // Every key survives, short groups use two columns, unknown text is kept.
    for (const char* key : {"init", "init_code", "digital", "milli_celsius", "rx_underflow", "lost",
                            "visual_confirmation_required", "reference_hz"}) CHECK(has(text, key));
    CHECK(has(text, "    init                ready            init_code           0"));
    CHECK(has(text, "free text line without keys"));
    CHECK(has(text, "  faults ---"));
    const auto coloured = styled([](std::ostream& o) { p::reply("s", "faults rx_underflow=3 rx_overflow=0 a=1 b=2\n", o); });
    CHECK(has(coloured, "\x1b[33m3\x1b[0m"));
    CHECK(!has(coloured, "\x1b[33m0\x1b[0m"));
    const auto single = plain([](std::ostream& o) { p::reply("rx.lo-frequency", "rx.lo-frequency=2400000000\n", o); });
    CHECK(has(single, "RX.LO-FREQUENCY") && has(single, "rx.lo-frequency             2400000000"));
    CHECK(has(plain([](std::ostream& o) { p::reply("x", "", o); }), "(empty reply)"));
}
void device_views() {
    const auto none = plain([](std::ostream& o) { p::devices({}, o); });
    CHECK(has(none, "[WARN]") && has(none, "No OmniCoreSDR device found") && has(none, "och-driver check"));
    const auto one = plain([](std::ostream& o) { p::devices({sample_device()}, o); });
    CHECK(has(one, "OMINICORE00000001") && has(one, "1 device found") && has(one, "61.44 MSPS"));
    const auto info = plain([](std::ostream& o) { p::device(sample_device(), true, o); });
    CHECK(has(info, "OmniCore Host " OCH_VERSION) && has(info, "5.0 Gbit/s  (SuperSpeed)") && has(info, "SDK_CMD_1.0"));
    auto slow = sample_device(); slow.usb_speed_mbps = 480;
    const auto hs = plain([&](std::ostream& o) { p::device(slow, true, o); });
    CHECK(has(hs, "480 Mbit/s  (High-Speed, USB 2.0)"));
    CHECK(has(hs, "RF sample-rate ceiling") && !has(hs, "Maximum sample rate"));
    CHECK_LAYOUT(hs, false);
    auto weird = sample_device(); std::strcpy(weird.name, "Bad\x1b[31mName\xC3\xA9");
    const auto escaped = plain([&](std::ostream& o) { p::device(weird, false, o); });
    CHECK(has(escaped, "Bad\\x1B[31mName\\xC3\\xA9"));
}
void value_helpers() {
    using namespace p::detail;
    CHECK(frequency(2400000000ull) == "2.400000 GHz  (2400000000 Hz)");
    CHECK(frequency(0) == "Unknown" && frequency(999) == "999 Hz");
    CHECK(rate(61440000) == "61.440 MSPS per channel");
    CHECK(grouped(0) == "0" && grouped(999) == "999" && grouped(1000) == "1,000" && grouped(1234567890) == "1,234,567,890");
    CHECK(bytes(512) == "512 B" && bytes(1536) == "1.5 KiB" && bytes(3 * 1048576ull) == "3.00 MiB");
    CHECK(mask(3) == "CH1 + CH2  (mask 3)" && mask(9) == "Unknown channel mask  (mask 9)");
    CHECK(columns("\x1b[1mab\x1b[0m\xE2\x94\x81") == 3);
}
void usb2_references() {
    auto d = sample_device(); d.usb_speed_mbps = 480;
    std::strcpy(d.firmware, usb2_reference::firmware);
    const auto info = plain([&](std::ostream& o) { p::device(d, true, o); });
    CHECK(!has(info, "USB2 streaming reference") && !has(info, "Reference host"));
    auto ss = d; ss.usb_speed_mbps = 5000;
    auto ss_info = plain([&](std::ostream& o) { p::device(ss, true, o); });
    auto hs_info = info;
    const auto hs_speed = hs_info.find("480 Mbit/s  (High-Speed, USB 2.0)");
    CHECK(hs_speed != std::string::npos);
    hs_info.replace(hs_speed, std::strlen("480 Mbit/s  (High-Speed, USB 2.0)"), "5.0 Gbit/s  (SuperSpeed)");
    CHECK(hs_info == ss_info);
    CHECK_LAYOUT(info, false);
    CHECK_LAYOUT(styled([&](std::ostream& o) { p::device(d, true, o); }), true);
    for (const auto& e : usb2_reference::entries) {
        CHECK(e.rate_hz > 0 && e.seconds >= 300);
        och_config c{}; c.sample_rate = e.rate_hz; c.usb_iq_bits = e.bits;
        c.rx_mask = e.rx_channels == 2 ? 3 : e.rx_channels;
        c.tx_mask = e.tx_channels == 2 ? 3 : e.tx_channels;
        CHECK(usb2_reference::find(d, c) == &e);
        CHECK(!has(plain([&](std::ostream& o) { p::usb2_stream_reference(d, c, o); }), "[WARN]"));
        ++c.sample_rate;
        const auto warning = plain([&](std::ostream& o) { p::usb2_stream_reference(d, c, o); });
        CHECK(has(warning, "[WARN]") && has(warning, "exceeds"));
        CHECK_LAYOUT(warning, false);
    }
    och_config c{}; c.rx_mask = 1; c.usb_iq_bits = 12;
    const auto* ch1 = usb2_reference::find(d, c); c.rx_mask = 2;
    CHECK(ch1 && !usb2_reference::find(d, c));
    c.rx_mask = 1; c.tx_mask = 3; CHECK(!usb2_reference::find(d, c)); // Unmeasured asymmetric duplex.
    c.tx_mask = 0; d.usb_speed_mbps = 5000;
    CHECK(!usb2_reference::find(d, c));
    d.usb_speed_mbps = 480; std::strcpy(d.firmware, "unmeasured");
    CHECK(!usb2_reference::find(d, c));
}
void startup_stability() {
    using namespace stream_assessment;
    auto observe = [](startup_observer& o, unsigned type, const char* message, uint64_t epoch, double seconds) {
        och_event e{}; e.type = type; e.epoch = epoch; std::strcpy(e.message, message); o.observe(e, seconds);
    };
    auto recover = [&](startup_observer& o, double seconds) {
        observe(o, OCH_EVENT_DEVICE, "EVT type=rx_overflow action=waiting_rx_resync status=0", 0, seconds);
        observe(o, OCH_EVENT_RX_SYNC, "RX preamble complete", 2, seconds + 0.01);
        observe(o, OCH_EVENT_DEVICE, "EVT type=rx_overflow action=rx_restarted status=0", 0, seconds + 0.02);
    };
    auto rx = sample_stats(); rx.device_overflows = rx.resyncs = 1;
    startup_observer start; recover(start, 0.05);
    CHECK(start.accepted(rx) == 1 && remaining_faults(rx, true, start.accepted(rx)) == 0);
    const auto accepted = plain([&](std::ostream& o) { p::stats(OCH_RX, rx, 2, 30720000, o, 3, start.accepted(rx)); });
    CHECK(has(accepted, "[ OK ]") && has(accepted, "Stable after accepted") && has(accepted, "Device RX overflows       1"));
    CHECK_LAYOUT(accepted, false);
    CHECK(rx.device_overflows == 1 && rx.resyncs == 1); // Original counters are never rewritten.
    auto tx = sample_stats(); tx.device_overflows = 1;
    CHECK(remaining_faults(tx, false, 1) == 0);
    ++tx.device_underflows; CHECK(remaining_faults(tx, false, 1) == 1);
    startup_observer late; recover(late, 1.01); CHECK(late.accepted(rx) == 0);
    startup_observer incomplete;
    observe(incomplete, OCH_EVENT_DEVICE, "EVT type=rx_overflow action=waiting_rx_resync status=0", 0, 0.1);
    CHECK(incomplete.accepted(rx) == 0);
    recover(start, 0.2); ++rx.device_overflows; ++rx.resyncs;
    CHECK(start.accepted(rx) == 2 && remaining_faults(rx, true, start.accepted(rx)) == 0);
    recover(start, 3.0); ++rx.device_overflows; ++rx.resyncs;
    CHECK(start.accepted(rx) == 2 && remaining_faults(rx, true, start.accepted(rx)) == 2);
    CHECK(throughput_ok(sample_stats(), 2, 30720000, 3));
    auto low = sample_stats(); low.usb_bytes /= 2;
    CHECK(!throughput_ok(low, 2, 30720000, 3));
}
void long_values_wrap_at_word_boundaries() {
    const std::string value = "alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi omicron pi rho";
    const auto text = plain([&](std::ostream& o) { p::field("A label", value, o); });
    const auto lines = split_lines(text);
    CHECK(lines.size() == 2);
    std::string joined = lines[0].substr(30) + " " + lines[1].substr(30);
    CHECK(joined == value);
    const auto long_label = plain([](std::ostream& o) { p::field("an unusually long label that cannot align", "v", o); });
    CHECK(split_lines(long_label).size() == 2);
}
void repeated_overflow_stop() {
    using namespace stream_assessment;
    och_event onset{}; onset.type = OCH_EVENT_DEVICE;
    std::strcpy(onset.message, "EVT type=rx_overflow action=waiting_rx_resync status=0");
    auto recovery = onset;
    std::strcpy(recovery.message, "EVT type=rx_overflow action=rx_restarted status=0");
    repeated_overflow_observer o;
    o.observe(onset, 0.1); o.observe(onset, 1.0);
    CHECK(!o.repeated && o.previous < 0);
    o.observe(onset, 1.1); o.observe(recovery, 1.2);
    CHECK(!o.repeated);
    o.observe(onset, 3.2); CHECK(!o.repeated); // Isolated faults do not abort.
    o.observe(onset, 5.2); CHECK(o.repeated);
    repeated_overflow_observer unrelated;
    auto other = onset; other.type = OCH_EVENT_RX_SYNC;
    unrelated.observe(other, 2); unrelated.observe(other, 2.1);
    unrelated.observe(onset, std::numeric_limits<double>::quiet_NaN());
    CHECK(!unrelated.repeated && unrelated.previous < 0);
}
void progress_line() {
    p::style ascii_style;
    auto line = p::progress(ascii_style, 15, 30, {{"RX", 245.76}, {"TX", 245.7}});
    CHECK(has(line.first, "############............") && has(line.first, " 50%") && has(line.first, "RX 245.8 MB/s"));
    CHECK(line.second == line.first.size() && line.second <= 78);
    CHECK(has(p::progress(ascii_style, 99, 30, {}).first, "100%"));
    CHECK(has(p::progress(ascii_style, 3, 0, {}).first, "RUN"));
    p::style fancy; fancy.color = fancy.unicode = true;
    auto styled_line = p::progress(fancy, 15, 30, {{"RX", 1}});
    CHECK(styled_line.second < styled_line.first.size() && styled_line.second <= 78);
}
void event_severity() {
    auto render = [](int type, const char* message) {
        och_event e{}; e.type = static_cast<uint32_t>(type); std::strcpy(e.message, message);
        return plain([&](std::ostream& o) { p::event(e, 0.5, o); });
    };
    CHECK(has(render(OCH_EVENT_TRANSPORT, "usb reset"), "[FAIL]"));
    CHECK(has(render(OCH_EVENT_HOST_OVERFLOW, "host ring full"), "[WARN]") );
    CHECK(has(render(OCH_EVENT_RX_SYNC, "RX preamble complete"), "[INFO]"));
    const auto recovered = render(OCH_EVENT_DEVICE, "EVT type=rx_overflow action=rx_restarted");
    CHECK(has(recovered, "[ OK ]") && has(recovered, "RECOVERED"));
    CHECK(has(render(OCH_EVENT_DEVICE, "EVT type=rx_overflow action=waiting_rx_resync"), "[WARN]"));
    CHECK(has(render(OCH_EVENT_DEVICE, ""), "(empty event message)"));
}
void help_and_catalog_complete() {
    const auto help = plain([](std::ostream& o) { help_view::print_help(o); });
    for (const char* word : {"find", "info", "power", "version", "commands", "get <name>", "set <name>", "action <name>",
                             "config", "selftest", "fir status", "clock status", "rx --file", "tx --tone",
                             "bench --rx", "--serial", "--json", "--no-color", "--iq-bits", "--overwrite"})
        CHECK(has(help, word));
    CHECK(help_view::help_text() == help);
    const auto catalog = plain([](std::ostream& o) { help_view::print_catalog(o); });
    for (const auto& entry : commands::catalog()) CHECK(has(catalog, entry.name));
}
void board_views_feed_reply() {
    och_fan_status fan{}; fan.available = 1; fan.frequency_hz = 25000; fan.temperature_valid = 1; fan.temperature_mc = 51234;
    const auto raw = board_view::fan(fan);
    CHECK(has(raw, "source=zynq") && has(raw, "temp_mC=51234"));
    const auto text = plain([&](std::ostream& o) { p::reply("fan.status", raw, o); });
    CHECK(has(text, "fan_status ---") && has(text, "frequency_hz") && has(text, "25000"));
}
} // namespace

int main() {
    const std::vector<std::pair<const char*, void (*)()>> tests = {
        {"numeric values accept k/M/G and reject partial input", numeric_suffixes},
        {"integer ranges and channel lists", integer_ranges},
        {"option parsing, aliases, conflicts and allowed sets", option_parsing},
        {"JSON string escaping", json_escaping},
        {"FTR single-column file", ftr_single_column},
        {"FTR two-column file selects TX/RX column", ftr_two_columns_select_direction},
        {"FTR invalid files are rejected before device access", ftr_rejections},
        {"plain output is 7-bit ASCII and fits 78 columns", plain_output_is_ascii_and_fits},
        {"styled output fits 78 columns with balanced SGR", styled_output_fits_and_balances},
        {"string streams are never styled", string_streams_never_styled},
        {"stream summary verdicts OK / WARN / FAIL", stats_verdicts},
        {"throughput table columns and ratios", throughput_table},
        {"SDK reply grouping, two-column layout and fault colouring", reply_layout},
        {"device discovery and identity views", device_views},
        {"USB2 references match measured channel/format/firmware only", usb2_references},
        {"startup recovery allowance preserves faults and rejects later errors", startup_stability},
        {"fail-fast ignores startup/recovery events and stops repeated overflows", repeated_overflow_stop},
        {"value formatting helpers", value_helpers},
        {"long values wrap at word boundaries without loss", long_values_wrap_at_word_boundaries},
        {"live progress line", progress_line},
        {"event severity and origin", event_severity},
        {"help and catalog cover every command", help_and_catalog_complete},
        {"board status views render through reply", board_views_feed_reply},
    };
    size_t failed = 0;
    for (const auto& test : tests) {
        try { test.second(); std::cout << "[PASS] " << test.first << '\n'; }
        catch (const std::exception& e) { ++failed; std::cout << "[FAIL] " << test.first << ": " << e.what() << '\n'; }
    }
    std::cout << '\n' << (tests.size() - failed) << '/' << tests.size() << " CLI tests passed\n";
    return failed ? 1 : 0;
}
