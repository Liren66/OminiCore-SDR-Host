#include "fake_device.hpp"
#include <sstream>

using namespace och::core;
namespace {
test::FakeUsb& fake(WINUSB_INTERFACE_HANDLE h){return *static_cast<test::FakeUsb*>(h);}
BOOL WINAPI free_usb(WINUSB_INTERFACE_HANDLE){return TRUE;}
BOOL WINAPI read_usb(WINUSB_INTERFACE_HANDLE h,UCHAR ep,PUCHAR bytes,ULONG size,PULONG count,LPOVERLAPPED ov){
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    f.requests[ov]={ep,bytes,size,false,false,++f.next_request};f.complete_rx();
    if(count)*count=f.requests[ov].completed?size:0;
    if(f.requests[ov].completed)return TRUE;
    SetLastError(ERROR_IO_PENDING);return FALSE;
}
BOOL WINAPI write_usb(WINUSB_INTERFACE_HANDLE h,UCHAR ep,PUCHAR bytes,ULONG size,PULONG count,LPOVERLAPPED ov){
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    f.writes.push_back({size,std::all_of(bytes,bytes+size,[](UCHAR c){return c==0;})});
    f.requests[ov]={ep,bytes,size,!f.hold_tx,false,++f.next_request};
    if(count)*count=f.hold_tx?0:size;
    if(!f.hold_tx){SetEvent(ov->hEvent);return TRUE;}
    SetLastError(ERROR_IO_PENDING);return FALSE;
}
BOOL WINAPI result_usb(WINUSB_INTERFACE_HANDLE h,LPOVERLAPPED ov,PULONG count,BOOL wait){
    // Bound throughput without making the worker or its stop path artificially
    // synchronous. Real OVERLAPPED events are used by WaitForMultipleObjects.
    if(!wait)Sleep(1);
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    auto it=f.requests.find(ov);
    if(it==f.requests.end()){SetLastError(ERROR_NOT_FOUND);return FALSE;}
    auto r=it->second;
    if(!r.completed){SetLastError(ERROR_IO_INCOMPLETE);return FALSE;}
    f.requests.erase(it);*count=r.cancelled?0:r.size;
    if(r.cancelled){SetLastError(ERROR_OPERATION_ABORTED);return FALSE;}
    if(r.endpoint==0x81&&r.size==och::dsp::Framer::header_bytes)++f.header_completions;
    return TRUE;
}
BOOL WINAPI abort_usb(WINUSB_INTERFACE_HANDLE h,UCHAR ep){
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    for(auto& item:f.requests)if(item.second.endpoint==ep){item.second.completed=true;item.second.cancelled=true;SetEvent(item.first->hEvent);}
    return TRUE;
}
BOOL WINAPI set_policy(WINUSB_INTERFACE_HANDLE h,UCHAR ep,ULONG key,ULONG size,PVOID value){
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    ULONG number=0;memcpy(&number,value,std::min<ULONG>(size,sizeof number));f.policies[{ep,key}]=number;return TRUE;
}
BOOL WINAPI get_policy(WINUSB_INTERFACE_HANDLE h,UCHAR ep,ULONG key,PULONG size,PVOID value){
    auto& f=fake(h);std::lock_guard<std::mutex> lock(f.mutex);
    ULONG number=key==MAXIMUM_TRANSFER_SIZE?1048576:f.policies[{ep,key}];
    memcpy(value,&number,std::min<ULONG>(*size,sizeof number));*size=sizeof number;return TRUE;
}
uint32_t crc(const int16_t* values,unsigned count){
    uint32_t c=0xffffffffU;
    for(unsigned i=0;i<count;++i)for(unsigned byte=0;byte<2;++byte){c^=(static_cast<uint16_t>(values[i])>>(byte*8))&255U;
        for(unsigned bit=0;bit<8;++bit)c=(c>>1)^((0U-(c&1U))&0xedb88320U);}
    return ~c;
}
std::vector<long long> numbers(std::string text){
    std::replace(text.begin(),text.end(),',',' ');std::istringstream in(text);std::vector<long long> result;long long v;
    while(in>>v)result.push_back(v);
    return result;
}
std::string csv(const uint32_t* values){std::string out;for(unsigned i=0;i<6;++i){if(i)out+=',';out+=std::to_string(values[i]);}return out;}
}
namespace test {
FakeUsb::FakeUsb(och_device& d){
    d.usb.h=this;d.usb.api.free=&free_usb;d.usb.api.read=&read_usb;d.usb.api.write=&write_usb;
    d.usb.api.result=&result_usb;d.usb.api.abort=&abort_usb;d.usb.api.setPolicy=&set_policy;d.usb.api.getPolicy=&get_policy;
    WINUSB_PIPE_INFORMATION pipe{};pipe.PipeType=UsbdPipeTypeBulk;pipe.MaximumPacketSize=1024;
    pipe.PipeId=0x81;d.usb.pipes[pipe.PipeId]=pipe;pipe.PipeId=0x01;d.usb.pipes[pipe.PipeId]=pipe;
    clock.struct_size=sizeof clock;clock.reference_hz=40000000;
    uint32_t rates[]={983040000,245760000,122880000,61440000,30720000,30720000};
    std::copy(rates,rates+6,clock.rx_hz);std::copy(rates,rates+6,clock.tx_hz);
    for(unsigned dir=0;dir<2;++dir){auto& c=fir[dir];c.struct_size=sizeof c;c.direction=dir;c.channel_mask=3;c.factor=1;c.tap_count=16;c.verified=1;c.crc32=crc(c.coefficients,c.tap_count);}
}
void FakeUsb::complete_rx(){
    if(!rx_enabled||hold_rx)return;
    // Preserve endpoint byte order even though OVERLAPPED pointer order differs.
    std::vector<decltype(requests)::value_type*> ordered;
    for(auto& item:requests)if(item.second.endpoint==0x81&&!item.second.completed)ordered.push_back(&item);
    std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a->second.sequence<b->second.sequence;});
    for(auto item:ordered){auto& r=item->second;
        for(ULONG i=0;i<r.size;++i,++rx_position)r.data[i]=rx_position<och::dsp::Framer::header_bytes?static_cast<UCHAR>(rx_position):0x55;
        r.completed=true;SetEvent(item->first->hEvent);
    }
}
std::vector<std::string> FakeUsb::command_log(){std::lock_guard<std::mutex> lock(mutex);return commands;}
std::vector<FakeUsb::Write> FakeUsb::write_log(){std::lock_guard<std::mutex> lock(mutex);return writes;}
void FakeUsb::clear_log(){std::lock_guard<std::mutex> lock(mutex);commands.clear();writes.clear();}
void FakeUsb::freeze_rx(bool value){std::lock_guard<std::mutex> lock(mutex);hold_rx=value;complete_rx();}
void FakeUsb::freeze_tx(bool value){std::lock_guard<std::mutex> lock(mutex);hold_tx=value;
    if(!hold_tx)for(auto& item:requests)if(item.second.endpoint==1){item.second.completed=true;SetEvent(item.first->hEvent);}
}
bool FakeUsb::inject_header_only(){
    std::vector<uint8_t> header(och::dsp::Framer::header_bytes);
    for(size_t i=0;i<header.size();++i)header[i]=static_cast<uint8_t>(i);
    return inject_rx(header);
}
bool FakeUsb::inject_rx(const std::vector<uint8_t>& bytes){
    std::lock_guard<std::mutex> lock(mutex);
    auto selected=requests.end();
    for(auto it=requests.begin();it!=requests.end();++it)if(it->second.endpoint==0x81&&!it->second.completed&&
        (selected==requests.end()||it->second.sequence<selected->second.sequence))selected=it;
    if(selected==requests.end())return false;
    auto& r=selected->second;
    if(bytes.size()>r.size)throw std::runtime_error("injected data exceeds USB request");
    r.size=static_cast<ULONG>(bytes.size());memcpy(r.data,bytes.data(),bytes.size());
    r.completed=true;SetEvent(selected->first->hEvent);return true;
}
void FakeUsb::fail_next(const std::string& prefix){std::lock_guard<std::mutex> lock(mutex);fail_command=prefix;}
void FakeUsb::pause_on(const std::string& prefix){
    std::lock_guard<std::mutex> lock(gate_mutex);paused_command=prefix;command_waiting=command_released=false;
}
bool FakeUsb::wait_command(unsigned timeout_ms){
    std::unique_lock<std::mutex> lock(gate_mutex);return gate_cv.wait_for(lock,std::chrono::milliseconds(timeout_ms),[&]{return command_waiting;});
}
void FakeUsb::release_command(){std::lock_guard<std::mutex> lock(gate_mutex);command_released=true;gate_cv.notify_all();}
std::string FakeUsb::execute(const std::string& cmd){
    {
        // The gate never holds the USB lock: real workers and producers must
        // remain able to progress during a slow firmware command round-trip.
        std::unique_lock<std::mutex> lock(gate_mutex);
        if(!paused_command.empty()&&cmd.rfind(paused_command,0)==0){
            command_waiting=true;gate_cv.notify_all();gate_cv.wait(lock,[&]{return command_released;});paused_command.clear();
        }
    }
    std::lock_guard<std::mutex> lock(mutex);commands.push_back(cmd);
    if(!fail_command.empty()&&cmd.rfind(fail_command,0)==0){fail_command.clear();throw Failure(OCH_PROTOCOL,"injected command failure: "+cmd);}
    if(cmd=="usb_stop!"){rx_enabled=tx_enabled=false;return {};}
    if(cmd.rfind("usb_data_en=",0)==0){auto n=numbers(cmd.substr(12));rx_enabled=n.at(0)!=0;tx_enabled=n.at(1)!=0;complete_rx();return {};}
    if(cmd.rfind("tx_enable=",0)==0){tx_enabled=cmd.back()=='1';return {};}
    if(cmd=="system_info?")return std::string("clock_config=")+(clock_supported?"1":"0")+
        " fir_upload="+(fir_supported?"1":"0")+(correction_supported?" rx_correction=1":"")+
        (lo_supported?" external_lo=1":"")+(reference_supported?" adf4002=1":"")+(reference_source_supported?" ref_source=1":"")+(pps_supported?" pps_monitor=1":"")+(board_supported?" board_rgb=1 fan_control=1 core_temp=1":"")+
        (iq_format_supported?std::string(" iq_format=1 iq8_2t2r=")+(iq8_supported?"1":"0"):std::string());
    if(cmd=="iq_format?") {
        require(iq_format_supported,OCH_UNSUPPORTED,"unknown IQ format command");
        if(!iq_format_reply_override.empty())return iq_format_reply_override;
        return "iq_format="+std::to_string(iq_bits)+" requested="+std::to_string(iq_bits)+" applied="+std::to_string(iq_bits)+
            " supported="+(iq8_supported?"12,8":"12")+" cfg_busy=0 cfg_error=0 verified=1 scope=2tx2rx";
    }
    if(cmd.rfind("iq_format=",0)==0) {
        require(iq_format_supported,OCH_UNSUPPORTED,"unknown IQ format command");const auto bits=std::stoul(cmd.substr(10));
        require(!rx_enabled&&!tx_enabled,OCH_BUSY,"IQ format switch while streaming");
        require(bits==12 || (bits==8&&iq8_supported&&rx_mask==3&&tx_mask==3&&tx_source==1),OCH_INVALID,"invalid IQ format scope");
        if(!ignore_iq_format_write)iq_bits=static_cast<unsigned>(bits);
        return {};
    }
    if(cmd.rfind("rx_channel=",0)==0 || cmd.rfind("tx_channel=",0)==0) {
        const auto mask=std::stoul(cmd.substr(11));require(iq_bits!=8||mask==3,OCH_INVALID,"IQ8 needs dual channel masks");
        (cmd[0]=='r'?rx_mask:tx_mask)=static_cast<unsigned>(mask);return {};
    }
    if(cmd.rfind("tx_source=",0)==0) {
        const auto source=std::stoul(cmd.substr(10));require(iq_bits!=8||source==1,OCH_INVALID,"IQ8 needs USB TX source");
        tx_source=static_cast<unsigned>(source);return {};
    }
    if(cmd=="rgb_status?") {
        require(board_supported,OCH_PROTOCOL,"unknown RGB command");
        if(!rgb_reply_override.empty())return rgb_reply_override;
        return std::string("rgb_status available=1 enabled=")+(rgb_enabled?"1 brightness_pct=25 state=idle red=0 green=64":"0 brightness_pct=25 state=off red=0 green=0")+" blue=0 blink_hz=0";
    }
    if(cmd=="fan_status?") {
        require(board_supported,OCH_PROTOCOL,"unknown fan command");
        if(!fan_reply_override.empty())return fan_reply_override;
        return std::string("fan_status available=1 enabled=")+(fan_enabled?"1 frequency_hz=25000 duty_permille=300 target_permille=300":"0 frequency_hz=25000 duty_permille=0 target_permille=0")+" boost=0 failsafe=0 temp_valid=1 temp_mC=45000 temp_age_ms=100 overtemp=0 source=zynq";
    }
    if(cmd=="core_temp?") {
        require(board_supported,OCH_PROTOCOL,"unknown temperature command");
        if(!core_reply_override.empty())return core_reply_override;
        return "core_temp source=zynq valid=1 milli_celsius=45000 age_ms=100 overtemp=0";
    }
    if(cmd.rfind("rgb_enable=",0)==0||cmd.rfind("fan_enable=",0)==0) {
        require(board_supported,OCH_PROTOCOL,"unknown board command");
        if(!ignore_board_write)(cmd[0]=='r'?rgb_enabled:fan_enabled)=cmd.back()=='1';
        return {};
    }
    if(cmd=="pps_status?") {
        require(pps_supported,OCH_PROTOCOL,"unknown PPS command");
        if(!pps_reply_override.empty())return pps_reply_override;
        return std::string("available=1 enabled=")+(pps_enabled?"1 state=no_signal":"0 state=disabled")+
            " signal_present=0 frequency_valid=0 nominal_hz=122880000 measured_hz=0 error_ppm=unknown pulse_count=0 invalid_count=0 age_ms=4294967295 raw_counter=0 hardware_timeout=1 monitor_only=1 adjustment=0 clock=DATA_CLK expected_pps_hz=1";
    }
    if(cmd.rfind("pps_input_en=",0)==0) {
        require(pps_supported,OCH_PROTOCOL,"unknown PPS command");
        if(!ignore_pps_write)pps_enabled=cmd.back()=='1';
        return {};
    }
    if(cmd=="rx_lo_source?"||cmd=="tx_lo_source?") {
        require(lo_supported,OCH_PROTOCOL,"unknown LO source command");if(!lo_reply_override.empty())return lo_reply_override;
        const auto dir=cmd[0]=='r'?0:1;
        return cmd.substr(0,12)+"="+std::to_string(lo_source[dir])+" frequency_hz="+std::to_string(lo_frequency[dir])+" verified=1 signal_present=unverified";
    }
    if(cmd.rfind("rx_lo_source=",0)==0||cmd.rfind("tx_lo_source=",0)==0) {
        require(lo_supported,OCH_PROTOCOL,"unknown LO source command");require(!rx_enabled&&!tx_enabled,OCH_BUSY,"LO source write while streaming");
        const auto dir=cmd[0]=='r'?0:1;const unsigned source=cmd.back()=='1';
        if(!ignore_lo_write){lo_source[dir]=source;if(!source){lo_frequency[dir]=internal_lo_frequency[dir];
            if(!dir&&clamp_rx_gain_on_internal)for(auto& gain:rx_gain)gain=std::min(gain,60);}}
        if(corrupt_lo_partner)lo_source[1-dir]=!lo_source[1-dir];
        return {};
    }
    if(cmd.rfind("rx_lo_freq=",0)==0||cmd.rfind("tx_lo_freq=",0)==0) {
        const auto dir=cmd[0]=='r'?0:1;lo_frequency[dir]=static_cast<uint64_t>(std::llround(std::stod(cmd.substr(11))*1000000));
        if(!lo_source[dir])internal_lo_frequency[dir]=lo_frequency[dir];
        return {};
    }
    if(cmd=="rx1_rf_gain?"||cmd=="rx2_rf_gain?")return cmd.substr(0,11)+"="+std::to_string(rx_gain[cmd[2]=='1'?0:1]);
    if(cmd.rfind("rx1_rf_gain=",0)==0||cmd.rfind("rx2_rf_gain=",0)==0){rx_gain[cmd[2]=='1'?0:1]=std::stoi(cmd.substr(12));return {};}
    if(cmd=="adf4002_status?") {
        require(reference_supported,OCH_PROTOCOL,"unknown ADF4002 command");if(!reference_reply_override.empty())return reference_reply_override;
        return "source="+std::to_string(reference_source)+" source_known="+std::to_string(reference_source_known)+" initialized=1 gpio_verified="+
            std::to_string(reference_gpio_verified)+" chip_verified=0 lock_known=0 locked=-1 led_known=0 led_on=-1 led_mode="+
            std::to_string(reference_source)+" reference_hz=10000000 oscillator_hz=40000000 r_counter=1 n_counter=4 last_error="+std::to_string(reference_last_error);
    }
    if(cmd.rfind("clock_ref_source=",0)==0) {
        require(reference_supported&&reference_source_supported,OCH_PROTOCOL,"unknown reference source command");
        require(!rx_enabled&&!tx_enabled,OCH_BUSY,"reference source write while streaming");
        if(!ignore_reference_write){reference_source=cmd.back()=='1';reference_source_known=reference_gpio_verified=!reference_write_leaves_unknown;reference_last_error=reference_write_leaves_unknown?-5:0;}
        return {};
    }
    if(cmd=="adf4002_self_test!") {
        require(reference_supported,OCH_PROTOCOL,"unknown ADF4002 command");require(!rx_enabled&&!tx_enabled,OCH_BUSY,"reference test while streaming");
        if(corrupt_reference_restore)reference_source=!reference_source;
        if(reference_test_leaves_unknown){reference_source_known=reference_gpio_verified=false;reference_last_error=-5;}
        return reference_test_override.empty()?"gpio_verified=1 chip_verified=0 visual_confirmation_required=1":reference_test_override;
    }
    if(cmd=="rx_dc_correction?"||cmd=="rx_iq_correction?"){
        if(!correction_supported)throw Failure(OCH_PROTOCOL,"unknown correction command");
        if(!correction_reply_override.empty())return correction_reply_override;
        const bool dc=cmd=="rx_dc_correction?";
        return std::string(dc?"rx_dc_correction=":"rx_iq_correction=")+std::to_string(dc?dc_correction:iq_correction);
    }
    if(cmd.rfind("rx_dc_correction=",0)==0||cmd.rfind("rx_iq_correction=",0)==0){
        require(correction_supported,OCH_PROTOCOL,"unknown correction command");
        require(!rx_enabled&&!tx_enabled,OCH_BUSY,"correction write while streaming");
        bool& target=cmd[3]=='d'?dc_correction:iq_correction;
        bool& partner=cmd[3]=='d'?iq_correction:dc_correction;
        if(!ignore_correction_write)target=cmd.back()=='1';
        if(corrupt_correction_partner)partner=!partner;
        return {};
    }
    if(cmd=="usb_status?")return "tx_main_fifo_groups="+std::to_string(tx_main_fifo_groups);
    if(cmd=="clock_chain?")return "ref="+std::to_string(clock.reference_hz)+" rx="+csv(clock.rx_hz)+" tx="+csv(clock.tx_hz);
    if(cmd.rfind("clock_sample_rate=",0)==0){clock.rx_hz[5]=clock.tx_hz[5]=static_cast<uint32_t>(std::stoul(cmd.substr(18)));return {};}
    if(cmd.rfind("clock_rx=",0)==0||cmd.rfind("clock_tx=",0)==0){auto n=numbers(cmd.substr(9));auto p=cmd[6]=='r'?clock.rx_hz:clock.tx_hz;for(unsigned i=0;i<6;++i)p[i]=static_cast<uint32_t>(n.at(i+1));return {};}
    if(cmd=="rx_fir_config?"||cmd=="tx_fir_config?"){const auto& c=fir[cmd[0]=='r'?0:1];return "factor="+std::to_string(c.factor)+" taps="+std::to_string(c.tap_count)+" gain_dB="+std::to_string(c.gain_db)+" enabled="+std::to_string(c.enabled)+" verified=1 crc32="+std::to_string(c.crc32);}
    if(cmd.rfind("rx_fir_read=",0)==0||cmd.rfind("tx_fir_read=",0)==0){auto n=numbers(cmd.substr(12));auto& c=fir[cmd[0]=='r'?0:1];std::string out=cmd.substr(0,12)+std::to_string(n.at(0));
        for(long long i=0;i<n.at(1);++i)out+=' '+std::to_string(c.coefficients[n.at(0)+i]);
        return out;}
    if(cmd.rfind("rx_fir_begin=",0)==0||cmd.rfind("tx_fir_begin=",0)==0){uploading=cmd[0]=='r'?0:1;auto n=numbers(cmd.substr(13));auto& c=fir[uploading];c.tap_count=static_cast<uint32_t>(n.at(1));c.factor=static_cast<uint32_t>(n.at(2));c.gain_db=static_cast<int32_t>(n.at(3));return {};}
    if(cmd.rfind("fir_data=",0)==0){auto n=numbers(cmd.substr(9));for(size_t i=2;i<n.size();++i)fir[uploading].coefficients[n.at(1)+i-2]=static_cast<int16_t>(n[i]);return {};}
    if(cmd.rfind("fir_commit=",0)==0){auto n=numbers(cmd.substr(11));auto& c=fir[uploading];c.enabled=static_cast<uint32_t>(n.at(1));c.crc32=crc(c.coefficients,c.tap_count);return {};}
    if(cmd.rfind("fir_enable=",0)==0){auto n=numbers(cmd.substr(11));fir[0].enabled=static_cast<uint32_t>(n.at(0));fir[1].enabled=static_cast<uint32_t>(n.at(1));return {};}
    return {};
}
}

