och_rgb_status rgb_status(Fixture& f)
{
    och_rgb_status s{}; s.struct_size = sizeof s;
    ok(och_get_rgb_status(&f.device, &s)); return s;
}
och_fan_status fan_status(Fixture& f)
{
    och_fan_status s{}; s.struct_size = sizeof s;
    ok(och_get_fan_status(&f.device, &s)); return s;
}
och_core_temperature core_temperature(Fixture& f)
{
    och_core_temperature s{}; s.struct_size = sizeof s;
    ok(och_get_core_temperature(&f.device, &s)); return s;
}
void board_defaults_and_switches()
{
    Fixture f;
    auto r = rgb_status(f); auto fan = fan_status(f); auto t = core_temperature(f);
    CHECK(r.enabled && r.brightness_pct == 25 && r.green == 64);
    CHECK(!fan.enabled && !fan.duty_permille && fan.frequency_hz == 25000);
    CHECK(t.valid && t.milli_celsius == 45000);
    ok(och_set_rgb_enabled(&f.device, 0)); CHECK(rgb_status(f).state == OCH_RGB_OFF);
    ok(och_set_fan_enabled(&f.device, 1)); CHECK(fan_status(f).duty_permille == 300);
    f.usb.clear_log();
    ok(och_set_rgb_enabled(&f.device, 0)); ok(och_set_fan_enabled(&f.device, 1));
    CHECK(!contains(f.usb.command_log(), "rgb_enable="));
    CHECK(!contains(f.usb.command_log(), "fan_enable="));
    ok(och_set_rgb_enabled(&f.device, 1)); ok(och_set_fan_enabled(&f.device, 0));
    CHECK(rgb_status(f).enabled && !fan_status(f).duty_permille);
}
void board_duplex_and_lease()
{
    Fixture f(true, true); auto* rx = f.create(OCH_RX); auto* tx = f.create(OCH_TX);
    const void* data = nullptr; size_t bytes = 0; och_metadata m{}; m.struct_size = sizeof m;
    ok(och_rx_acquire(rx, &data, &bytes, &m, 1000));
    const auto epoch = rx->epoch; const auto resets = f.usb.resets; f.usb.clear_log();
    for (unsigned enabled : {0U, 1U, 0U}) {
        ok(och_set_rgb_enabled(&f.device, enabled));
        ok(och_set_fan_enabled(&f.device, enabled));
        (void)core_temperature(f);
    }
    CHECK(rx->running && tx->running && rx->lease && rx->epoch == epoch);
    CHECK(f.device.configured && f.usb.resets == resets);
    CHECK(!contains(f.usb.command_log(), "usb_stop!"));
    CHECK(!contains(f.usb.command_log(), "tx_enable="));
    ok(och_rx_release(rx));
}
void board_invalid_and_unsupported()
{
    Fixture f; auto* rx = f.create(OCH_RX);
    CHECK(och_set_rgb_enabled(&f.device, 2) == OCH_INVALID);
    CHECK(och_set_fan_enabled(&f.device, UINT32_MAX) == OCH_INVALID);
    CHECK(och_get_core_temperature(nullptr, nullptr) == OCH_INVALID);
    och_rgb_status r{}; och_fan_status s{}; och_core_temperature t{};
    CHECK(och_get_rgb_status(&f.device, &r) == OCH_INVALID);
    CHECK(och_get_fan_status(&f.device, &s) == OCH_INVALID);
    CHECK(och_get_core_temperature(&f.device, &t) == OCH_INVALID);
    r.struct_size = sizeof r; s.struct_size = sizeof s; t.struct_size = sizeof t;
    f.usb.board_supported = false;
    CHECK(och_get_rgb_status(&f.device, &r) == OCH_UNSUPPORTED);
    CHECK(och_get_fan_status(&f.device, &s) == OCH_UNSUPPORTED);
    CHECK(och_get_core_temperature(&f.device, &t) == OCH_UNSUPPORTED);
    CHECK(och_set_rgb_enabled(&f.device, 0) == OCH_UNSUPPORTED);
    CHECK(och_set_fan_enabled(&f.device, 1) == OCH_UNSUPPORTED);
    CHECK(rx->running && f.device.configured);
}
void board_failures_preserve_rf()
{
    Fixture f(true, true); auto* rx = f.create(OCH_RX); auto* tx = f.create(OCH_TX);
    f.usb.fail_next("rgb_enable=");
    CHECK(och_set_rgb_enabled(&f.device, 0) == OCH_PROTOCOL);
    f.usb.fail_next("fan_enable=");
    CHECK(och_set_fan_enabled(&f.device, 1) == OCH_PROTOCOL);
    f.usb.ignore_board_write = true;
    CHECK(och_set_rgb_enabled(&f.device, 0) == OCH_PROTOCOL);
    CHECK(och_set_fan_enabled(&f.device, 1) == OCH_PROTOCOL);
    CHECK(rx->running && tx->running && f.device.configured);
    CHECK(!contains(f.usb.command_log(), "usb_stop!"));
}
void board_unknown_temperature_and_failsafe()
{
    Fixture f;
    f.usb.core_reply_override = "core_temp source=zynq valid=0 milli_celsius=unknown age_ms=4294967295 overtemp=0";
    CHECK(!core_temperature(f).valid);
    f.usb.fan_reply_override = "fan_status available=1 enabled=1 frequency_hz=25000 duty_permille=1000 target_permille=1000 boost=0 failsafe=1 temp_valid=0 temp_mC=unknown temp_age_ms=3000 overtemp=0 source=zynq";
    auto s = fan_status(f); CHECK(s.failsafe && !s.temperature_valid && s.duty_permille == 1000);
    const auto text = och_cli::board_view::fan(s);
    CHECK(text.find("temp_mC=unknown") != std::string::npos && text.find("duty_pct=100.0") != std::string::npos);
    CHECK(och_cli::board_view::temperature(core_temperature(f)).find("Celsius=unknown") != std::string::npos);
}
void board_strict_status_parsers()
{
    Fixture f; auto* rx = f.create(OCH_RX);
    const auto rgb = f.usb.execute("rgb_status?");
    const auto fan = f.usb.execute("fan_status?");
    const auto temp = f.usb.execute("core_temp?");
    const auto change = [](std::string text, const char* old, const char* value) {
        text.replace(text.find(old), std::string(old).size(), value); return text;
    };
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{
             {"enabled=1", "enabled=2"}, {"green=64", "green=255"}, {"state=idle", "state=unknown"},
             {"brightness_pct=25", "brightness_pct=100"}, {"blink_hz=0", "blink_hz=1"}}) {
        f.usb.rgb_reply_override = change(rgb, pair.first, pair.second);
        och_rgb_status s{}; s.struct_size = sizeof s;
        CHECK(och_get_rgb_status(&f.device, &s) == OCH_PROTOCOL);
    }
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{
             {"duty_permille=0", "duty_permille=1001"}, {"duty_permille=0", "duty_permille=300"},
             {"source=zynq", "source=ad9361"}, {"temp_valid=1", "temp_valid=0"},
             {"temp_age_ms=100", "temp_age_ms=2501"}, {"failsafe=0", "failsafe=1"},
             {"frequency_hz=25000", "frequency_hz=0"}}) {
        f.usb.fan_reply_override = change(fan, pair.first, pair.second);
        och_fan_status s{}; s.struct_size = sizeof s;
        CHECK(och_get_fan_status(&f.device, &s) == OCH_PROTOCOL);
    }
    for (const auto& pair : std::vector<std::pair<const char*, const char*>>{
             {"source=zynq", "source=ad9361"}, {"milli_celsius=45000", "milli_celsius=nan"},
             {"milli_celsius=45000", "milli_celsius=2147483648"}, {"age_ms=100", "age_ms=2501"},
             {"valid=1", "valid=0"}}) {
        f.usb.core_reply_override = change(temp, pair.first, pair.second);
        och_core_temperature s{}; s.struct_size = sizeof s;
        CHECK(och_get_core_temperature(&f.device, &s) == OCH_PROTOCOL);
    }
    CHECK(rx->running && f.device.configured);
}
void board_cli_and_abi()
{
    using namespace och_cli;
    static_assert(sizeof(och_rgb_status) == 64 && sizeof(och_fan_status) == 64 &&
                  sizeof(och_core_temperature) == 32, "board ABI layout changed");
    CHECK(commands::translate("rgb.enabled", {"off"}, commands::Operation::set) == "rgb_enable=0");
    CHECK(commands::translate("fan.enabled", {"on"}, commands::Operation::set) == "fan_enable=1");
    CHECK(commands::translate("rgb.status", {}, commands::Operation::query) == "rgb_status?");
    CHECK(commands::translate("fan.status", {}, commands::Operation::query) == "fan_status?");
    CHECK(commands::translate("core.temperature", {}, commands::Operation::query) == "core_temp?");
    Fixture f; CHECK(board_view::temperature(core_temperature(f)).find("Celsius=45.000") != std::string::npos);
    std::ostringstream out;
    presentation::reply("BOARD", board_view::fan(fan_status(f)), out);
    CHECK(out.str().find("zynq") != std::string::npos);
}
