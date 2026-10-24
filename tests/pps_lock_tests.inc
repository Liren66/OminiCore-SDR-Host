// Frequency valid is not the same as frequency locked: firmware accepts +/-10%.
std::string lock_measurement(uint32_t measured) {
    std::ostringstream out;
    out<<"available=1 enabled=1 state=valid signal_present=1 frequency_valid=1"
       <<" nominal_hz=100000000 measured_hz="<<measured<<" error_ppm="
       <<std::fixed<<std::setprecision(6)<<(double(measured)-100000000.0)/100.0
       <<" pulse_count=3 invalid_count=0 age_ms=15 raw_counter="<<measured-1
       <<" hardware_timeout=0 monitor_only=1 adjustment=0 clock=DATA_CLK expected_pps_hz=1";
    return out.str();
}
void pps_lock_boundaries() {
    static_assert(sizeof(och_pps_status)==96,"PPS ABI size changed");
    static_assert(offsetof(och_pps_status,error_ppm)==56,"PPS ABI offset changed");
    static_assert(sizeof(och_reference_status)==96,"reference ABI size changed");
    Fixture f;
    for(const auto& value:std::vector<std::pair<uint32_t,int>>{
            {100000000,1},{100000099,1},{100000100,1},{99999900,1},
            {100000101,0},{99999899,0},{100001000,0}}) {
        f.usb.pps_reply_override=lock_measurement(value.first);
        const auto p=pps(f);const auto r=reference(f);
        CHECK(p.lock_known==1&&p.locked==value.second);
        CHECK(r.lock_known==1&&r.locked==p.locked);
        CHECK(!r.chip_verified&&!r.led_known&&r.led_on==-1);
    }
}
void pps_lock_unavailable() {
    Fixture f;CHECK(!pps(f).lock_known&&pps(f).locked==-1);
    ok(och_set_pps_enabled(&f.device,1));CHECK(!pps(f).lock_known&&pps(f).locked==-1);
    for(const auto& value:std::vector<std::pair<const char*,unsigned>>{
            {"waiting",1},{"invalid_interval",1},{"no_signal",0},{"clock_unavailable",0}}) {
        f.usb.pps_reply_override=std::string("available=1 enabled=1 state=")+value.first+
            " signal_present="+std::to_string(value.second)+" frequency_valid=0 nominal_hz="+
            (std::string(value.first)=="clock_unavailable"?"0":"100000000")+
            " measured_hz=0 error_ppm=unknown pulse_count=3 invalid_count=1 age_ms=15 raw_counter=0 hardware_timeout=0 monitor_only=1 adjustment=0 clock=DATA_CLK expected_pps_hz=1";
        CHECK(!pps(f).lock_known&&pps(f).locked==-1);
        CHECK(!reference(f).lock_known&&reference(f).locked==-1);
    }
    auto text=lock_measurement(100000000);
    text.replace(text.find("hardware_timeout=0"),18,"hardware_timeout=1");
    f.usb.pps_reply_override=text;CHECK(!pps(f).lock_known&&pps(f).locked==-1);
    text=lock_measurement(100000000);text.replace(text.find("age_ms=15"),9,"age_ms=1600");
    f.usb.pps_reply_override=text;
    CHECK(!reference(f).lock_known&&reference(f).locked==-1);
}
void pps_lock_reference_independence() {
    Fixture f;
    f.usb.pps_supported=false;
    ok(och_set_reference_source(&f.device,OCH_SOURCE_EXTERNAL));
    auto r=reference(f);CHECK(r.source_known&&r.source==OCH_SOURCE_EXTERNAL&&!r.lock_known&&r.locked==-1);
    f.usb.pps_supported=true;f.usb.pps_reply_override="malformed PPS";
    ok(och_set_reference_source(&f.device,OCH_SOURCE_INTERNAL));
    f.usb.clear_log();ok(och_set_reference_source(&f.device,OCH_SOURCE_EXTERNAL));
    CHECK(!contains(f.usb.command_log(),"pps_status?"));
    r=reference(f);CHECK(r.source_known&&r.source==OCH_SOURCE_EXTERNAL&&!r.lock_known&&r.locked==-1);
    och_pps_status p{};p.struct_size=sizeof p;CHECK(och_get_pps_status(&f.device,&p)==OCH_PROTOCOL);
}
void pps_lock_streams() {
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);
    const void* data=nullptr;size_t bytes=0;och_metadata metadata{};metadata.struct_size=sizeof metadata;
    ok(och_rx_acquire(rx,&data,&bytes,&metadata,1000));const auto epoch=rx->epoch;
    f.usb.clear_log();f.usb.pps_reply_override=lock_measurement(100000000);
    CHECK(reference(f).locked==1&&pps(f).locked==1);
    CHECK(rx->running&&tx->running&&rx->lease&&rx->epoch==epoch&&f.device.configured);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));ok(och_rx_release(rx));
}
void pps_lock_cli_view() {
    using namespace och_cli;
    Fixture f;
    const std::string raw="rf_ready=1\nadf4002_status source=1 source_known=1 lock_known=0 locked=-1 led_known=0 led_on=-1 last_error=0\n";
    for(const auto* command:{"clock_ref_source?","clock_ref_source=1","adf4002_status?","pps_input_en=1","pps_status?","system_status?"})
        CHECK(monitor_view::applies(command));
    CHECK(!monitor_view::applies("adf4002_self_test!"));CHECK(!monitor_view::applies("rx_lo_freq?"));
    auto p=pps(f);
    for(const auto* status:std::vector<const och_pps_status*>{nullptr,&p}) {
        const auto view=monitor_view::reply(raw,status);
        CHECK(view.find("lock")==std::string::npos&&view.find("led_on")==std::string::npos);
        CHECK(view.find("source=1 source_known=1")!=std::string::npos);
        std::ostringstream human;presentation::reply("REFERENCE",view,human);
        CHECK(human.str().find("locked")==std::string::npos);
    }
    f.usb.pps_reply_override=lock_measurement(100000000);p=pps(f);
    auto view=monitor_view::reply(raw+"pps_status stale=1\n",&p);
    CHECK(view.find("locked=1 lock_source=PPS lock_tolerance_ppm=1")!=std::string::npos);
    CHECK(view.find("measured_hz=100000000")!=std::string::npos);
    CHECK(view.find("stale=")==std::string::npos&&view.find("locked=-1")==std::string::npos);
    CHECK(view.find("locked=")==view.rfind("locked="));
    f.usb.pps_reply_override=lock_measurement(100000101);p=pps(f);
    view=monitor_view::reply(raw,&p);CHECK(view.find("locked=0 lock_source=PPS")!=std::string::npos);
    const auto canonical=commands::canonical_reply("clock.ref-source",monitor_view::reply(
        "clock_ref_source=1 reference_hz=10000000 lock_known=0 locked=-1\n",nullptr));
    CHECK(canonical.find("clock.ref-source=1")!=std::string::npos&&canonical.find("locked")==std::string::npos);
}