// Only the physical device boundary is replaced. Configuration/stream decisions
// are supplied by production sources, never duplicated in this test double.
och_device::~och_device()=default;
void och_device::open(const och::transport::DeviceInfo&){throw Failure(OCH_IO,"test attempted physical device open");}
std::string och_device::command(const std::string& cmd,uint32_t){return fake(usb.h).execute(cmd);}
void och_device::reset_transport(){
    require(!rx_on&&!tx_on,OCH_BUSY,"test transport reset while active");auto& f=fake(usb.h);std::lock_guard<std::mutex> lock(f.mutex);
    ++f.resets;f.rx_position=0;transport_dirty=false;
}
void och_device::emit(uint32_t type,const std::string& message,uint64_t ep){
    och_event e{};e.struct_size=sizeof e;e.type=type;e.epoch=ep;e.sequence=++event_seq;e.host_monotonic_ns=now_ns();copy_text(e.message,sizeof e.message,message);
    std::lock_guard<std::mutex> lock(events_mutex);events.push_back(e);events_cv.notify_all();
}
namespace och::core {
std::string serial_of(const och::transport::DeviceInfo&){return "fake-device";}
void fill_info(och_device_info& info,const och::transport::DeviceInfo&){info={};info.struct_size=sizeof info;info.abi_version=OCH_ABI_VERSION;}
}
