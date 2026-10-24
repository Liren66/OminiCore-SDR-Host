#pragma once
#include "core/internal.hpp"

// Test transport only. No DLL is loaded, no USB handle is opened, and device.cpp
// is intentionally not linked. The public C API and actual workers remain real.
namespace test {
struct FakeUsb {
    struct Request {
        UCHAR endpoint;
        UCHAR* data;
        ULONG size;
        bool completed=false, cancelled=false;
        uint64_t sequence=0;
    };
    struct Write { size_t bytes; bool all_zero; };
    std::mutex mutex;
    std::map<OVERLAPPED*,Request> requests;
    std::map<std::pair<UCHAR,ULONG>,ULONG> policies;
    std::vector<std::string> commands;
    std::vector<Write> writes;
    std::string fail_command;
    std::mutex gate_mutex;
    std::condition_variable gate_cv;
    std::string paused_command;
    bool command_waiting=false,command_released=false;
    bool hold_rx=false, hold_tx=false, rx_enabled=false, tx_enabled=false;
    size_t rx_position=0, resets=0;
    uint64_t next_request=0;
    std::atomic<unsigned> header_completions{0};
    och_clock_config clock{};
    och_fir_config fir[2]{};
    bool clock_supported=true, fir_supported=true;
    bool iq_format_supported=true, iq8_supported=true, ignore_iq_format_write=false;
    unsigned iq_bits=12, rx_mask=3, tx_mask=0, tx_source=1;
    unsigned tx_main_fifo_groups=6000;
    std::string iq_format_reply_override;
    bool correction_supported=true, dc_correction=true, iq_correction=true;
    bool ignore_correction_write=false, corrupt_correction_partner=false;
    std::string correction_reply_override;
    bool board_supported=true, rgb_enabled=true, fan_enabled=false, ignore_board_write=false;
    std::string rgb_reply_override, fan_reply_override, core_reply_override;
    bool pps_supported=true, pps_enabled=false, ignore_pps_write=false;
    std::string pps_reply_override;
    bool lo_supported=true, reference_supported=true, reference_source_supported=true;
    unsigned lo_source[2]={0,0}, reference_source=0;
    uint64_t lo_frequency[2]={1000000000ULL,1000000000ULL}, internal_lo_frequency[2]={1000000000ULL,1000000000ULL};
    int rx_gain[2]={30,30};bool clamp_rx_gain_on_internal=false;
    bool ignore_lo_write=false, corrupt_lo_partner=false, ignore_reference_write=false, corrupt_reference_restore=false;
    bool reference_source_known=true, reference_gpio_verified=true, reference_write_leaves_unknown=false, reference_test_leaves_unknown=false;
    int32_t reference_last_error=0;
    std::string lo_reply_override, reference_reply_override, reference_test_override;
    unsigned uploading=0;
    explicit FakeUsb(och_device&);
    void complete_rx(); // mutex held
    std::vector<std::string> command_log();
    std::vector<Write> write_log();
    void clear_log();
    void freeze_rx(bool);
    void freeze_tx(bool);
    bool inject_header_only();
    bool inject_rx(const std::vector<uint8_t>&);
    void fail_next(const std::string&);
    void pause_on(const std::string&);
    bool wait_command(unsigned timeout_ms=1000);
    void release_command();
    std::string execute(const std::string&);
};
}
