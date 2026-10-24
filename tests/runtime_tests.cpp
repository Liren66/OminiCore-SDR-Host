#include "fake_device.hpp"
#include "cli/commands.hpp"
#include "cli/monitor_view.hpp"
#include "cli/presentation.hpp"
#include "cli/board_view.hpp"
#include <iostream>
#include <limits>

namespace {
constexpr size_t block_bytes=49152;
void check(bool condition,const char* expression,int line){if(!condition)throw std::runtime_error(std::string("line ")+std::to_string(line)+": "+expression);}
#define CHECK(value) check(bool(value),#value,__LINE__)
void ok(int rc){if(rc!=OCH_OK)throw std::runtime_error("API result "+std::to_string(rc)+": "+och_last_error());}
template<class F> void eventually(F predicate,unsigned ms=1000){
    const auto deadline=och::core::now_ns()+uint64_t(ms)*1000000;
    while(!predicate()){if(och::core::now_ns()>=deadline)throw std::runtime_error("condition did not become true");Sleep(1);}
}
struct Fixture {
    och_device device;
    test::FakeUsb usb{device};
    och_config config{};
    explicit Fixture(bool rx=true,bool tx=false){och_default_config(&config);config.rx_mask=rx?1:0;config.tx_mask=tx?1:0;config.rx_agc=0;ok(och_configure(&device,&config));usb.clear_log();}
    ~Fixture(){if(device.rx){if(device.rx->lease)och_rx_release(device.rx);och_destroy_stream(device.rx);}if(device.tx)och_destroy_stream(device.tx);}
    och_stream* create(och_direction direction,bool start=true,unsigned host_blocks=8,uint32_t format=OCH_PACKED12){
        och_stream_args args{};och_default_stream_args(&args,direction);args.format=format;args.transfer_bytes=block_bytes;args.usb_queue_depth=2;args.host_buffer_bytes=static_cast<uint32_t>(block_bytes*host_blocks);
        och_stream* s=nullptr;ok(och_create_stream(&device,&args,&s));
        if(start){if(direction==OCH_TX)prefill(s);ok(och_start(s));}return s;
    }
    void prefill(och_stream* s){std::vector<uint8_t> data(block_bytes*4,0x66);size_t accepted=0;ok(och_send(s,data.data(),data.size()/s->group,&accepted,100));CHECK(accepted==data.size()/s->group);}
    och_metadata receive(och_stream* s){std::vector<uint8_t> data(block_bytes);size_t count=0;och_metadata m{};m.struct_size=sizeof m;ok(och_recv(s,data.data(),data.size()/s->group,&count,&m,1000));CHECK(count>0);return m;}
    void change(){config.rx_frequency+=1000000;ok(och_configure(&device,&config));}
};
bool contains(const std::vector<std::string>& commands,const std::string& prefix){return std::any_of(commands.begin(),commands.end(),[&](const auto& s){return s.rfind(prefix,0)==0;});}
size_t occurrences(const std::vector<std::string>& commands,const std::string& text){return std::count(commands.begin(),commands.end(),text);}
struct Cleanup {std::function<void()> callback;~Cleanup(){callback();}};
std::vector<uint8_t> rx_packet(unsigned headers,size_t samples){
    const auto header_bytes=och::dsp::Framer::header_bytes;
    std::vector<uint8_t> bytes(headers*header_bytes+samples*3,0x55);
    for(size_t i=0;i<headers*header_bytes;++i)bytes[i]=static_cast<uint8_t>(i);
    return bytes;
}

void rx_restart_and_epoch(){
    Fixture f;auto* rx=f.create(OCH_RX);auto* slot_memory=rx->slots.front()->bytes.get();auto initial=f.receive(rx);CHECK(initial.epoch>0);CHECK(initial.flags&OCH_META_START);
    for(unsigned i=0;i<3;++i){f.change();CHECK(f.device.rx==rx);CHECK(rx->running);auto after=f.receive(rx);
        CHECK(after.epoch>initial.epoch);CHECK(after.flags&OCH_META_START);CHECK(after.flags&OCH_META_DISCONTINUITY);CHECK(after.flags&OCH_META_RECONFIGURED);
        CHECK(!(after.flags&OCH_META_DEVICE_OVERFLOW));CHECK(after.device_dropped_samples==-1);CHECK(rx->slots.front()->bytes.get()==slot_memory);initial=after;
    }
    const auto commands=f.usb.command_log();CHECK(occurrences(commands,"usb_stop!")==3);
    CHECK(contains(commands,"rx_lo_freq="));CHECK(!contains(commands,"clock_sample_rate="));CHECK(!contains(commands,"tx_lo_freq="));
    unsigned notices=0;och_event e{};e.struct_size=sizeof e;while(och_poll_event(&f.device,&e,0)==OCH_OK)if(e.type==OCH_EVENT_RECONFIGURED)++notices;
    CHECK(notices==3);
}
void header_only_epoch_is_preserved(){
    Fixture f;f.usb.freeze_rx(true);auto* rx=f.create(OCH_RX);CHECK(f.usb.inject_header_only());
    eventually([&]{return f.usb.header_completions.load()==1;});f.change();f.usb.freeze_rx(false);
    const auto first_iq=f.receive(rx);CHECK(first_iq.epoch==2);CHECK(first_iq.flags&OCH_META_RECONFIGURED);
    och_stats stats{};stats.struct_size=sizeof stats;ok(och_get_stats(rx,&stats));CHECK(stats.headers==2);CHECK(stats.resyncs==1);
}
void adjacent_headers_count_every_epoch(){
    Fixture f;f.usb.freeze_rx(true);auto* rx=f.create(OCH_RX);CHECK(f.usb.inject_rx(rx_packet(2,10)));
    const auto m=f.receive(rx);CHECK(m.epoch==2);och_stats stats{};stats.struct_size=sizeof stats;ok(och_get_stats(rx,&stats));
    CHECK(stats.headers==2);CHECK(stats.resyncs==1);f.change();f.usb.freeze_rx(false);CHECK(f.receive(rx).epoch==3);
    ok(och_get_stats(rx,&stats));CHECK(stats.headers==3);CHECK(stats.resyncs==2);
}
void tx_restart_discards_old_iq(){
    Fixture f(false,true);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX);eventually([&]{return f.usb.write_log().size()>=2;});
    // With two pending USB writes, old nonzero samples remain in the host ring.
    CHECK(tx->ring.queued()>0);f.usb.clear_log();f.config.tx_frequency+=1000000;ok(och_configure(&f.device,&f.config));
    CHECK(f.device.tx==tx);CHECK(tx->running);eventually([&]{return f.usb.write_log().size()>=2;});
    auto writes=f.usb.write_log();CHECK(std::all_of(writes.begin(),writes.end(),[](auto w){return w.all_zero;}));
    och_stats stats{};stats.struct_size=sizeof stats;ok(och_get_stats(tx,&stats));CHECK(stats.tx_padding_samples==4*block_bytes/3);
    f.usb.freeze_tx(false);eventually([&]{return f.usb.write_log().size()==4;});
    writes=f.usb.write_log();CHECK(std::all_of(writes.begin(),writes.end(),[](auto w){return w.all_zero;}));
    std::vector<uint8_t> fresh(block_bytes,0x77);size_t accepted=0;ok(och_send(tx,fresh.data(),fresh.size()/3,&accepted,100));
    eventually([&]{return f.usb.write_log().size()==5;});CHECK(!f.usb.write_log().back().all_zero);
}
void duplex_restart(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);const auto before=f.receive(rx);f.change();
    CHECK(rx->running&&tx->running);CHECK(f.device.rx_on&&f.device.tx_on);CHECK(f.receive(rx).epoch>before.epoch);
    const auto commands=f.usb.command_log();CHECK(contains(commands,"tx_enable=1"));CHECK(contains(commands,"usb_data_en=1 "));
}
void idle_and_stopped_streams_stay_idle(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX,false);auto* tx=f.create(OCH_TX,false);f.change();
    CHECK(!rx->running&&!tx->running);CHECK(!f.device.rx_on&&!f.device.tx_on);CHECK(!rx->ever_started&&!tx->ever_started);
    ok(och_start(rx));ok(och_stop(rx,100));f.change();CHECK(!rx->running);CHECK(!tx->running);CHECK(!f.device.rx_on&&!f.device.tx_on);
}
void inactive_partner_does_not_start(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX,false);f.change();CHECK(rx->running&&!tx->running);CHECK(!tx->ever_started);CHECK(!f.device.tx_on);
}
void unchanged_and_invalid_do_not_interrupt(){
    Fixture f;auto* rx=f.create(OCH_RX);const auto first=f.receive(rx);f.usb.clear_log();const auto resets=f.usb.resets;
    ok(och_configure(&f.device,&f.config));CHECK(f.usb.command_log().empty());CHECK(f.usb.resets==resets);
    ok(och_set_sample_rate(&f.device,f.config.sample_rate));CHECK(f.usb.command_log().empty());
    auto invalid=f.config;invalid.sample_rate=0;CHECK(och_configure(&f.device,&invalid)==OCH_INVALID);
    invalid=f.config;invalid.rx_gain[0]=std::numeric_limits<double>::quiet_NaN();CHECK(och_configure(&f.device,&invalid)==OCH_INVALID);
    invalid=f.config;invalid.rx_mask=3;CHECK(och_configure(&f.device,&invalid)==OCH_BUSY);
    CHECK(f.usb.command_log().empty());CHECK(f.usb.resets==resets);CHECK(rx->running);CHECK(f.receive(rx).epoch==first.epoch);
}
void outstanding_lease_prevents_mutation(){
    Fixture f;auto* rx=f.create(OCH_RX);const void* bytes=nullptr;size_t size=0;och_metadata m{};m.struct_size=sizeof m;
    ok(och_rx_acquire(rx,&bytes,&size,&m,1000));CHECK(bytes&&size);const auto saved=*static_cast<const uint8_t*>(bytes);f.usb.clear_log();
    ok(och_configure(&f.device,&f.config));CHECK(f.usb.command_log().empty());
    f.config.rx_frequency+=1000000;CHECK(och_configure(&f.device,&f.config)==OCH_BUSY);
    const auto commands=f.usb.command_log();CHECK(std::all_of(commands.begin(),commands.end(),[](const auto& command){return !command.empty()&&command.back()=='?';}));CHECK(rx->running);
    CHECK(*static_cast<const uint8_t*>(bytes)==saved);ok(och_rx_release(rx));ok(och_configure(&f.device,&f.config));CHECK(rx->running);
}
void long_receive_releases_for_reconfigure(){
    Fixture f;f.usb.freeze_rx(true);auto* rx=f.create(OCH_RX);std::promise<void> entered;
    auto pending=std::async(std::launch::async,[&]{std::vector<uint8_t> data(block_bytes);size_t count=0;och_metadata m{};m.struct_size=sizeof m;entered.set_value();return och_recv(rx,data.data(),data.size()/3,&count,&m,60000);});
    entered.get_future().wait();Sleep(20);const auto began=och::core::now_ns();f.change();CHECK(och::core::now_ns()-began<1500000000ULL);
    CHECK(pending.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(pending.get()==OCH_TIMEOUT);
    CHECK(rx->running);f.usb.freeze_rx(false);f.receive(rx);
}
void long_send_releases_for_reconfigure(){
    Fixture f(false,true);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX);std::promise<void> entered;
    auto pending=std::async(std::launch::async,[&]{std::vector<uint8_t> data(block_bytes*32,0x66);size_t accepted=0;entered.set_value();return och_send(tx,data.data(),data.size()/3,&accepted,60000);});
    entered.get_future().wait();Sleep(20);const auto began=och::core::now_ns();f.config.tx_frequency+=1000000;ok(och_configure(&f.device,&f.config));
    CHECK(och::core::now_ns()-began<1500000000ULL);CHECK(pending.wait_for(std::chrono::seconds(1))==std::future_status::ready);
    const auto result=pending.get();CHECK(result==OCH_OK||result==OCH_TIMEOUT);CHECK(tx->running);
}
void long_flush_releases_for_reconfigure(){
    Fixture f(false,true);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX);std::promise<void> entered;
    auto pending=std::async(std::launch::async,[&]{entered.set_value();return och_flush(tx,60000);});
    entered.get_future().wait();Sleep(20);const auto began=och::core::now_ns();f.config.tx_frequency+=1000000;ok(och_configure(&f.device,&f.config));
    CHECK(och::core::now_ns()-began<1500000000ULL);CHECK(pending.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(pending.get()==OCH_TIMEOUT);CHECK(tx->running);
}
void producer_can_refill_during_hardware_resume(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX);f.usb.clear_log();
    f.usb.pause_on("usb_data_en=1 0");f.config.rx_frequency+=1000000;
    std::future<int> update;
    Cleanup cleanup{[&]{f.usb.release_command();f.usb.freeze_tx(false);}};
    update=std::async(std::launch::async,[&]{return och_configure(&f.device,&f.config);});
    CHECK(f.usb.wait_command());CHECK(update.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
    // Configuration has committed, but RX's firmware acknowledgement is held.
    // The real send API must already accept new samples after the four zeros.
    std::vector<uint8_t> fresh(block_bytes*2,0x77);size_t accepted=0;
    ok(och_send(tx,fresh.data(),fresh.size()/3,&accepted,100));CHECK(accepted==fresh.size()/3);
    CHECK(!tx->running);CHECK(tx->ring.queued()==block_bytes*6);f.usb.release_command();
    CHECK(update.wait_for(std::chrono::seconds(1))==std::future_status::ready);ok(update.get());CHECK(rx->running&&tx->running);
    f.usb.freeze_tx(false);eventually([&]{return f.usb.write_log().size()==6;});const auto writes=f.usb.write_log();
    for(size_t i=0;i<4;++i)CHECK(writes[i].all_zero);
    CHECK(!writes[4].all_zero&&!writes[5].all_zero);
}
void handoff_failure_interrupts_producer_before_clearing(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX);
    f.usb.pause_on("usb_data_en=1 0");f.usb.fail_next("usb_data_en=1 0");f.config.rx_frequency+=1000000;
    std::future<int> update,producer;
    Cleanup cleanup{[&]{f.usb.release_command();f.usb.freeze_tx(false);}};
    update=std::async(std::launch::async,[&]{return och_configure(&f.device,&f.config);});CHECK(f.usb.wait_command());
    producer=std::async(std::launch::async,[&]{std::vector<uint8_t> fresh(block_bytes*32,0x77);size_t accepted=0;return och_send(tx,fresh.data(),fresh.size()/3,&accepted,60000);});
    eventually([&]{return tx->ring.queued()==block_bytes*8;});CHECK(producer.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
    f.usb.release_command();CHECK(update.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(update.get()==OCH_PROTOCOL);
    CHECK(producer.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(producer.get()==OCH_OK);
    CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(rx->ring.queued()==0&&tx->ring.queued()==0);
    std::vector<uint8_t> fresh(block_bytes,0x77);size_t accepted=99;CHECK(och_send(tx,fresh.data(),fresh.size()/3,&accepted,100)==OCH_STOPPED);CHECK(accepted==0);
}
void minimum_ring_handoff_failure_interrupts_empty_send(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);f.usb.freeze_tx(true);auto* tx=f.create(OCH_TX,true,4);
    // RX resumes successfully. Fail the following TX enable-for-prefill command,
    // before its worker can consume any of the four zero blocks in a full ring.
    f.usb.pause_on("usb_data_en=1 1");f.usb.fail_next("usb_data_en=1 1");f.config.rx_frequency+=1000000;
    std::future<int> update,producer;std::promise<void> entered;
    Cleanup cleanup{[&]{f.usb.release_command();f.usb.freeze_tx(false);}};
    update=std::async(std::launch::async,[&]{return och_configure(&f.device,&f.config);});CHECK(f.usb.wait_command());
    producer=std::async(std::launch::async,[&]{std::vector<uint8_t> fresh(block_bytes,0x77);size_t accepted=0;entered.set_value();const auto rc=och_send(tx,fresh.data(),fresh.size()/3,&accepted,60000);CHECK(accepted==0);return rc;});
    entered.get_future().wait();Sleep(20);CHECK(tx->ring.queued()==block_bytes*4);CHECK(producer.wait_for(std::chrono::milliseconds(0))==std::future_status::timeout);
    f.usb.release_command();CHECK(update.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(update.get()==OCH_PROTOCOL);
    CHECK(producer.wait_for(std::chrono::seconds(1))==std::future_status::ready);CHECK(producer.get()==OCH_TIMEOUT);
    CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(rx->ring.queued()==0&&tx->ring.queued()==0);
}
void failure_stops_both_directions(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.clear_log();f.usb.fail_next("rx_lo_freq=");f.config.rx_frequency+=1000000;
    CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(!f.device.rx_on&&!f.device.tx_on);
    const auto commands=f.usb.command_log();CHECK(!contains(commands,"tx_enable=1"));CHECK(!contains(commands,"usb_data_en=1"));
    const auto start_result=och_start(rx);CHECK(start_result==OCH_INVALID||start_result==OCH_BUSY);
    ok(och_configure(&f.device,&f.config));CHECK(f.device.configured);CHECK(!rx->running&&!tx->running);
}
void resume_failure_stops_both_directions(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.clear_log();f.usb.fail_next("tx_enable=1");f.config.rx_frequency+=1000000;
    CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(!f.device.rx_on&&!f.device.tx_on);
    CHECK(contains(f.usb.command_log(),"usb_data_en=1 0"));
}
void rx_resume_failure_prevents_tx_restart(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.clear_log();f.usb.fail_next("usb_data_en=1 0");f.config.rx_frequency+=1000000;
    CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);CHECK(!contains(f.usb.command_log(),"tx_enable=1"));
    CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(!f.device.rx_on&&!f.device.tx_on);
}
void failed_reconfigure_accounts_unread_rx_samples(){
    Fixture f;f.usb.freeze_rx(true);auto* rx=f.create(OCH_RX);CHECK(f.usb.inject_rx(rx_packet(1,30)));
    uint8_t sample[3]{};size_t count=0;och_metadata meta{};meta.struct_size=sizeof meta;ok(och_recv(rx,sample,1,&count,&meta,1000));CHECK(count==1);
    och_stats before{};before.struct_size=sizeof before;ok(och_get_stats(rx,&before));CHECK(before.host_dropped_samples==0);
    f.usb.fail_next("rx_lo_freq=");f.config.rx_frequency+=1000000;CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);
    och_stats after{};after.struct_size=sizeof after;ok(och_get_stats(rx,&after));CHECK(after.host_dropped_samples==29);CHECK(after.host_queue_bytes==0);
    count=99;CHECK(och_recv(rx,sample,1,&count,&meta,0)==OCH_IO);CHECK(count==0);
}
void typed_fir_failure_stops_both_directions(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);och_fir_config fir{};fir.struct_size=sizeof fir;
    ok(och_get_fir(&f.device,OCH_RX,&fir));fir.coefficients[0]=123;f.usb.clear_log();f.usb.fail_next("fir_commit=");
    CHECK(och_set_fir(&f.device,&fir)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);CHECK(!f.device.rx_on&&!f.device.tx_on);
    const auto commands=f.usb.command_log();CHECK(contains(commands,"fir_abort="));CHECK(!contains(commands,"tx_enable=1"));
}
void typed_clock_and_fir_setters_resume(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);auto previous=f.receive(rx);
    ok(och_set_sample_rate(&f.device,15360000));CHECK(rx->running&&tx->running);CHECK(f.device.config.sample_rate==15360000);CHECK(f.receive(rx).epoch>previous.epoch);
    och_clock_config clock{};clock.struct_size=sizeof clock;ok(och_get_clock(&f.device,&clock));clock.rx_hz[5]=clock.tx_hz[5]=7680000;
    ok(och_set_clock(&f.device,&clock));CHECK(rx->running&&tx->running);CHECK(f.device.config.sample_rate==7680000);
    och_fir_config fir{};fir.struct_size=sizeof fir;ok(och_get_fir(&f.device,OCH_RX,&fir));fir.coefficients[0]=123;fir.enabled=1;
    ok(och_set_fir(&f.device,&fir));CHECK(rx->running&&tx->running);ok(och_set_fir_enabled(&f.device,0,1));CHECK(rx->running&&tx->running);
}
och_rx_correction_config corrections(Fixture& f){
    och_rx_correction_config c{};c.struct_size=sizeof c;ok(och_get_rx_correction(&f.device,&c));return c;
}
void correction_idle_and_partner_preservation(){
    Fixture f;auto c=corrections(f);CHECK(c.dc_enabled==1&&c.iq_enabled==1);
    ok(och_set_rx_dc_correction(&f.device,0));c=corrections(f);CHECK(c.dc_enabled==0&&c.iq_enabled==1);
    ok(och_set_rx_iq_correction(&f.device,0));c=corrections(f);CHECK(c.dc_enabled==0&&c.iq_enabled==0);
    ok(och_set_rx_dc_correction(&f.device,1));c=corrections(f);CHECK(c.dc_enabled==1&&c.iq_enabled==0);
    CHECK(f.device.configured);CHECK(!f.device.rx_on&&!f.device.tx_on);
}
void correction_duplex_resume_and_epoch(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);const auto before=f.receive(rx);
    ok(och_set_rx_dc_correction(&f.device,0));CHECK(f.device.rx==rx&&f.device.tx==tx);CHECK(rx->running&&tx->running);
    const auto after=f.receive(rx);CHECK(after.epoch>before.epoch);CHECK(after.flags&OCH_META_RECONFIGURED);
    ok(och_set_rx_iq_correction(&f.device,0));CHECK(rx->running&&tx->running);CHECK(f.receive(rx).epoch>after.epoch);
    CHECK(occurrences(f.usb.command_log(),"usb_stop!")==2);const auto c=corrections(f);CHECK(!c.dc_enabled&&!c.iq_enabled);
}
void correction_noop_and_invalid_leave_stream_running(){
    Fixture f;auto* rx=f.create(OCH_RX);const auto epoch=f.receive(rx).epoch;const auto resets=f.usb.resets;
    ok(och_set_rx_dc_correction(&f.device,1));ok(och_set_rx_iq_correction(&f.device,1));
    CHECK(och_set_rx_dc_correction(&f.device,2)==OCH_INVALID);CHECK(och_set_rx_iq_correction(&f.device,UINT32_MAX)==OCH_INVALID);
    och_rx_correction_config invalid{};CHECK(och_get_rx_correction(&f.device,&invalid)==OCH_INVALID);
    CHECK(och_get_rx_correction(&f.device,nullptr)==OCH_INVALID);CHECK(och_set_rx_dc_correction(nullptr,1)==OCH_INVALID);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));CHECK(!contains(f.usb.command_log(),"rx_dc_correction="));
    CHECK(f.usb.resets==resets&&rx->running);CHECK(f.receive(rx).epoch==epoch);
}
void correction_legacy_firmware_preserves_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.correction_supported=false;f.usb.clear_log();
    och_rx_correction_config c{};c.struct_size=sizeof c;
    CHECK(och_get_rx_correction(&f.device,&c)==OCH_UNSUPPORTED);
    CHECK(och_set_rx_dc_correction(&f.device,0)==OCH_UNSUPPORTED);CHECK(och_set_rx_iq_correction(&f.device,0)==OCH_UNSUPPORTED);
    CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
    CHECK(!contains(f.usb.command_log(),"rx_dc_correction"));CHECK(!contains(f.usb.command_log(),"rx_iq_correction"));
}
void correction_lease_blocks_write(){
    Fixture f;auto* rx=f.create(OCH_RX);const void* data=nullptr;size_t count=0;och_metadata m{};m.struct_size=sizeof m;
    ok(och_rx_acquire(rx,&data,&count,&m,1000));CHECK(data&&count);f.usb.clear_log();
    CHECK(och_set_rx_dc_correction(&f.device,0)==OCH_BUSY);CHECK(rx->running);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));CHECK(!contains(f.usb.command_log(),"rx_dc_correction="));
    ok(och_rx_release(rx));ok(och_set_rx_dc_correction(&f.device,0));CHECK(rx->running);
}
void correction_stopped_partner_stays_stopped(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX,false);
    ok(och_set_rx_iq_correction(&f.device,0));CHECK(rx->running&&!tx->running);CHECK(!tx->ever_started&&!f.device.tx_on);
}
void correction_write_failure_stops_both(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.clear_log();f.usb.fail_next("rx_dc_correction=");
    CHECK(och_set_rx_dc_correction(&f.device,0)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);
    CHECK(!contains(f.usb.command_log(),"tx_enable=1"));CHECK(!f.device.rx_on&&!f.device.tx_on);
}
void correction_readback_mismatch_stops_both(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.ignore_correction_write=true;
    CHECK(och_set_rx_iq_correction(&f.device,0)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running);CHECK(!f.device.configured);
}
void correction_partner_corruption_is_detected(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.corrupt_correction_partner=true;
    CHECK(och_set_rx_dc_correction(&f.device,0)==OCH_PROTOCOL);CHECK(!rx->running);CHECK(!f.device.configured);
}
void correction_malformed_reply_does_not_stop_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.clear_log();f.usb.correction_reply_override="rx_dc_correction=2 rx_iq_correction=1";
    och_rx_correction_config c{};c.struct_size=sizeof c;CHECK(och_get_rx_correction(&f.device,&c)==OCH_PROTOCOL);
    CHECK(och_set_rx_dc_correction(&f.device,0)==OCH_PROTOCOL);CHECK(rx->running&&f.device.configured);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void correction_cli_catalog(){
    using namespace och_cli::commands;
    const auto* dc=find("rx.dc-correction");const auto* iq=find("rx.iq-correction");CHECK(dc&&iq);
    CHECK(dc->query=="rx_dc_correction?"&&dc->setter=="rx_dc_correction=");
    CHECK(iq->query=="rx_iq_correction?"&&iq->setter=="rx_iq_correction=");
    CHECK(dc->set_args.size()==1&&iq->set_args.size()==1);
    CHECK(dc->set_args[0].minimum==0&&dc->set_args[0].maximum==1&&dc->set_args[0].step==1);
}
void unsupported_clock_and_fir_preserve_duplex(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);
    auto clock=f.usb.clock;auto fir=f.usb.fir[0];clock.rx_hz[5]=clock.tx_hz[5]=15360000;fir.coefficients[0]=123;
    f.usb.clock_supported=f.usb.fir_supported=false;f.usb.clear_log();const auto resets=f.usb.resets;
    CHECK(och_set_clock(&f.device,&clock)==OCH_UNSUPPORTED);
    CHECK(och_set_sample_rate(&f.device,15360000)==OCH_UNSUPPORTED);
    CHECK(och_set_fir(&f.device,&fir)==OCH_UNSUPPORTED);
    CHECK(och_set_fir_enabled(&f.device,1,0)==OCH_UNSUPPORTED);
    CHECK(rx->running&&tx->running&&f.device.configured);CHECK(f.usb.resets==resets);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void fixed_reference_clock_rejection_preserves_duplex(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);auto clock=f.usb.clock;clock.reference_hz+=1000000;
    f.usb.clear_log();CHECK(och_set_clock(&f.device,&clock)==OCH_UNSUPPORTED);
    CHECK(rx->running&&tx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
och_lo_source_config sources(Fixture& f){och_lo_source_config c{};c.struct_size=sizeof c;ok(och_get_lo_source(&f.device,&c));return c;}
och_reference_status reference(Fixture& f){och_reference_status c{};c.struct_size=sizeof c;ok(och_get_reference_status(&f.device,&c));return c;}
void lo_source_duplex_and_frequency_cache(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);const auto before=f.receive(rx);
    ok(och_set_rx_lo_source(&f.device,1));CHECK(rx->running&&tx->running);auto c=sources(f);CHECK(c.rx_source==1&&c.tx_source==0);
    const auto after=f.receive(rx);CHECK(after.epoch>before.epoch);CHECK(after.flags&OCH_META_RECONFIGURED);CHECK(after.flags&OCH_META_DISCONTINUITY);
    f.config.rx_frequency=2400000000ULL;ok(och_configure(&f.device,&f.config));CHECK(sources(f).rx_frequency==2400000000ULL);
    CHECK(f.usb.internal_lo_frequency[0]==1000000000ULL);
    ok(och_set_rx_lo_source(&f.device,0));och_config actual{};actual.struct_size=sizeof actual;ok(och_get_config(&f.device,&actual));
    CHECK(actual.rx_frequency==1000000000ULL&&sources(f).rx_frequency==1000000000ULL);CHECK(actual.tx_frequency==f.config.tx_frequency);
    f.usb.clear_log();ok(och_configure(&f.device,&f.config));CHECK(contains(f.usb.command_log(),"rx_lo_freq=2400"));CHECK(sources(f).rx_frequency==2400000000ULL);
    ok(och_set_tx_lo_source(&f.device,1));CHECK(sources(f).tx_source==1&&sources(f).rx_source==0);
}
void lo_source_noop_invalid_unsupported_preserve_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.clear_log();
    ok(och_set_rx_lo_source(&f.device,0));CHECK(och_set_rx_lo_source(&f.device,2)==OCH_INVALID);
    CHECK(och_set_tx_lo_source(&f.device,UINT32_MAX)==OCH_INVALID);CHECK(och_get_lo_source(&f.device,nullptr)==OCH_INVALID);
    och_lo_source_config bad{};CHECK(och_get_lo_source(&f.device,&bad)==OCH_INVALID);
    f.usb.lo_supported=false;CHECK(och_set_rx_lo_source(&f.device,1)==OCH_UNSUPPORTED);
    CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void lo_source_restored_band_updates_manual_gain_cache(){
    Fixture f;auto* rx=f.create(OCH_RX);ok(och_set_rx_lo_source(&f.device,1));
    f.config.rx_frequency=2400000000ULL;f.config.rx_gain[0]=76;f.config.rx_gain[1]=72;
    ok(och_configure(&f.device,&f.config));f.usb.clamp_rx_gain_on_internal=true;
    ok(och_set_rx_lo_source(&f.device,0));och_config actual{};actual.struct_size=sizeof actual;ok(och_get_config(&f.device,&actual));
    CHECK(actual.rx_frequency==1000000000ULL&&actual.rx_gain[0]==60&&actual.rx_gain[1]==60&&rx->running);
}
void external_lo_range_preflight_preserves_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.config.rx_frequency=5000000000ULL;ok(och_configure(&f.device,&f.config));f.usb.clear_log();
    CHECK(och_set_rx_lo_source(&f.device,1)==OCH_INVALID);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
    f.config.rx_frequency=4000000000ULL;ok(och_configure(&f.device,&f.config));ok(och_set_rx_lo_source(&f.device,1));f.usb.clear_log();
    f.config.rx_frequency=4000000001ULL;CHECK(och_configure(&f.device,&f.config)==OCH_INVALID);
    CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void lo_source_lease_and_inactive_partner(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX,false);const void* data=nullptr;size_t bytes=0;och_metadata m{};m.struct_size=sizeof m;
    ok(och_rx_acquire(rx,&data,&bytes,&m,1000));f.usb.clear_log();CHECK(och_set_tx_lo_source(&f.device,1)==OCH_BUSY);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));ok(och_rx_release(rx));ok(och_set_tx_lo_source(&f.device,1));CHECK(rx->running&&!tx->running&&!tx->ever_started);
}
void lo_source_write_failure_stops_duplex(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.fail_next("rx_lo_source=");
    CHECK(och_set_rx_lo_source(&f.device,1)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running&&!f.device.configured);
}
void lo_source_readback_mismatch_stops_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.ignore_lo_write=true;CHECK(och_set_rx_lo_source(&f.device,1)==OCH_PROTOCOL);
    CHECK(!rx->running&&!f.device.configured);
}
void lo_source_partner_corruption_stops_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.corrupt_lo_partner=true;CHECK(och_set_rx_lo_source(&f.device,1)==OCH_PROTOCOL);
    CHECK(!rx->running&&!f.device.configured);
}
void lo_source_malformed_preflight_preserves_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.lo_reply_override="rx_lo_source=1 frequency_hz=5000000000 verified=1";f.usb.clear_log();
    CHECK(och_set_rx_lo_source(&f.device,1)==OCH_PROTOCOL);CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void reference_unknown_state_and_duplex_restore(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);const auto before=f.receive(rx);auto c=reference(f);
    CHECK(c.source==0&&c.source_known==1&&c.last_error==0&&c.gpio_verified==1&&c.chip_verified==0&&c.lock_known==0&&c.locked==-1&&c.led_known==0&&c.led_on==-1&&c.led_mode==0);
    ok(och_set_reference_source(&f.device,1));c=reference(f);CHECK(c.source==1&&c.led_mode==1&&c.locked==-1&&c.led_on==-1);
    CHECK(c.reference_hz==10000000&&c.oscillator_hz==40000000);CHECK(f.usb.clock.reference_hz==40000000);
    CHECK(rx->running&&tx->running);const auto after=f.receive(rx);CHECK(after.epoch>before.epoch&&after.flags&OCH_META_RECONFIGURED);
    ok(och_set_reference_source(&f.device,0));CHECK(reference(f).led_mode==0);
}
void reference_noop_invalid_and_unsupported_preserve_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.clear_log();ok(och_set_reference_source(&f.device,0));
    CHECK(och_set_reference_source(&f.device,2)==OCH_INVALID);CHECK(och_get_reference_status(&f.device,nullptr)==OCH_INVALID);
    och_reference_status bad{};CHECK(och_get_reference_status(&f.device,&bad)==OCH_INVALID);
    f.usb.reference_source_supported=false;CHECK(och_set_reference_source(&f.device,1)==OCH_UNSUPPORTED);
    f.usb.reference_supported=false;och_reference_test_result result{};result.struct_size=sizeof result;CHECK(och_test_reference(&f.device,&result)==OCH_UNSUPPORTED);
    CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void reference_lease_and_inactive_partner(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX,false);const void* data=nullptr;size_t bytes=0;och_metadata m{};m.struct_size=sizeof m;
    ok(och_rx_acquire(rx,&data,&bytes,&m,1000));f.usb.clear_log();CHECK(och_set_reference_source(&f.device,1)==OCH_BUSY);
    och_reference_test_result result{};result.struct_size=sizeof result;CHECK(och_test_reference(&f.device,&result)==OCH_BUSY);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));ok(och_rx_release(rx));ok(och_set_reference_source(&f.device,1));CHECK(rx->running&&!tx->running&&!tx->ever_started);
}
void reference_write_failure_stops_duplex(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);f.usb.fail_next("clock_ref_source=");
    CHECK(och_set_reference_source(&f.device,1)==OCH_PROTOCOL);CHECK(!rx->running&&!tx->running&&!f.device.configured);
}
void reference_readback_mismatch_stops_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.ignore_reference_write=true;CHECK(och_set_reference_source(&f.device,1)==OCH_PROTOCOL);
    CHECK(!rx->running&&!f.device.configured);
}
void reference_malformed_unknown_state_preserves_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.reference_reply_override="source=0 source_known=1 initialized=1 gpio_verified=1 chip_verified=0 lock_known=0 locked=0 led_known=0 led_on=-1 led_mode=0 reference_hz=10000000 oscillator_hz=40000000 r_counter=1 n_counter=4 last_error=0";f.usb.clear_log();
    CHECK(och_set_reference_source(&f.device,1)==OCH_PROTOCOL);CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
