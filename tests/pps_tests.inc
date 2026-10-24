och_pps_status pps(Fixture& f){och_pps_status c{};c.struct_size=sizeof c;ok(och_get_pps_status(&f.device,&c));return c;}
void pps_defaults_and_missing_signal(){
    Fixture f;auto c=pps(f);CHECK(!c.enabled&&c.state==OCH_PPS_DISABLED&&!c.frequency_valid&&c.error_ppm==0);
    ok(och_set_pps_enabled(&f.device,1));c=pps(f);CHECK(c.enabled&&c.state==OCH_PPS_NO_SIGNAL&&!c.signal_present&&!c.frequency_valid&&!c.measured_hz);
    f.usb.clear_log();ok(och_set_pps_enabled(&f.device,1));CHECK(!contains(f.usb.command_log(),"pps_input_en="));
    ok(och_set_pps_enabled(&f.device,0));CHECK(!pps(f).enabled);
}
void pps_duplex_and_lease_are_undisturbed(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);
    const void* data=nullptr;size_t bytes=0;och_metadata m{};m.struct_size=sizeof m;
    ok(och_rx_acquire(rx,&data,&bytes,&m,1000));const auto epoch=rx->epoch;const auto resets=f.usb.resets;f.usb.clear_log();
    ok(och_set_pps_enabled(&f.device,1));ok(och_set_pps_enabled(&f.device,0));
    CHECK(rx->running&&tx->running&&rx->lease&&rx->epoch==epoch&&f.device.configured&&f.usb.resets==resets);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));CHECK(!contains(f.usb.command_log(),"tx_enable="));
    ok(och_rx_release(rx));
}
void pps_invalid_and_legacy_firmware(){
    Fixture f;auto* rx=f.create(OCH_RX);CHECK(och_set_pps_enabled(&f.device,2)==OCH_INVALID);
    CHECK(och_get_pps_status(&f.device,nullptr)==OCH_INVALID);och_pps_status c{};
    CHECK(och_get_pps_status(&f.device,&c)==OCH_INVALID);c.struct_size=sizeof c;
    f.usb.pps_supported=false;CHECK(och_get_pps_status(&f.device,&c)==OCH_UNSUPPORTED);
    CHECK(och_set_pps_enabled(&f.device,1)==OCH_UNSUPPORTED);CHECK(rx->running&&f.device.configured);
    CHECK(!contains(f.usb.command_log(),"pps_input_en="));
}
void pps_errors_do_not_stop_rf(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.clear_log();
    f.usb.fail_next("pps_input_en=");CHECK(och_set_pps_enabled(&f.device,1)==OCH_PROTOCOL);
    f.usb.ignore_pps_write=true;CHECK(och_set_pps_enabled(&f.device,1)==OCH_PROTOCOL);
    CHECK(rx->running&&tx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
const std::string pps_valid_reply="available=1 enabled=1 state=valid signal_present=1 frequency_valid=1 nominal_hz=122880000 measured_hz=122881228 error_ppm=9.993490 pulse_count=3 invalid_count=0 age_ms=15 raw_counter=122881227 hardware_timeout=0 monitor_only=1 adjustment=0 clock=DATA_CLK expected_pps_hz=1";
void pps_valid_frequency(){
    Fixture f;f.usb.pps_reply_override=pps_valid_reply;const auto c=pps(f);
    CHECK(c.state==OCH_PPS_VALID&&c.frequency_valid&&c.measured_hz==122881228&&std::abs(c.error_ppm-9.993489583)<0.000001);
}
void pps_malformed_and_stale_data(){
    Fixture f;auto* rx=f.create(OCH_RX);
    const std::pair<std::string,std::string> bad[]={
        {"enabled=1","enabled=0"},{"state=valid","state=bad"},{"frequency_valid=1","frequency_valid=0"},
        {"signal_present=1","signal_present=0"},{"nominal_hz=122880000","nominal_hz=0"},
        {"age_ms=15","age_ms=1600"},{"raw_counter=122881227","raw_counter=122881228"},
        {"error_ppm=9.993490","error_ppm=nan"},{"error_ppm=9.993490","error_ppm=1junk"},
        {"adjustment=0","adjustment=1"},{"measured_hz=122881228","measured_hz=4294967296"}};
    for(const auto& change:bad){auto text=pps_valid_reply;text.replace(text.find(change.first),change.first.size(),change.second);
        f.usb.pps_reply_override=text;och_pps_status c{};c.struct_size=sizeof c;CHECK(och_get_pps_status(&f.device,&c)==OCH_PROTOCOL);}
    CHECK(rx->running&&f.device.configured);
}
void pps_cli_catalog(){
    const auto* enable=och_cli::commands::find("pps.input-enabled");const auto* status=och_cli::commands::find("pps.status");
    CHECK(enable&&status&&enable->supported&&status->supported);
    CHECK(enable->query=="pps_input_en?"&&enable->setter=="pps_input_en="&&status->query=="pps_status?");
    CHECK(!enable->shared_clock);
}
