#pragma once
#include <omnicore/och.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace och_cli::stream_assessment {
// Qualification policy: recovered RX overflows observed within the first second.
// All counters remain intact; this affects only the CLI's stability verdict.
inline constexpr double startup_seconds = 1.0;
// Stop only on repeated overflow onsets after the startup window. Recovery
// notifications describe the same fault and must not count as another overflow.
struct repeated_overflow_observer {
    double previous = -1;
    bool repeated = false;
    void observe(const och_event& e, double seconds) {
        if (!std::isfinite(seconds) || seconds <= startup_seconds || e.type != OCH_EVENT_DEVICE) return;
        const std::string message(e.message);
        if (message.find("type=rx_overflow ") == std::string::npos ||
            message.find("action=waiting_rx_resync ") == std::string::npos) return;
        if (previous >= 0 && seconds >= previous && seconds - previous <= 2.0) repeated = true;
        previous = seconds;
    }
};
struct startup_observer {
    uint64_t overflows = 0, recoveries = 0, resyncs = 0;
    void observe(const och_event& e, double seconds) {
        if (!std::isfinite(seconds) || seconds < 0 || seconds > startup_seconds) return;
        const std::string message(e.message);
        if (e.type == OCH_EVENT_DEVICE && message.find("type=rx_overflow ") != std::string::npos) {
            if (message.find("action=waiting_rx_resync ") != std::string::npos) ++overflows;
            if (message.find("action=rx_restarted ") != std::string::npos) ++recoveries;
        }
        if (e.type == OCH_EVENT_RX_SYNC && e.epoch > 1) ++resyncs;
    }
    uint64_t accepted(const och_stats& rx) const {
        return std::min({overflows, recoveries, resyncs, rx.device_overflows, rx.resyncs});
    }
};
inline uint64_t remaining_faults(const och_stats& s, bool rx, uint64_t accepted) {
    return s.transfer_errors + s.host_dropped_samples + s.device_underflows + s.event_drops +
        s.tx_starvation_events + s.tx_padding_samples +
        (s.device_overflows - std::min(s.device_overflows, accepted)) +
        (s.resyncs - (rx ? std::min(s.resyncs, accepted) : 0));
}
inline bool throughput_ok(const och_stats& s, unsigned channels, uint32_t rate, unsigned bytes) {
    const double expected = static_cast<double>(rate) * channels * bytes;
    return s.elapsed_ns > 0 && expected > 0 &&
        static_cast<double>(s.usb_bytes) / (s.elapsed_ns / 1e9) >= expected * 0.98;
}
} // namespace och_cli::stream_assessment
