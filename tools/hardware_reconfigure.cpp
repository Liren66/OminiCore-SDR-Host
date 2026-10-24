// Public-API hardware regression for live reconfiguration. Never transmits
// unless --tx-zero or --duplex-zero is explicitly supplied. No IQ is saved.
#include <omnicore/och.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
uint64_t ns() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
double ms(uint64_t a, uint64_t b) { return (static_cast<double>(a) - static_cast<double>(b)) / 1e6; }
std::mutex output_mutex;
std::string quote(const std::string& text) {
    std::ostringstream out; out << '"';
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
        else if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c < 32) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<unsigned>(c) << std::dec;
        else out << static_cast<char>(c);
    }
    return out.str() + '"';
}
void log(const std::string& fields) {
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << '{' << fields << "}\n" << std::flush;
}
void check(bool ok, const std::string& why) { if (!ok) throw std::runtime_error(why); }
void call(int rc, const std::string& what) {
    if (rc != OCH_OK) throw std::runtime_error(what + " rc=" + std::to_string(rc) + " error=" + och_last_error());
}
och_stats stats(och_stream* stream) {
    och_stats st{}; st.struct_size = sizeof st;
    if (stream) call(och_get_stats(stream, &st), "get_stats");
    return st;
}
struct Options {
    enum Mode { Rx, TxZero, DuplexZero } mode = Rx;
    std::string serial;
    uint32_t rate = 10000000, rounds = 2, timeout = 10000, settle = 100;
    bool rf_controls = false;
};
uint32_t number(const char* s, const char* name, uint32_t low, uint32_t high) {
    size_t consumed = 0; auto n = std::stoull(s, &consumed);
    check(consumed == std::strlen(s) && n >= low && n <= high, std::string("invalid ") + name);
    return static_cast<uint32_t>(n);
}
void usage() {
    std::cout << "hardware_reconfigure [--rx | --tx-zero | --duplex-zero] [--serial SERIAL]\n"
                 "  [--sample-rate HZ] [--rounds N] [--timeout-ms MS] [--settle-ms MS]\n"
                 "  [--rf-controls]\n"
                 "Default: RX only, 1 channel, 10 MSPS, 100 MHz, manual gain 30 dB.\n"
                 "TX requires an explicit zero mode; every IQ byte is zero, attenuation 89.75 dB.\n"
                 "Round 1 uses recv; round 2 uses acquire/release (alternating thereafter).\n"
                 "--rf-controls additionally reapplies the current clock chain, verified RX/TX FIR\n"
                 "coefficients, and saved FIR enable states, then verifies identical readback.\n"
                 "It tests control/restart lifecycle, not a new filter or its frequency response.\n"
                 "Outputs JSON lines. Timing uses host observation, not hardware sample timestamps.\n"
                 "call_ms measures the configuration API; first_new_epoch_after_return_ms measures\n"
                 "the first observed reconfigured RX epoch relative to API return (may be negative).\n"
                 "Failures return nonzero. No IQ recording, flashing, or persistent output files.\n";
}
struct Observation { uint64_t at, epoch, index; uint32_t flags; };
struct Harness {
    Options options;
    och_device* dev = nullptr;
    och_stream* rx = nullptr;
    och_stream* tx = nullptr;
    och_config config{};
    och_stream_args rx_args{}, tx_args{};
    std::atomic<bool> stop{false}, pause_rx{false}, rx_paused{false}, acquire_mode{false};
    std::atomic<uint64_t> rx_blocks{0}, rx_samples{0}, tx_samples{0}, rx_timeouts{0}, tx_timeouts{0};
    std::thread consumer, producer;
    std::mutex observation_mutex, failure_mutex;
    std::condition_variable observation_cv;
    std::deque<Observation> observations;
    uint64_t last_epoch = 0, reconfigured_blocks = 0, reconfigured_events = 0;
    std::string failure;
    bool lease_held = false;
    unsigned changes = 0;
    explicit Harness(Options opts) : options(std::move(opts)) {}
    ~Harness() { cleanup(); }
    bool has_rx() const { return options.mode != Options::TxZero; }
    bool has_tx() const { return options.mode != Options::Rx; }
    void failed(const std::string& text) {
        { std::lock_guard<std::mutex> lock(failure_mutex); if (failure.empty()) failure = text; }
        stop = true; observation_cv.notify_all();
    }
    void check_worker() {
        std::lock_guard<std::mutex> lock(failure_mutex);
        check(failure.empty(), failure);
    }
    void observe(const och_metadata& m, size_t frames) {
        check(frames != 0 && m.epoch != 0, "RX returned empty samples or epoch zero");
        std::lock_guard<std::mutex> lock(observation_mutex);
        check(m.epoch >= last_epoch, "RX epoch moved backwards");
        if (m.flags & OCH_META_RECONFIGURED) {
            const uint32_t expected = OCH_META_START | OCH_META_DISCONTINUITY | OCH_META_TIME_UNKNOWN;
            check((m.flags & expected) == expected, "reconfigured RX block lacks START/DISCONTINUITY/TIME_UNKNOWN");
            // A real device overflow before the first delivered IQ may coexist
            // with RECONFIGURED; preserve/report flags rather than reject it.
            ++reconfigured_blocks;
        }
        if (m.epoch != last_epoch || (m.flags & OCH_META_RECONFIGURED)) {
            observations.push_back({ns(), m.epoch, m.sample_index, m.flags});
            if (observations.size() > 1024) observations.pop_front();
        }
        last_epoch = m.epoch; ++rx_blocks; rx_samples += frames; observation_cv.notify_all();
    }
    void receive() {
        try {
            std::vector<uint8_t> buffer(rx_args.transfer_bytes);
            while (!stop) {
                if (pause_rx) { rx_paused = true; std::this_thread::sleep_for(1ms); continue; }
                rx_paused = false;
                och_metadata m{}; m.struct_size = sizeof m; size_t count = 0;
                int rc;
                if (acquire_mode) {
                    const void* data = nullptr;
                    rc = och_rx_acquire(rx, &data, &count, &m, 100);
                    if (rc == OCH_OK) {
                        // Release before recording the observation, shortening the lease.
                        call(och_rx_release(rx), "consumer RX release");
                        check(data && count && count % 3 == 0, "invalid RX lease");
                        observe(m, count / 3);
                    }
                } else {
                    rc = och_recv(rx, buffer.data(), buffer.size() / 3, &count, &m, 100);
                    if (rc == OCH_OK) observe(m, count);
                }
                if (rc == OCH_TIMEOUT) { ++rx_timeouts; std::this_thread::sleep_for(1ms); }
                else if (rc != OCH_OK) { if (stop && rc == OCH_STOPPED) break; call(rc, "continuous RX"); }
            }
        } catch (const std::exception& e) { failed(e.what()); }
        rx_paused = true;
    }
    void transmit() {
        try {
            std::vector<uint8_t> zeros(tx_args.transfer_bytes, 0);
            while (!stop) {
                size_t accepted = 0;
                int rc = och_send(tx, zeros.data(), zeros.size() / 3, &accepted, 100);
                tx_samples += accepted;
                if (rc == OCH_TIMEOUT) { ++tx_timeouts; std::this_thread::sleep_for(1ms); }
                else if (rc != OCH_OK) { if (stop && rc == OCH_STOPPED) break; call(rc, "continuous zero TX"); }
            }
        } catch (const std::exception& e) { failed(e.what()); }
    }
    void events() {
        if (!dev) return;
        for (;;) {
            och_event e{}; e.struct_size = sizeof e;
            int rc = och_poll_event(dev, &e, 0);
            if (rc == OCH_TIMEOUT) break;
            call(rc, "poll event");
            if (e.type == OCH_EVENT_RECONFIGURED) ++reconfigured_events;
            log("\"kind\":\"event\",\"type\":" + std::to_string(e.type) +
                ",\"sequence\":" + std::to_string(e.sequence) + ",\"epoch\":" + std::to_string(e.epoch) +
                ",\"message\":" + quote(e.message));
        }
    }
    void print_stats(const char* stage) {
        for (auto pair : {std::make_pair("rx", rx), std::make_pair("tx", tx)}) if (pair.second) {
            auto s = stats(pair.second);
            std::ostringstream line;
            line << "\"kind\":\"stats\",\"stage\":" << quote(stage) << ",\"direction\":" << quote(pair.first)
                 << ",\"running\":" << s.running << ",\"usb_bytes\":" << s.usb_bytes << ",\"samples\":" << s.samples
                 << ",\"padding_samples\":" << s.tx_padding_samples << ",\"host_dropped_samples\":" << s.host_dropped_samples
                 << ",\"device_overflows\":" << s.device_overflows << ",\"device_underflows\":" << s.device_underflows
                 << ",\"transfer_errors\":" << s.transfer_errors << ",\"starvation_events\":" << s.tx_starvation_events
                 << ",\"headers\":" << s.headers << ",\"resyncs\":" << s.resyncs << ",\"event_drops\":" << s.event_drops;
            log(line.str());
        }
    }
    void running() {
        check_worker();
        if (rx) check(stats(rx).running != 0, "RX did not remain running");
        if (tx) check(stats(tx).running != 0, "TX did not remain running");
    }
    Observation wait_observation(uint64_t after, bool reconfigured) {
        std::unique_lock<std::mutex> lock(observation_mutex);
        auto find = [&] {
            return std::find_if(observations.begin(), observations.end(), [&](const Observation& o) {
                return o.at >= after && (!reconfigured || (o.flags & OCH_META_RECONFIGURED));
            });
        };
        bool ready = observation_cv.wait_for(lock, std::chrono::milliseconds(options.timeout), [&] { return stop || find() != observations.end(); });
        auto found = find();
        if (!ready || found == observations.end()) {
            lock.unlock(); check_worker();
            throw std::runtime_error(reconfigured ? "no new RECONFIGURED RX epoch before timeout" : "initial RX samples timed out");
        }
        return *found;
    }
    uint64_t reconfigured_count() { std::lock_guard<std::mutex> lock(observation_mutex); return reconfigured_blocks; }
    void initialize() {
        call(och_open(options.serial.empty() ? nullptr : options.serial.c_str(), &dev), "open");
        och_device_info info{}; info.struct_size = sizeof info; call(och_get_info(dev, &info), "get_info");
        log("\"kind\":\"device\",\"version\":" + quote(och_version()) + ",\"serial\":" + quote(info.serial) +
            ",\"firmware\":" + quote(info.firmware) + ",\"hardware_timestamps\":" + std::to_string(info.hardware_timestamps));
        och_default_config(&config); config.rx_mask = has_rx() ? 1 : 0; config.tx_mask = has_tx() ? 1 : 0;
        config.sample_rate = options.rate; config.rx_frequency = config.tx_frequency = 100000000;
        config.rx_bandwidth = config.tx_bandwidth = 5000000; config.rx_agc = 0;
        config.rx_gain[0] = config.rx_gain[1] = 30; config.tx_attenuation[0] = config.tx_attenuation[1] = 89.75;
        call(och_configure(dev, &config), "initial configure");
        if (rx == nullptr && has_rx()) {
            och_default_stream_args(&rx_args, OCH_RX); rx_args.format = OCH_PACKED12;
            call(och_create_stream(dev, &rx_args, &rx), "create RX");
        }
        if (tx == nullptr && has_tx()) {
            och_default_stream_args(&tx_args, OCH_TX); tx_args.format = OCH_PACKED12;
            call(och_create_stream(dev, &tx_args, &tx), "create zero TX");
            std::vector<uint8_t> zeros(tx_args.transfer_bytes * 8ULL, 0); size_t accepted = 0;
            call(och_send(tx, zeros.data(), zeros.size() / 3, &accepted, 1000), "initial TX prefill");
            check(accepted == zeros.size() / 3, "initial zero TX prefill was partial");
            tx_samples += accepted; call(och_start(tx), "start zero TX");
            producer = std::thread([this] { transmit(); });
        }
        if (rx) { call(och_start(rx), "start RX"); consumer = std::thread([this] { receive(); }); wait_observation(0, false); }
        std::this_thread::sleep_for(std::chrono::milliseconds(options.settle));
        running(); events(); print_stats("initial");
    }
    void unchanged(const char* name, const std::function<int()>& operation, int expected) {
        events(); const auto before_events = reconfigured_events, before_rx = reconfigured_count();
        auto before_tx = stats(tx); auto began = ns(); int rc = operation(); auto ended = ns();
        const std::string error = rc ? och_last_error() : "";
        check(rc == expected, std::string(name) + " returned " + std::to_string(rc) + ", expected " + std::to_string(expected) + ": " + error);
        running(); events(); auto after_tx = stats(tx);
        check(reconfigured_events == before_events && reconfigured_count() == before_rx, std::string(name) + " unexpectedly reconfigured RX/device");
        if (tx) check(after_tx.tx_padding_samples == before_tx.tx_padding_samples, std::string(name) + " unexpectedly restarted TX");
        och_config actual{}; actual.struct_size = sizeof actual; call(och_get_config(dev, &actual), "config after rejection/no-op");
        check(actual.rx_frequency == config.rx_frequency && actual.sample_rate == config.sample_rate &&
              actual.rx_mask == config.rx_mask && actual.tx_mask == config.tx_mask, "rejection/no-op altered applied configuration");
        std::ostringstream line; line << "\"kind\":\"check\",\"name\":" << quote(name) << ",\"rc\":" << rc
            << ",\"call_ms\":" << ms(ended, began) << ",\"pass\":true,\"error\":" << quote(error); log(line.str());
    }
    void rejection_checks() {
        unchanged("no_op", [&] { return och_configure(dev, &config); }, OCH_OK);
        auto invalid = config; invalid.sample_rate = 0;
        unchanged("invalid_config", [&] { return och_configure(dev, &invalid); }, OCH_INVALID);
        auto mask = config; if (rx) mask.rx_mask = 3; else mask.tx_mask = 3;
        unchanged("stream_mask_change", [&] { return och_configure(dev, &mask); }, OCH_BUSY);
        if (!rx) return;
        pause_rx = true; auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout);
        while (!rx_paused && Clock::now() < deadline) { check_worker(); std::this_thread::sleep_for(1ms); }
        check(rx_paused, "consumer did not pause for lease test");
        const void* data = nullptr; size_t count = 0; och_metadata metadata{}; metadata.struct_size = sizeof metadata;
        call(och_rx_acquire(rx, &data, &count, &metadata, options.timeout), "lease rejection test acquire"); lease_held = true;
        check(data && count, "lease rejection test empty lease");
        std::vector<uint8_t> prefix(std::min<size_t>(count, 256)); std::memcpy(prefix.data(), data, prefix.size());
        auto proposed = config; proposed.rx_frequency += 1000000;
        unchanged("outstanding_lease", [&] { return och_configure(dev, &proposed); }, OCH_BUSY);
        check(std::memcmp(prefix.data(), data, prefix.size()) == 0, "rejected reconfigure changed leased bytes");
        call(och_rx_release(rx), "release rejection-test lease"); lease_held = false; pause_rx = false;
    }
    void change(const std::string& name, const std::function<int()>& operation) {
        check_worker(); auto before_tx = stats(tx); const uint64_t before_rx_samples = rx_samples;
        uint64_t began = 0, ended = 0; unsigned busy = 0; auto deadline = Clock::now() + std::chrono::milliseconds(options.timeout);
        for (;;) {
            began = ns(); int rc = operation(); ended = ns();
            if (rc == OCH_BUSY && acquire_mode && rx) {
                // A consumer lease can race the management thread legitimately.
                ++busy; check(Clock::now() < deadline, "continuous RX leases starved reconfiguration");
                std::this_thread::sleep_for(1ms); continue;
            }
            call(rc, name); break;
        }
        ++changes; running();
        Observation observed{}; if (rx) observed = wait_observation(began, true);
        auto after_tx = stats(tx);
        if (tx) check(after_tx.tx_padding_samples > before_tx.tx_padding_samples, name + " did not account zero TX restart padding");
        auto target_tx_bytes = after_tx.usb_bytes;
        const auto progress_deadline = Clock::now() + std::chrono::milliseconds(options.timeout);
        while ((tx && stats(tx).usb_bytes == target_tx_bytes) || (rx && rx_samples == before_rx_samples)) {
            running(); check(Clock::now() < progress_deadline, name + " no stream progress after reconfiguration");
            std::this_thread::sleep_for(1ms);
        }
        call(och_get_config(dev, &config), "read applied configuration");
        std::ostringstream line; line << std::fixed << std::setprecision(3);
        line << "\"kind\":\"change\",\"name\":" << quote(name) << ",\"pass\":true,\"call_ms\":" << ms(ended, began)
             << ",\"lease_busy_retries\":" << busy << ",\"rx_api\":" << quote(acquire_mode ? "acquire" : "recv")
             << ",\"sample_rate\":" << config.sample_rate;
        if (rx) line << ",\"new_epoch\":" << observed.epoch << ",\"flags\":" << observed.flags
                     << ",\"first_new_epoch_after_return_ms\":" << ms(observed.at, ended)
                     << ",\"first_new_epoch_from_call_ms\":" << ms(observed.at, began);
        if (tx) line << ",\"tx_running\":" << after_tx.running << ",\"tx_padding_added\":" << (after_tx.tx_padding_samples - before_tx.tx_padding_samples)
                     << ",\"tx_underflows_added\":" << (after_tx.device_underflows - before_tx.device_underflows);
        log(line.str()); events(); std::this_thread::sleep_for(std::chrono::milliseconds(options.settle));
    }
    void rf_control_checks() {
        och_clock_config saved_clock{}; saved_clock.struct_size = sizeof saved_clock;
        call(och_get_clock(dev, &saved_clock), "snapshot clock for identical reapply");
        auto verify_clock = [&] {
            och_clock_config actual{}; actual.struct_size = sizeof actual;
            call(och_get_clock(dev, &actual), "clock readback after RF control");
            check(actual.reference_hz == saved_clock.reference_hz &&
                  std::equal(saved_clock.rx_hz, saved_clock.rx_hz + 6, actual.rx_hz) &&
                  std::equal(saved_clock.tx_hz, saved_clock.tx_hz + 6, actual.tx_hz),
                  "RF control changed the saved clock chain");
        };
        change("clock_same_chain", [&] { return och_set_clock(dev, &saved_clock); });
        verify_clock();
        log("\"kind\":\"readback\",\"name\":\"clock_same_chain\",\"pass\":true,\"identical\":true");

        och_fir_config saved_fir[2]{};
        bool verified[2]{};
        // Save both directions before rewriting either. Even RX-only mode may
        // reapply a verified TX filter; no TX stream is created or started here.
        for (unsigned direction = 0; direction < 2; ++direction) {
            auto& saved = saved_fir[direction]; saved.struct_size = sizeof saved;
            const auto name = direction == OCH_RX ? "rx_fir" : "tx_fir";
            const int rc = och_get_fir(dev, static_cast<och_direction>(direction), &saved);
            if (rc == OCH_UNSUPPORTED) {
                log("\"kind\":\"skip\",\"name\":" + quote(name) + ",\"reason\":" + quote(och_last_error()));
                continue;
            }
            call(rc, std::string("snapshot ") + name);
            check(saved.direction == direction && saved.tap_count <= 128, std::string(name) + " snapshot invalid");
            if (saved.verified != 1) {
                log("\"kind\":\"skip\",\"name\":" + quote(name) + ",\"reason\":\"saved firmware FIR is not verified; no coefficients rewritten\"");
                continue;
            }
            verified[direction] = true;
            log("\"kind\":\"snapshot\",\"name\":" + quote(name) + ",\"enabled\":" + std::to_string(saved.enabled) +
                ",\"factor\":" + std::to_string(saved.factor) + ",\"tap_count\":" + std::to_string(saved.tap_count) +
                ",\"crc32\":" + std::to_string(saved.crc32));
        }
        auto verify_fir = [&](unsigned direction) {
            const auto& saved = saved_fir[direction];
            och_fir_config actual{}; actual.struct_size = sizeof actual;
            call(och_get_fir(dev, static_cast<och_direction>(direction), &actual), "FIR readback after identical reapply");
            check(actual.verified == 1 && actual.direction == saved.direction && actual.channel_mask == saved.channel_mask &&
                  actual.factor == saved.factor && actual.gain_db == saved.gain_db && actual.tap_count == saved.tap_count &&
                  actual.enabled == saved.enabled && actual.crc32 == saved.crc32 &&
                  std::equal(saved.coefficients, saved.coefficients + saved.tap_count, actual.coefficients),
                  std::string(direction == OCH_RX ? "RX" : "TX") + " FIR readback differs from saved coefficients/configuration");
        };
        for (unsigned direction = 0; direction < 2; ++direction) if (verified[direction]) {
            const std::string name = direction == OCH_RX ? "rx_fir_same_coefficients" : "tx_fir_same_coefficients";
            change(name, [&] { return och_set_fir(dev, &saved_fir[direction]); });
            verify_fir(direction); verify_clock();
            log("\"kind\":\"readback\",\"name\":" + quote(name) + ",\"pass\":true,\"identical\":true");
        }
        if (verified[OCH_RX] && verified[OCH_TX]) {
            change("fir_same_enable_states", [&] { return och_set_fir_enabled(dev, saved_fir[OCH_RX].enabled, saved_fir[OCH_TX].enabled); });
            verify_fir(OCH_RX); verify_fir(OCH_TX); verify_clock();
            log("\"kind\":\"readback\",\"name\":\"fir_same_enable_states\",\"pass\":true,\"identical\":true");
        } else {
            log("\"kind\":\"skip\",\"name\":\"fir_same_enable_states\",\"reason\":\"both saved FIR directions must be supported and verified\"");
        }
        // Check all available snapshots once more after the complete sequence.
        for (unsigned direction = 0; direction < 2; ++direction) if (verified[direction]) verify_fir(direction);
        verify_clock(); running(); events();
    }
    void run() {
        initialize(); rejection_checks();
        for (uint32_t round = 0; round < options.rounds; ++round) {
            acquire_mode = (round & 1) != 0;
            auto updated = config; updated.rx_frequency = updated.tx_frequency = (round & 1) ? 100000000 : 101000000;
            change("frequency_" + std::to_string(round + 1), [&] { return och_configure(dev, &updated); });
            updated = config; updated.rx_gain[0] = updated.rx_gain[1] = (round & 1) ? 30 : 31;
            change("gain_" + std::to_string(round + 1), [&] { return och_configure(dev, &updated); });
            updated = config; updated.rx_bandwidth = updated.tx_bandwidth = (round & 1) ? 5000000 : 4000000;
            change("bandwidth_" + std::to_string(round + 1), [&] { return och_configure(dev, &updated); });
            const auto alternative = options.rate == 30720000 ? 15360000U : (options.rate == 12000000 ? 10000000U : 12000000U);
            const auto rate = (round & 1) ? options.rate : alternative;
            change("sample_rate_" + std::to_string(round + 1), [&] { return och_set_sample_rate(dev, rate); });
        }
        if (options.rf_controls) rf_control_checks();
        unchanged("final_no_op", [&] { return och_configure(dev, &config); }, OCH_OK);
        running(); events(); print_stats("final_running");
        check(reconfigured_events == changes, "wrong number of RECONFIGURED events");
        if (rx) check(reconfigured_count() == changes, "wrong number of RECONFIGURED RX markers");
        const auto rx_st = stats(rx), tx_st = stats(tx);
        check((!rx || rx_st.transfer_errors == 0) && (!tx || tx_st.transfer_errors == 0), "USB transfer errors occurred");
        cleanup(); check_worker();
        log("\"kind\":\"summary\",\"pass\":true,\"changes\":" + std::to_string(changes) +
            ",\"rx_samples\":" + std::to_string(rx_samples.load()) + ",\"tx_samples_accepted\":" + std::to_string(tx_samples.load()) +
            ",\"rx_timeouts\":" + std::to_string(rx_timeouts.load()) + ",\"tx_timeouts\":" + std::to_string(tx_timeouts.load()) +
            ",\"timing\":\"host_API_and_observation_only_not_hardware_sample_time\"");
    }
    void cleanup() noexcept {
        stop = true; pause_rx = false; observation_cv.notify_all();
        if (consumer.joinable()) consumer.join();
        if (producer.joinable()) producer.join();
        if (lease_held && rx) { och_rx_release(rx); lease_held = false; }
        for (auto slot : {&rx, &tx}) if (*slot) {
            const char* direction = slot == &rx ? "RX" : "TX";
            int rc = och_stop(*slot, 3000);
            if (rc != OCH_OK) { const auto text = std::string("cleanup stop ") + direction + ": " + och_last_error(); failed(text); log("\"kind\":\"cleanup_error\",\"message\":" + quote(text)); }
            rc = och_destroy_stream(*slot); *slot = nullptr;
            if (rc != OCH_OK) { const auto text = std::string("cleanup destroy ") + direction + ": " + och_last_error(); failed(text); log("\"kind\":\"cleanup_error\",\"message\":" + quote(text)); }
        }
        if (dev) {
            try { events(); } catch (const std::exception& e) { failed(e.what()); }
            const int rc = och_close(dev); dev = nullptr;
            if (rc != OCH_OK) failed(std::string("cleanup close: ") + och_last_error());
        }
    }
};
} // namespace
int main(int argc, char** argv) {
    try {
        Options options; bool mode_seen = false;
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") { usage(); return 0; }
            if (arg == "--rf-controls") { options.rf_controls = true; continue; }
            if (arg == "--rx" || arg == "--tx-zero" || arg == "--duplex-zero") {
                check(!mode_seen, "choose exactly one mode"); mode_seen = true;
                options.mode = arg == "--rx" ? Options::Rx : arg == "--tx-zero" ? Options::TxZero : Options::DuplexZero;
            } else {
                check(i + 1 < argc, "missing value for " + arg); const char* value = argv[++i];
                if (arg == "--serial") options.serial = value;
                else if (arg == "--sample-rate") options.rate = number(value, "sample rate", 1000000, 61440000);
                else if (arg == "--rounds") options.rounds = number(value, "rounds", 1, 50);
                else if (arg == "--timeout-ms") options.timeout = number(value, "timeout", 100, 60000);
                else if (arg == "--settle-ms") options.settle = number(value, "settle", 0, 5000);
                else throw std::runtime_error("unknown option " + arg);
            }
        }
        const auto mode = options.mode == Options::Rx ? "rx" : options.mode == Options::TxZero ? "tx_zero" : "duplex_zero";
        log("\"kind\":\"start\",\"mode\":" + quote(mode) + ",\"sample_rate\":" + std::to_string(options.rate) +
            ",\"rounds\":" + std::to_string(options.rounds) + ",\"rf_controls\":" + (options.rf_controls ? "true" : "false") +
            ",\"tx_attenuation_db\":89.75,\"tx_payload\":\"all_zero_packed12\"");
        Harness harness(options); harness.run(); return 0;
    } catch (const std::exception& e) {
        log("\"kind\":\"summary\",\"pass\":false,\"error\":" + quote(e.what())); return 1;
    }
}
