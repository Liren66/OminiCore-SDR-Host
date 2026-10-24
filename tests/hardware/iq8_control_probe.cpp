// Standalone hardware regression. Build only unless explicitly run by the operator.
// Uses production typed APIs; raw commands below are read-only diagnostics.
#include <omnicore/och.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock=std::chrono::steady_clock;
using Fields=std::map<std::string,std::string>;
using Counters=std::map<std::string,uint64_t>;
constexpr uint32_t rate=30720000,block_bytes=196608;
constexpr size_t frame_bytes=4,block_frames=block_bytes/frame_bytes;
void require(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
std::string quote(const std::string& s) {
    static const char hex[]="0123456789abcdef";std::string out="\"";
    for(unsigned char c:s){if(c=='\n')out+="\\n";else if(c=='\r')out+="\\r";
        else if(c=='\t')out+="\\t";else if(c<0x20){out+="\\u00";out+=hex[c>>4];out+=hex[c&15];}
        else{if(c=='\\'||c=='\"')out+='\\';out+=static_cast<char>(c);}}
    return out+'"';
}
Fields fields(const std::string& text) {
    Fields out;std::istringstream in(text);std::string token;
    while(in>>token){auto at=token.find('=');if(at!=std::string::npos)
        require(out.emplace(token.substr(0,at),token.substr(at+1)).second,"duplicate diagnostic field");}
    return out;
}
std::string line(const std::string& text,const std::string& prefix) {
    size_t at=text.find(prefix);
    while(at!=std::string::npos&&at&&text[at-1]!='\n')at=text.find(prefix,at+1);
    require(at!=std::string::npos,"missing diagnostic line "+prefix);return text.substr(at,text.find('\n',at)-at);
}
Counters faults(const std::string& text) {
    const auto begin=text.find("faults ");require(begin!=std::string::npos,"missing bridge fault counters");
    const auto values=fields(text.substr(begin,text.find('\n',begin)-begin));Counters out;
    for(const char* key:{"rx_overflow","rx_underflow","tx_overflow","tx_underflow","rx_protocol","tx_protocol","rx_pair","tx_pair","event_dropped"}) {
        const auto it=values.find(key);require(it!=values.end(),std::string("missing bridge counter ")+key);
        size_t used=0;const auto value=std::stoull(it->second,&used);require(used==it->second.size(),"invalid bridge counter");out[key]=value;
    }
    return out;
}
Counters delta(Counters after,const Counters& before) {
    for(auto& value:after){const auto previous=before.at(value.first);require(value.second>=previous,"bridge counter decreased/reset");value.second-=previous;}
    return after;
}
std::string json(const Counters& values) {
    std::ostringstream out;out<<'{';bool first=true;
    for(const auto& value:values){if(!first)out<<',';first=false;out<<quote(value.first)<<':'<<value.second;}return out.str()+'}';
}
bool same_clock(const och_clock_config& a,const och_clock_config& b) {
    return a.reference_hz==b.reference_hz&&std::equal(a.rx_hz,a.rx_hz+6,b.rx_hz)&&std::equal(a.tx_hz,a.tx_hz+6,b.tx_hz);
}
struct Options {std::string serial;double startup=3,settle=3,steady=3;};
Options parse(int argc,char** argv) {
    Options out;
    for(int n=1;n<argc;++n){const std::string key=argv[n];require(n+1<argc,"missing option value");const std::string value=argv[++n];
        if(key=="--serial"){out.serial=value;continue;}
        size_t used=0;const double seconds=std::stod(value,&used);require(used==value.size()&&std::isfinite(seconds)&&seconds>=0.25&&seconds<=30,"observation seconds must be 0.25..30");
        if(key=="--startup-seconds")out.startup=seconds;else if(key=="--settle-seconds")out.settle=seconds;
        else if(key=="--steady-seconds"){require(seconds>=1,"steady window must be at least 1 second");out.steady=seconds;}
        else throw std::runtime_error("unknown option "+key);
    }
    return out;
}
struct Snapshot {och_stats rx{},tx{};Counters bridge;uint64_t boundaries=0,gaps=0,reconfigured=0,epoch=0;double stamp=0;};
class Probe {
    Options options;
    och::device dev;
    std::unique_ptr<och::stream> rx,tx;
    std::vector<int8_t> zeros=std::vector<int8_t>(block_bytes),input=std::vector<int8_t>(block_bytes);
    std::atomic<bool> stop_tx{false},stop_rx{false},failed{false};std::thread producer,consumer;
    std::atomic<uint64_t> boundaries{0},gaps{0},reconfigured{0},epoch{0};
    std::mutex failure_mutex;std::string worker_error;
    Clock::time_point origin=Clock::now();uint64_t reconfigure_events=0;
    och_rx_correction_config saved_correction{};och_pps_status saved_pps{};och_clock_config saved_clock{},active_clock{};
    bool saved=false,configured=false,closed=false;
public:
    explicit Probe(Options value):options(std::move(value)),dev(options.serial){}
    ~Probe(){if(!closed){try{finish();}catch(...){}}}
    double elapsed()const{return std::chrono::duration<double>(Clock::now()-origin).count();}
    void worker_failure(const std::exception& error){std::lock_guard<std::mutex> lock(failure_mutex);worker_error=error.what();failed=true;}
    void healthy(){if(failed){std::lock_guard<std::mutex> lock(failure_mutex);throw std::runtime_error("IO worker: "+worker_error);}}
    void events() {
        och_event event{};unsigned drained=0;
        while(drained++<512&&dev.poll_event(event)) {
            if(event.type==OCH_EVENT_RECONFIGURED)++reconfigure_events;
            std::cout<<"{\"type\":\"event\",\"t\":"<<elapsed()<<",\"event_type\":"<<event.type
                <<",\"sequence\":"<<event.sequence<<",\"epoch\":"<<event.epoch<<",\"message\":"<<quote(event.message)<<"}\n";
        }
    }
    void wait(double seconds) {
        const auto end=Clock::now()+std::chrono::duration<double>(seconds);
        while(Clock::now()<end){healthy();events();std::this_thread::sleep_for(std::chrono::milliseconds(10));}
        healthy();events();
    }
    std::string verify_wire() {
        const auto reply=dev.command("iq_format?");const auto f=fields(reply);
        for(const char* key:{"iq_format","requested","applied"})require(f.at(key)=="8",std::string("IQ8 lost: ")+key);
        require(f.at("verified")=="1"&&f.at("cfg_busy")=="0"&&f.at("cfg_error")=="0","IQ8 readback unverified or busy/error");
        const auto c=dev.config();require(c.usb_iq_bits==8&&c.rx_mask==3&&c.tx_mask==3&&c.sample_rate==rate,"cached IQ8 2TX2RX configuration changed");
        require(fields(dev.command("rx_channel?")).at("rx_channel")=="3"&&fields(dev.command("tx_channel?")).at("tx_channel")=="3","hardware channel masks changed");
        require(rx->stats().running&&tx->stats().running,"both streams must be running");
        return reply;
    }
    Snapshot snapshot() {
        Snapshot s;s.bridge=faults(dev.command("usb_status?"));events();s.rx=rx->stats();s.tx=tx->stats();
        s.boundaries=boundaries;s.gaps=gaps;s.reconfigured=reconfigured;s.epoch=epoch;s.stamp=elapsed();return s;
    }
    void steady(const std::string& label) {
        const auto first=snapshot();wait(options.steady);const auto last=snapshot();const auto bridge=delta(last.bridge,first.bridge);
        const double seconds=last.stamp-first.stamp;
        const double rx_bps=double(last.rx.usb_bytes-first.rx.usb_bytes)/seconds,tx_bps=double(last.tx.usb_bytes-first.tx.usb_bytes)/seconds;
        const bool clean=std::all_of(bridge.begin(),bridge.end(),[](const auto& p){return p.second==0;})&&
            last.rx.running&&last.tx.running&&last.rx.host_dropped_samples==first.rx.host_dropped_samples&&
            last.rx.headers==first.rx.headers&&last.rx.resyncs==first.rx.resyncs&&last.rx.transfer_errors==first.rx.transfer_errors&&
            last.tx.transfer_errors==first.tx.transfer_errors&&last.rx.device_overflows==first.rx.device_overflows&&
            last.rx.device_underflows==first.rx.device_underflows&&last.rx.event_drops==first.rx.event_drops&&
            last.tx.tx_starvation_events==first.tx.tx_starvation_events&&last.tx.tx_padding_samples==first.tx.tx_padding_samples&&
            last.boundaries==first.boundaries&&last.gaps==first.gaps&&last.reconfigured==first.reconfigured&&
            last.rx.samples>first.rx.samples&&rx_bps>rate*4.0*0.98&&tx_bps>rate*4.0*0.98;
        std::cout<<"{\"type\":\"steady\",\"case\":"<<quote(label)<<",\"seconds\":"<<seconds<<",\"clean\":"<<(clean?"true":"false")
            <<",\"rx_MBps\":"<<rx_bps/1e6<<",\"tx_MBps\":"<<tx_bps/1e6<<",\"bridge_delta\":"<<json(bridge)
            <<",\"rx_host_drop_delta\":"<<last.rx.host_dropped_samples-first.rx.host_dropped_samples
            <<",\"rx_header_delta\":"<<last.rx.headers-first.rx.headers<<",\"rx_resync_delta\":"<<last.rx.resyncs-first.rx.resyncs
            <<",\"tx_starved_delta\":"<<last.tx.tx_starvation_events-first.tx.tx_starvation_events
            <<",\"rx_transfer_error_delta\":"<<last.rx.transfer_errors-first.rx.transfer_errors<<",\"tx_transfer_error_delta\":"<<last.tx.transfer_errors-first.tx.transfer_errors
            <<",\"device_overflow_delta\":"<<last.rx.device_overflows-first.rx.device_overflows<<",\"device_underflow_delta\":"<<last.rx.device_underflows-first.rx.device_underflows
            <<",\"event_drop_delta\":"<<last.rx.event_drops-first.rx.event_drops<<",\"tx_padding_delta\":"<<last.tx.tx_padding_samples-first.tx.tx_padding_samples
            <<",\"rx_samples_delta\":"<<last.rx.samples-first.rx.samples<<",\"reconfigured_metadata_delta\":"<<last.reconfigured-first.reconfigured
            <<",\"rx_running\":"<<last.rx.running<<",\"tx_running\":"<<last.tx.running
            <<",\"boundary_delta\":"<<last.boundaries-first.boundaries<<",\"index_gap_delta\":"<<last.gaps-first.gaps<<"}"<<std::endl;
        require(clean,label+": steady streaming did not recover cleanly");
    }
    void operation(const std::string& name,bool interrupts,const std::function<void()>& apply,const std::function<void()>& verify) {
        healthy();events();const auto before=snapshot();const auto event_before=reconfigure_events;
        const auto start=Clock::now();
        std::cout<<"{\"type\":\"operation_begin\",\"case\":"<<quote(name)<<",\"t\":"<<elapsed()<<",\"expected_reconfigure\":"<<(interrupts?"true":"false")<<"}"<<std::endl;
        apply();const double call_ms=std::chrono::duration<double,std::milli>(Clock::now()-start).count();verify();
        const auto wire=verify_wire();wait(options.settle);
        if(interrupts) {
            const auto limit=Clock::now()+std::chrono::seconds(5);
            while(reconfigured<=before.reconfigured&&Clock::now()<limit)wait(0.02);
        }
        const auto after=snapshot();const auto event_delta=reconfigure_events-event_before;
        const uint64_t padding=after.tx.tx_padding_samples-before.tx.tx_padding_samples;
        const bool semantics=interrupts?(after.epoch>before.epoch&&after.reconfigured-before.reconfigured==1&&event_delta==1&&padding==4ull*block_bytes/frame_bytes):
            (after.reconfigured==before.reconfigured&&event_delta==0&&padding==0);
        std::cout<<"{\"type\":\"operation_result\",\"case\":"<<quote(name)<<",\"call_ms\":"<<call_ms
            <<",\"settled_ms\":"<<std::chrono::duration<double,std::milli>(Clock::now()-start).count()
            <<",\"semantics_ok\":"<<(semantics?"true":"false")<<",\"iq_format\":"<<quote(wire)
            <<",\"rx_running\":"<<after.rx.running<<",\"tx_running\":"<<after.tx.running<<",\"epoch_before\":"<<before.epoch
            <<",\"epoch_after\":"<<after.epoch<<",\"reconfigure_events\":"<<event_delta<<",\"reconfigured_metadata_delta\":"<<after.reconfigured-before.reconfigured
            <<",\"tx_padding_samples_delta\":"<<padding<<",\"transition_host_drop_delta\":"<<after.rx.host_dropped_samples-before.rx.host_dropped_samples
            <<",\"transition_bridge_delta\":"<<json(delta(after.bridge,before.bridge))<<"}"<<std::endl;
        require(semantics,name+": unexpected reconfiguration/metadata behavior");steady(name);
    }
    void check_correction(unsigned dc,unsigned iq) {
        const auto actual=dev.rx_correction();require(actual.dc_enabled==dc&&actual.iq_enabled==iq,"RX correction or partner readback mismatch");
    }
    void check_pps(unsigned enabled) {
        const auto actual=dev.pps_status();require(actual.enabled==enabled,"PPS enable readback mismatch");
        std::cout<<"{\"type\":\"pps\",\"enabled\":"<<actual.enabled<<",\"state\":"<<actual.state<<",\"frequency_valid\":"<<actual.frequency_valid
            <<",\"nominal_hz\":"<<actual.nominal_hz<<",\"lock_known\":"<<actual.lock_known<<",\"locked\":"<<actual.locked<<"}\n";
    }
    void run() {
        saved_clock=dev.clock();saved_correction=dev.rx_correction();saved_pps=dev.pps_status();saved=true;
        auto cfg=och::default_config();cfg.sample_rate=rate;cfg.rx_mask=cfg.tx_mask=3;cfg.usb_iq_bits=8;
        cfg.tx_attenuation[0]=cfg.tx_attenuation[1]=89.75;configured=true;dev.configure(cfg);active_clock=dev.clock();
        std::cout<<"{\"type\":\"config\",\"version\":"<<quote(och_version())<<",\"rate\":"<<rate<<",\"iq_bits\":8,\"rx_mask\":3,\"tx_mask\":3,\"format\":\"sc8\",\"zero_iq\":true,\"attenuation_db\":89.75"
            <<",\"saved_dc\":"<<saved_correction.dc_enabled<<",\"saved_iq\":"<<saved_correction.iq_enabled<<",\"saved_pps\":"<<saved_pps.enabled
            <<",\"observation_seconds\":"<<options.startup+options.steady+7*(options.settle+options.steady)<<"}"<<std::endl;
        auto args=och::stream_args(OCH_RX);args.format=OCH_SC8;rx=std::make_unique<och::stream>(dev,args);
        args.direction=OCH_TX;tx=std::make_unique<och::stream>(dev,args);
        for(unsigned n=0;n<4;++n){size_t accepted=0;och::check(tx->send(zeros.data(),block_frames,accepted,1000));require(accepted==block_frames,"partial TX prefill");}
        producer=std::thread([&]{try{while(!stop_tx){size_t accepted=0;const auto rc=tx->send(zeros.data(),block_frames,accepted,100);
            if(rc==OCH_TIMEOUT){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
            if(!(stop_tx&&rc==OCH_STOPPED))och::check(rc);}}
            catch(const std::exception& e){worker_failure(e);}});
        const auto warm_limit=Clock::now()+std::chrono::seconds(10);
        while(tx->stats().host_queue_bytes<32ull*1048576){healthy();require(Clock::now()<warm_limit,"TX producer warmup timeout");std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        rx->start();
        consumer=std::thread([&]{try{bool have=false;uint64_t next=0,previous_epoch=0;
            while(!stop_rx){size_t received=0;och_metadata m{};const auto rc=rx->recv(input.data(),block_frames,received,m,100);
                if(rc==OCH_TIMEOUT){std::this_thread::sleep_for(std::chrono::milliseconds(1));continue;}
                if(stop_rx&&rc==OCH_STOPPED)break;
                och::check(rc);require(received>0&&m.epoch>0&&(!have||m.epoch>=previous_epoch),"RX count/epoch invalid");
                if(have&&(m.epoch!=previous_epoch||(m.flags&OCH_META_DISCONTINUITY)))++boundaries;
                if(have&&m.epoch==previous_epoch&&m.sample_index!=next)++gaps;
                if(m.flags&OCH_META_RECONFIGURED){require((m.flags&(OCH_META_START|OCH_META_DISCONTINUITY|OCH_META_TIME_UNKNOWN))==(OCH_META_START|OCH_META_DISCONTINUITY|OCH_META_TIME_UNKNOWN)&&m.device_dropped_samples==-1,"reconfigure metadata flags/loss marker missing");++reconfigured;}
                epoch=m.epoch;next=m.sample_index+received;previous_epoch=m.epoch;have=true;
            }}catch(const std::exception& e){worker_failure(e);}});
        tx->start();verify_wire();wait(options.startup);steady("startup");
        const unsigned dc=saved_correction.dc_enabled,iq=saved_correction.iq_enabled,pps=saved_pps.enabled;
        operation("dc_toggle",true,[&]{dev.set_rx_dc_correction(!dc);},[&]{check_correction(!dc,iq);});
        operation("dc_restore",true,[&]{dev.set_rx_dc_correction(dc);},[&]{check_correction(dc,iq);});
        operation("iq_toggle",true,[&]{dev.set_rx_iq_correction(!iq);},[&]{check_correction(dc,!iq);});
        operation("iq_restore",true,[&]{dev.set_rx_iq_correction(iq);},[&]{check_correction(dc,iq);});
        operation("pps_toggle",false,[&]{dev.set_pps_enabled(!pps);},[&]{check_pps(!pps);});
        operation("pps_restore",false,[&]{dev.set_pps_enabled(pps);},[&]{check_pps(pps);});
        operation("clock_same_chain_apply",true,[&]{dev.set_clock(active_clock);},[&]{require(same_clock(dev.clock(),active_clock),"clock chain readback mismatch");});
        require(rx->native_handle()!=nullptr&&tx->native_handle()!=nullptr,"stream handles lost");
    }
    void finish() {
        if(closed)return;
        closed=true;std::vector<std::string> errors;bool stopped=false,restored=false;std::string final_status;
        auto attempt=[&](const char* name,const std::function<void()>& action){try{action();}catch(const std::exception& error){errors.push_back(std::string(name)+": "+error.what());}};
        stop_tx=true;if(producer.joinable())producer.join();
        if(tx)attempt("stop TX",[&]{tx->stop(3000);});
        stop_rx=true;if(rx)attempt("stop RX",[&]{rx->stop(3000);});if(consumer.joinable())consumer.join();
        if(failed){std::lock_guard<std::mutex> lock(failure_mutex);errors.push_back("IO worker: "+worker_error);}
        if(tx)attempt("close TX",[&]{tx->close();});
        if(rx)attempt("close RX",[&]{rx->close();});
        tx.reset();rx.reset();
        if(saved&&configured) {
            attempt("restore DC",[&]{dev.set_rx_dc_correction(saved_correction.dc_enabled);});
            attempt("restore IQ",[&]{dev.set_rx_iq_correction(saved_correction.iq_enabled);});
            attempt("restore PPS",[&]{dev.set_pps_enabled(saved_pps.enabled);});
            attempt("restore clock",[&]{if(!same_clock(dev.clock(),saved_clock))dev.set_clock(saved_clock);});
            attempt("verify restored controls",[&]{check_correction(saved_correction.dc_enabled,saved_correction.iq_enabled);check_pps(saved_pps.enabled);require(same_clock(dev.clock(),saved_clock),"saved clock chain not restored");restored=true;});
        }
        attempt("verify hardware stopped",[&]{final_status=dev.command("usb_status?");const auto ready=fields(line(final_status,"rx_ready=")),stream=fields(line(final_status,"usb_stream "));
            // usb_stream rx/tx are monotonic epoch counters, not enable bits.
            require(ready.at("rx_ready")=="0/0"&&ready.at("tx_ready")=="0/0"&&ready.at("tx_started")=="0"&&
                stream.at("expected_rx_Bps")=="0"&&stream.at("expected_tx_Bps")=="0","hardware remains enabled after stop");stopped=true;});
        std::cout<<"{\"type\":\"cleanup\",\"stopped\":"<<(stopped?"true":"false")<<",\"controls_restored\":"<<(restored?"true":"false")
            <<",\"status\":"<<quote(final_status)<<",\"scope\":\"saved DC/IQ/PPS/full clock chain; test RF configuration and IQ8 masks remain selected, streams stopped\",\"errors\":[";
        for(size_t i=0;i<errors.size();++i){if(i)std::cout<<',';std::cout<<quote(errors[i]);}std::cout<<"]}"<<std::endl;
        require(errors.empty(),"cleanup/restoration failed; inspect cleanup errors");
    }
};
}
int main(int argc,char** argv) {
    if(argc==2&&std::string(argv[1])=="--help") {
        std::cout<<"iq8_control_probe [--serial SERIAL] [--startup-seconds 3] [--settle-seconds 3] [--steady-seconds 3]\n"
            "Explicit hardware test: 30.72 MSPS, 2TX2RX, IQ8/SC8, zero IQ and 89.75 dB TX attenuation.\n"
            "Typed DC toggle/restore, IQ toggle/restore, PPS toggle/restore, same full clock-chain apply.\n"
            "Default observation windows total 48 seconds, plus bounded device/control latency and cleanup.\n"
            "Startup/reconfiguration allow a 3-second recovery window before strict 3-second steady assertions.\n"
            "Saves/restores DC/IQ/PPS/full clock chain; leaves test RF configuration and IQ8 masks selected with streams stopped.\n";
        return 0;
    }
    const auto started=Clock::now();
    try {const auto options=parse(argc,argv);Probe probe(options);
        try{probe.run();probe.finish();std::cout<<"{\"type\":\"result\",\"passed\":true,\"operations\":7,\"total_seconds\":"<<std::chrono::duration<double>(Clock::now()-started).count()<<"}"<<std::endl;return 0;}
        catch(...){const auto error=std::current_exception();try{probe.finish();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}std::rethrow_exception(error);}
    }catch(const std::exception& e){std::cout<<"{\"type\":\"result\",\"passed\":false,\"total_seconds\":"<<std::chrono::duration<double>(Clock::now()-started).count()<<",\"error\":"<<quote(e.what())<<"}"<<std::endl;return 1;}
}