void reference_test_restores_source_and_requires_visual_confirmation(){
    Fixture f(true,true);auto* rx=f.create(OCH_RX);auto* tx=f.create(OCH_TX);ok(och_set_reference_source(&f.device,1));const auto before=f.receive(rx);
    och_reference_test_result result{};result.struct_size=sizeof result;ok(och_test_reference(&f.device,&result));
    CHECK(result.gpio_verified==1&&result.chip_verified==0&&result.visual_confirmation_required==1);CHECK(reference(f).source==1);
    CHECK(rx->running&&tx->running&&f.device.configured);const auto after=f.receive(rx);CHECK(after.epoch>before.epoch&&after.flags&OCH_META_RECONFIGURED);
    CHECK(och_test_reference(&f.device,nullptr)==OCH_INVALID);och_reference_test_result bad{};CHECK(och_test_reference(&f.device,&bad)==OCH_INVALID);
}
void reference_test_restore_failure_stops_stream(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.corrupt_reference_restore=true;och_reference_test_result result{};result.struct_size=sizeof result;
    CHECK(och_test_reference(&f.device,&result)==OCH_PROTOCOL);CHECK(!rx->running&&!f.device.configured);
}
void reference_test_rejects_false_chip_verification(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.reference_test_override="gpio_verified=1 chip_verified=1 visual_confirmation_required=0";
    och_reference_test_result result{};result.struct_size=sizeof result;CHECK(och_test_reference(&f.device,&result)==OCH_PROTOCOL);CHECK(!rx->running&&!f.device.configured);
}
void reference_and_lo_cli_catalog(){
    using namespace och_cli::commands;
    CHECK(translate("rx.lo-source",{"external"},Operation::set)=="rx_lo_source=1");
    CHECK(translate("tx.lo-source",{"internal"},Operation::set)=="tx_lo_source=0");
    CHECK(translate("clock.ref-source",{"external"},Operation::set)=="clock_ref_source=1");
    CHECK(translate("clock.ref-status",{},Operation::query)=="adf4002_status?");
    CHECK(translate("clock.ref-test",{},Operation::action)=="adf4002_self_test!");
    CHECK(canonical_reply("rx.lo-source","rx_lo_source=1 frequency_hz=1000000000 verified=1")=="rx.lo-source=1 frequency_hz=1000000000 verified=1");
    bool rejected=false;try{(void)translate("rx.lo-source",{"2"},Operation::set);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
}
void reference_unknown_source_is_readable_diagnostic(){
    Fixture f;f.usb.reference_source_known=f.usb.reference_gpio_verified=false;f.usb.reference_last_error=-5;
    const auto c=reference(f);CHECK(c.initialized==1&&c.source==0&&c.source_known==0&&c.gpio_verified==0&&c.last_error==-5);
    CHECK(c.lock_known==0&&c.locked==-1&&c.led_known==0&&c.led_on==-1);CHECK(sizeof(c)==96);
}
void reference_unknown_cached_internal_requires_real_recovery_write(){
    Fixture f;f.usb.reference_source_known=f.usb.reference_gpio_verified=false;f.usb.reference_last_error=-5;f.usb.clear_log();
    ok(och_set_reference_source(&f.device,0));const auto c=reference(f);
    CHECK(occurrences(f.usb.command_log(),"clock_ref_source=0")==1);
    CHECK(!contains(f.usb.command_log(),"rf_reinit!"));CHECK(c.source_known==1&&c.gpio_verified==1&&c.last_error==0&&c.source==0);
}
void reference_unknown_source_test_rejected_before_interrupt(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.reference_source_known=f.usb.reference_gpio_verified=false;f.usb.reference_last_error=-5;f.usb.clear_log();
    och_reference_test_result result{};result.struct_size=sizeof result;CHECK(och_test_reference(&f.device,&result)==OCH_INVALID);
    CHECK(std::string(och_last_error()).find("explicitly set reference source to internal")!=std::string::npos);
    CHECK(!contains(f.usb.command_log(),"usb_stop!"));CHECK(!contains(f.usb.command_log(),"adf4002_self_test!"));CHECK(rx->running&&f.device.configured);
}
void reference_unknown_postwrite_state_fails_closed(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.reference_source_known=f.usb.reference_gpio_verified=false;f.usb.reference_last_error=-5;f.usb.reference_write_leaves_unknown=true;
    CHECK(och_set_reference_source(&f.device,0)==OCH_PROTOCOL);CHECK(!rx->running&&!f.device.configured);
    const auto c=reference(f);CHECK(c.source_known==0&&c.last_error==-5);
}
void reference_unknown_posttest_state_fails_closed(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.reference_test_leaves_unknown=true;och_reference_test_result result{};result.struct_size=sizeof result;
    CHECK(och_test_reference(&f.device,&result)==OCH_PROTOCOL);CHECK(!rx->running&&!f.device.configured);
}
void reference_source_diagnostic_fields_are_strict(){
    Fixture f;auto* rx=f.create(OCH_RX);f.usb.clear_log();
    const std::string tail=" initialized=1 gpio_verified=1 chip_verified=0 lock_known=0 locked=-1 led_known=0 led_on=-1 led_mode=0 reference_hz=10000000 oscillator_hz=40000000 r_counter=1 n_counter=4 last_error=";
    for(const std::string& invalid:{"source=0 source_known=2"+tail+"0","source=0 source_known=1"+tail+"-5","source=0 source_known=0"+tail+"2147483648","source=0 source_known=0"+tail+"-5oops"}) {
        f.usb.reference_reply_override=invalid;och_reference_status c{};c.struct_size=sizeof c;CHECK(och_get_reference_status(&f.device,&c)==OCH_PROTOCOL);
    }
    CHECK(rx->running&&f.device.configured);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
}
}
// PPS cases share the actual stream workers and control API fixture above.
namespace {
#include "pps_tests.inc"
#include "board_tests.inc"
#include "pps_lock_tests.inc"
#include "iq8_tests.inc"
}
int main(){
    struct Case {const char* name;void (*run)();};
    const Case cases[]={
        {"IQ8 codec exhaustively preserves high bits and scaling",iq8_codec_exhaustive},
        {"IQ8 float conversion preserves legacy quantization",iq8_float_quantization},
        {"IQ8 framer handles fragmented and adjacent preambles",iq8_framer_boundaries},
        {"IQ8 config checks capabilities masks ABI and defaults",iq8_configuration_contract},
        {"IQ8 format changes require destroyed stream objects",iq8_format_change_refused},
        {"IQ8 wire readback fails closed",iq8_readback_failure},
        {"IQ8 RX supports all four application layouts and leases",iq8_receive_formats},
        {"IQ8 TX supports all four application layouts",iq8_transmit_formats},
        {"IQ8 duplex reconfigure preserves wire counts and epochs",iq8_duplex_reconfigure},
        {"IQ8 CLI mapping and wire throughput presentation",iq8_cli_contract},
        {"All legacy 12-bit masks and IQ8 tail padding retain counts",iq8_legacy_matrix_and_tail_padding},
        {"Fresh config clears a previous handle's IQ8 before masks",iq8_new_handle_resets_previous_format},
        {"IQ8 first 16KiB packet starts TX before USB block completion",iq8_first_packet_prefill_starts_pending_usb},
        {"board defaults, switches and no-op",board_defaults_and_switches},
        {"board control preserves duplex and RX lease",board_duplex_and_lease},
        {"board arguments and legacy firmware rejection",board_invalid_and_unsupported},
        {"board failures leave RF running",board_failures_preserve_rf},
        {"board unknown temperature and full-duty failsafe",board_unknown_temperature_and_failsafe},
        {"board status parsers reject malformed and stale data",board_strict_status_parsers},
        {"board CLI mapping, presentation and ABI",board_cli_and_abi},
        {"PPS lock has inclusive positive/negative 1 ppm boundaries",pps_lock_boundaries},
        {"absent disabled invalid and stale PPS hide lock",pps_lock_unavailable},
        {"reference control is independent of PPS support and errors",pps_lock_reference_independence},
        {"PPS lock queries preserve duplex and RX lease",pps_lock_streams},
        {"CLI hides hardware lock and uses the same PPS snapshot",pps_lock_cli_view},
        {"PPS defaults off and absent signal stays unknown",pps_defaults_and_missing_signal},
        {"PPS toggles preserve duplex and outstanding RX lease",pps_duplex_and_lease_are_undisturbed},
        {"PPS rejects invalid and legacy firmware requests",pps_invalid_and_legacy_firmware},
        {"PPS control errors leave RF streams running",pps_errors_do_not_stop_rf},
        {"PPS typed API reports valid frequency and ppm",pps_valid_frequency},
        {"PPS parser rejects malformed and stale measurements",pps_malformed_and_stale_data},
        {"PPS CLI catalog maps monitoring commands",pps_cli_catalog},
        {"RX restart preserves handle and monotonic epochs",rx_restart_and_epoch},
        {"header-only RX epoch survives reconfiguration",header_only_epoch_is_preserved},
        {"adjacent RX preambles count all headers and resyncs",adjacent_headers_count_every_epoch},
        {"TX restart drops old IQ and transmits zero prefill",tx_restart_discards_old_iq},
        {"duplex resumes both directions",duplex_restart},
        {"idle and stopped streams stay idle",idle_and_stopped_streams_stay_idle},
        {"inactive duplex partner stays idle",inactive_partner_does_not_start},
        {"unchanged and invalid config leave hardware untouched",unchanged_and_invalid_do_not_interrupt},
        {"outstanding zero-copy lease blocks mutation",outstanding_lease_prevents_mutation},
        {"60-second recv yields promptly to reconfigure",long_receive_releases_for_reconfigure},
        {"60-second send yields promptly to reconfigure",long_send_releases_for_reconfigure},
        {"60-second flush yields promptly to reconfigure",long_flush_releases_for_reconfigure},
        {"producer refills during hardware restart acknowledgement",producer_can_refill_during_hardware_resume},
        {"handoff failure interrupts producer before queue reset",handoff_failure_interrupts_producer_before_clearing},
        {"minimum four-block ring handoff failure interrupts waiting send",minimum_ring_handoff_failure_interrupts_empty_send},
        {"parameter failure stops both directions",failure_stops_both_directions},
        {"resume failure stops both directions",resume_failure_stops_both_directions},
        {"RX resume failure prevents TX restart",rx_resume_failure_prevents_tx_restart},
        {"failed reconfiguration accounts unread RX loss",failed_reconfigure_accounts_unread_rx_samples},
        {"FIR commit failure aborts and stops both directions",typed_fir_failure_stops_both_directions},
        {"typed clock and FIR setters resume streaming",typed_clock_and_fir_setters_resume},
        {"correction idle setters preserve the other setting",correction_idle_and_partner_preservation},
        {"correction changes resume duplex with new RX epochs",correction_duplex_resume_and_epoch},
        {"correction no-op and invalid requests preserve active stream",correction_noop_and_invalid_leave_stream_running},
        {"legacy firmware correction rejection preserves active stream",correction_legacy_firmware_preserves_stream},
        {"correction change respects outstanding RX lease",correction_lease_blocks_write},
        {"correction change leaves inactive TX stopped",correction_stopped_partner_stays_stopped},
        {"correction command failure stops duplex",correction_write_failure_stops_both},
        {"correction readback mismatch stops duplex",correction_readback_mismatch_stops_both},
        {"correction detects unintended partner changes",correction_partner_corruption_is_detected},
        {"malformed correction preflight preserves active stream",correction_malformed_reply_does_not_stop_stream},
        {"correction CLI catalog uses exact SDK commands",correction_cli_catalog},
        {"unsupported FIR and clock leave duplex running",unsupported_clock_and_fir_preserve_duplex},
        {"fixed reference clock rejection leaves duplex running",fixed_reference_clock_rejection_preserves_duplex},
        {"LO source switches resume duplex and synchronize nominal frequency cache",lo_source_duplex_and_frequency_cache},
        {"LO source no-op invalid and unsupported requests preserve stream",lo_source_noop_invalid_unsupported_preserve_stream},
        {"restoring internal RX LO synchronizes band-clamped manual gain cache",lo_source_restored_band_updates_manual_gain_cache},
        {"external LO 4 GHz limits are checked before stream interruption",external_lo_range_preflight_preserves_stream},
        {"LO source respects RX lease and inactive partner",lo_source_lease_and_inactive_partner},
        {"LO source write failure stops duplex",lo_source_write_failure_stops_duplex},
        {"LO source readback mismatch stops stream",lo_source_readback_mismatch_stops_stream},
        {"LO source partner corruption stops stream",lo_source_partner_corruption_stops_stream},
        {"malformed LO source preflight preserves stream",lo_source_malformed_preflight_preserves_stream},
        {"reference selection preserves unknown observations and resumes duplex",reference_unknown_state_and_duplex_restore},
        {"reference no-op invalid and unsupported requests preserve stream",reference_noop_invalid_and_unsupported_preserve_stream},
        {"reference operations respect RX lease and inactive partner",reference_lease_and_inactive_partner},
        {"reference source write failure stops duplex",reference_write_failure_stops_duplex},
        {"reference source readback mismatch stops stream",reference_readback_mismatch_stops_stream},
        {"malformed unknown reference state preserves stream",reference_malformed_unknown_state_preserves_stream},
        {"reference self-test restores source and requires human LED confirmation",reference_test_restores_source_and_requires_visual_confirmation},
        {"reference self-test restoration failure stops stream",reference_test_restore_failure_stops_stream},
        {"reference self-test rejects false chip verification",reference_test_rejects_false_chip_verification},
        {"reference and LO CLI catalog maps exact SDK commands",reference_and_lo_cli_catalog},
        {"unknown reference source remains a readable diagnostic",reference_unknown_source_is_readable_diagnostic},
        {"cached internal reference after fault requires actual recovery write",reference_unknown_cached_internal_requires_real_recovery_write},
        {"unknown reference self-test is rejected before stream interruption",reference_unknown_source_test_rejected_before_interrupt},
        {"unknown reference readback after write fails closed",reference_unknown_postwrite_state_fails_closed},
        {"unknown reference readback after self-test fails closed",reference_unknown_posttest_state_fails_closed},
        {"reference source-known and signed error fields are parsed strictly",reference_source_diagnostic_fields_are_strict}
    };
    unsigned failed=0;
    for(const auto& c:cases){try{c.run();std::cout<<"PASS "<<c.name<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<c.name<<": "<<e.what()<<'\n';}}
    std::cout<<(sizeof cases/sizeof *cases-failed)<<"/"<<sizeof cases/sizeof *cases<<" passed\n";return failed?1:0;
}
