// Included in the production-runtime test fixture namespace. No hardware IO.
void iq8_enable(Fixture& f) {
    f.config.rx_mask=f.config.tx_mask=3;f.config.usb_iq_bits=8;
    ok(och_configure(&f.device,&f.config));CHECK(f.usb.iq_bits==8);f.usb.clear_log();
}
void iq8_codec_exhaustive() {
    using namespace och::dsp;
    constexpr size_t n=4096;
    std::vector<uint8_t> p12(n*3),wire(n*2),expanded(n*3),again(n*2);
    std::vector<int16_t> s16(n*2),decoded(n*2);std::vector<float> floats(n*2);
    for(unsigned i=0;i<n;++i) {
        unsigned q=4095-i;p12[i*3]=static_cast<uint8_t>(i>>4);
        p12[i*3+1]=static_cast<uint8_t>((i<<4)|(q>>8));p12[i*3+2]=static_cast<uint8_t>(q);
        s16[i*2]=static_cast<int16_t>(static_cast<uint16_t>((i<<4)|13));
        s16[i*2+1]=static_cast<int16_t>(static_cast<uint16_t>((q<<4)|3));
    }
    pack8_packed12(p12.data(),wire.data(),n);pack8_sc16(s16.data(),again.data(),n);CHECK(wire==again);
    unpack8_sc16(wire.data(),decoded.data(),n);unpack8_fc32(wire.data(),floats.data(),n);
    for(unsigned i=0;i<n;++i)for(unsigned c=0;c<2;++c) {
        unsigned original=c?4095-i:i;unsigned top=original>>4;
        CHECK(wire[i*2+c]==top);int value=top<128?int(top):int(top)-256;
        CHECK(decoded[i*2+c]==value*256);CHECK(floats[i*2+c]==value/128.0f);
    }
    unpack8_packed12(wire.data(),expanded.data(),n);pack8_packed12(expanded.data(),again.data(),n);CHECK(wire==again);
    for(size_t i=0;i<n;++i){CHECK(expanded[i*3]==wire[i*2]);CHECK(expanded[i*3+1]==(wire[i*2+1]>>4));CHECK(expanded[i*3+2]==uint8_t(wire[i*2+1]<<4));}
    // Empty tails and awkward lengths must not read or write outside the span.
    pack8_packed12(nullptr,nullptr,0);unpack8_sc16(nullptr,nullptr,0);pack8_fc32(nullptr,nullptr,0);
    for(size_t count:{size_t(1),size_t(7),size_t(8),size_t(9),size_t(31)}) {
        std::vector<uint8_t> guard(count*2+2,0xa5);pack8_sc16(s16.data(),guard.data()+1,count);
        CHECK(guard.front()==0xa5 && guard.back()==0xa5);CHECK(std::equal(guard.begin()+1,guard.end()-1,wire.begin()));
    }
}
void iq8_float_quantization() {
    using namespace och::dsp;
    std::vector<float> input;
    for(int v=-2048;v<=2047;++v)for(float fraction:{-0.5f,0.0f,0.5f})input.push_back((v+fraction)/2048.0f);
    for(float v:{-std::numeric_limits<float>::infinity(),std::numeric_limits<float>::infinity(),
        std::numeric_limits<float>::quiet_NaN(),-1.0f,1.0f,-0.0f,
        std::nextafter(0.5f/2048.0f,0.0f),std::nextafter(-0.5f/2048.0f,0.0f)})input.push_back(v);
    if(input.size()%2)input.push_back(0);
    std::vector<uint8_t> legacy(input.size()/2*3),expected(input.size()),actual(input.size());
    pack_fc32(input.data(),legacy.data(),input.size()/2);pack8_packed12(legacy.data(),expected.data(),input.size()/2);
    pack8_fc32(input.data(),actual.data(),input.size()/2);CHECK(actual==expected);
}
void iq8_framer_boundaries() {
    struct Sink {std::vector<uint8_t> bytes;std::vector<uint64_t> starts;};
    auto sink=[](void* ctx,const uint8_t* p,size_t n,uint64_t ep,bool start) {
        auto& s=*static_cast<Sink*>(ctx);CHECK(n%4==0);s.bytes.insert(s.bytes.end(),p,p+n);if(start)s.starts.push_back(ep);
    };
    std::vector<uint8_t> header(och::dsp::Framer::header_bytes);for(size_t i=0;i<header.size();++i)header[i]=static_cast<uint8_t>(i);
    std::vector<uint8_t> input(5,0x77);input.insert(input.end(),header.begin(),header.end());
    input.insert(input.end(),12,0x55);input.insert(input.end(),header.begin(),header.end());
    input.insert(input.end(),header.begin(),header.end());input.insert(input.end(),8,0x66);
    for(size_t chunk:{size_t(1),size_t(2),size_t(3),size_t(4),size_t(5),size_t(16383),size_t(16384),size_t(65536)}) {
        och::dsp::Framer framer(2,2);Sink s;
        for(size_t at=0;at<input.size();at+=chunk)framer.feed(input.data()+at,std::min(chunk,input.size()-at),sink,&s);
        CHECK(s.bytes.size()==20);CHECK(std::count(s.bytes.begin(),s.bytes.end(),0x55)==12);
        CHECK(s.starts==std::vector<uint64_t>({1,3}));CHECK(framer.epoch()==3);CHECK(framer.pending_bytes()==0);
        framer.reset();CHECK(!framer.synchronized());framer.feed(nullptr,0,sink,&s);
    }
    bool rejected=false;try{och::dsp::Framer bad(1,2);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
}
void iq8_configuration_contract() {
    CHECK(OCH_ABI_VERSION==1);CHECK(sizeof(och_config)==104);CHECK(offsetof(och_config,usb_iq_bits)==76);
    CHECK(sizeof(och_stream_args)==64);CHECK(och_sample_size(OCH_SC8)==2);CHECK(och_sample_size(OCH_PACKED12)==3);
    Fixture f(true,true);CHECK(f.config.usb_iq_bits==0);CHECK(f.usb.iq_bits==12);
    auto invalid=f.config;invalid.usb_iq_bits=8;CHECK(och_configure(&f.device,&invalid)==OCH_INVALID);CHECK(f.usb.command_log().empty());
    invalid.usb_iq_bits=10;CHECK(och_configure(&f.device,&invalid)==OCH_INVALID);
    f.config.rx_mask=f.config.tx_mask=3;f.config.usb_iq_bits=8;f.usb.iq8_supported=false;
    CHECK(och_configure(&f.device,&f.config)==OCH_UNSUPPORTED);CHECK(!contains(f.usb.command_log(),"usb_stop!"));
    CHECK(std::string(och_last_error()).find("FPGA bitstream")!=std::string::npos);
    f.usb.iq8_supported=true;f.usb.iq_format_supported=false;CHECK(och_configure(&f.device,&f.config)==OCH_UNSUPPORTED);
    f.config.usb_iq_bits=0;ok(och_configure(&f.device,&f.config));CHECK(f.device.configured);
    f.usb.iq_format_supported=true;iq8_enable(f);
    f.config.usb_iq_bits=12;f.config.rx_mask=1;f.config.tx_mask=0;ok(och_configure(&f.device,&f.config));
    const auto log=f.usb.command_log();auto format=std::find(log.begin(),log.end(),"iq_format=12"),mask=std::find(log.begin(),log.end(),"rx_channel=1");
    CHECK(format<mask);CHECK(f.usb.iq_bits==12);
    f.usb.clear_log();f.config.usb_iq_bits=0;ok(och_configure(&f.device,&f.config));CHECK(f.usb.command_log().empty());
    och_stream_args a{};och_default_stream_args(&a,OCH_RX);a.format=OCH_SC8;och_stream* stream=nullptr;
    CHECK(och_create_stream(&f.device,&a,&stream)==OCH_INVALID);CHECK(stream==nullptr);
}
void iq8_format_change_refused() {
    Fixture f(true,true);iq8_enable(f);auto* rx=f.create(OCH_RX,false,8,OCH_SC8);auto* tx=f.create(OCH_TX,false,8,OCH_SC8);
    CHECK(rx->group==4&&tx->group==4);f.usb.clear_log();auto next=f.config;next.usb_iq_bits=12;
    CHECK(och_configure(&f.device,&next)==OCH_BUSY);CHECK(f.usb.command_log().empty());
    next=f.config;next.rx_mask=1;CHECK(och_configure(&f.device,&next)==OCH_INVALID);
    ok(och_configure(&f.device,&f.config));CHECK(f.usb.command_log().empty());
}
void iq8_readback_failure() {
    Fixture f(true,true);f.config.rx_mask=f.config.tx_mask=3;f.config.usb_iq_bits=8;f.usb.ignore_iq_format_write=true;
    CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);CHECK(!f.device.configured&&!f.device.rx_on&&!f.device.tx_on);
    f.usb.ignore_iq_format_write=false;
    for(const char* reply:{"iq_format=12 requested=12 applied=12 supported=12,8 cfg_busy=1 cfg_error=0 verified=1 scope=2tx2rx",
        "iq_format=12 requested=12 applied=12 supported=12,8 cfg_busy=0 cfg_error=0 verified=0 scope=2tx2rx",
        "iq_format=12 requested=12 applied=12 supported=12,8 cfg_busy=0 cfg_error=0 verified=1 scope=2tx2rx applied=12",
        "iq_format=12 requested=12 applied=12junk supported=12,8 cfg_busy=0 cfg_error=0 verified=1 scope=2tx2rx"}) {
        f.usb.iq_format_reply_override=reply;CHECK(och_configure(&f.device,&f.config)==OCH_PROTOCOL);CHECK(!f.device.configured);
    }
    f.usb.iq_format_reply_override.clear();ok(och_configure(&f.device,&f.config));CHECK(f.device.configured&&f.usb.iq_bits==8);
}
void iq8_receive_formats() {
    const std::vector<uint8_t> wire={0x80,0xff,0x7f,0x00,0x01,0x02,0x81,0xfe,0x10,0x20,0x30,0x40};
    for(auto format:{OCH_SC8,OCH_PACKED12,OCH_SC16,OCH_FC32}) {
        Fixture f(true,true);iq8_enable(f);f.usb.freeze_rx(true);auto* rx=f.create(OCH_RX,true,8,format);
        std::vector<uint8_t> packet(och::dsp::Framer::header_bytes);for(size_t i=0;i<packet.size();++i)packet[i]=static_cast<uint8_t>(i);
        packet.insert(packet.end(),wire.begin(),wire.end());eventually([&]{return f.usb.inject_rx(packet);});
        alignas(float) uint8_t output[48]{};size_t count=0;och_metadata m{};m.struct_size=sizeof m;
        ok(och_recv(rx,output,1,&count,&m,1000));CHECK(count==1&&m.sample_index==0&&m.epoch==1&&(m.flags&OCH_META_START));
        if(format==OCH_SC8)CHECK(std::equal(output,output+4,wire.begin()));
        else if(format==OCH_PACKED12){const uint8_t expected[]={0x80,0x0f,0xf0,0x7f,0x00,0x00};CHECK(std::equal(output,output+6,expected));}
        else if(format==OCH_SC16){const auto* p=reinterpret_cast<const int16_t*>(output);CHECK(p[0]==-32768&&p[1]==-256&&p[2]==32512&&p[3]==0);}
        else {const auto* p=reinterpret_cast<const float*>(output);CHECK(p[0]==-1&&p[1]==-1.0f/128&&p[2]==127.0f/128&&p[3]==0);}
        const void* raw=nullptr;size_t bytes=0;ok(och_rx_acquire(rx,&raw,&bytes,&m,1000));CHECK(bytes==8&&m.sample_index==1);
        CHECK(std::equal(static_cast<const uint8_t*>(raw),static_cast<const uint8_t*>(raw)+bytes,wire.begin()+4));
        ok(och_rx_release(rx));och_stats st{};st.struct_size=sizeof st;ok(och_get_stats(rx,&st));CHECK(st.samples==3);
    }
}
void iq8_transmit_formats() {
    const uint8_t wire[]={0x80,0xff,0x7f,0x00,0x01,0x02,0x81,0xfe};
    const uint8_t packed[]={0x80,0x0f,0xff,0x7f,0xf0,0x0f,0x01,0xf0,0x2f,0x81,0xff,0xef};
    const int16_t words[]={-32768,-1,32767,255,511,767,-32257,-257};
    const float floats[]={-1,-1.0f/2048,2047.0f/2048,15.0f/2048,31.0f/2048,47.0f/2048,-2017.0f/2048,-17.0f/2048};
    for(auto format:{OCH_SC8,OCH_PACKED12,OCH_SC16,OCH_FC32}) {
        Fixture f(true,true);iq8_enable(f);auto* tx=f.create(OCH_TX,false,8,format);size_t count=0;
        const void* input=format==OCH_SC8?static_cast<const void*>(wire):format==OCH_PACKED12?static_cast<const void*>(packed):format==OCH_SC16?static_cast<const void*>(words):static_cast<const void*>(floats);
        ok(och_send(tx,input,2,&count,0));CHECK(count==2);const auto* b=tx->ring.read();CHECK(b&&b->size==8);
        CHECK(std::equal(b->data.get(),b->data.get()+8,wire));CHECK(tx->samples==2);
    }
}
void iq8_duplex_reconfigure() {
    Fixture f(true,true);iq8_enable(f);auto* rx=f.create(OCH_RX,true,8,OCH_SC8);auto* tx=f.create(OCH_TX,false,8,OCH_SC8);
    f.prefill(tx);ok(och_start(tx));const auto before=f.receive(rx);CHECK(before.epoch>0);
    f.change();CHECK(rx->running&&tx->running&&f.usb.iq_bits==8);CHECK(rx->group==4&&tx->group==4);
    const auto after=f.receive(rx);CHECK(after.epoch>before.epoch&&(after.flags&OCH_META_RECONFIGURED));
    och_stats st{};st.struct_size=sizeof st;ok(och_get_stats(tx,&st));CHECK(st.tx_padding_samples==4*block_bytes/4);
    CHECK(!contains(f.usb.command_log(),"iq_format="));
}
void iq8_cli_contract() {
    using namespace och_cli::commands;
    CHECK(translate("usb.iq-bits",{"8"},Operation::set)=="iq_format=8");
    CHECK(translate("usb.iq-bits",{"packed12"},Operation::set)=="iq_format=12");
    CHECK(translate("usb.iq-bits",{},Operation::query)=="iq_format?");
    bool rejected=false;try{translate("usb.iq-bits",{"10"},Operation::set);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    och_stats st{};std::ostringstream out;och_cli::presentation::stats(OCH_RX,st,2,30720000,out,2);
    CHECK(out.str().find("122.880")!=std::string::npos);CHECK(out.str().find("2 bytes x")!=std::string::npos);
    auto parse=[](std::vector<std::string> words) {
        std::vector<char*> argv;for(auto& word:words)argv.push_back(word.data());
        return och_cli::parse(static_cast<int>(argv.size()),argv.data());
    };
    for(const char* command:{"config","rx","tx","bench"}) {
        const auto options=parse({"och",command,"--iq-bits","8","--format","sc8","--channels","1,2"});
        CHECK(options.get("iq-bits")=="8"&&options.get("format")=="sc8");
        CHECK(parse({"och",command,"--iq-bits=12"}).get("iq-bits")=="12");
    }
    for(auto words:{std::vector<std::string>{"och","bench","--iq-bits"},
        std::vector<std::string>{"och","bench","--iq-bits=8","--iq-bits=12"}}) {
        rejected=false;try{parse(words);}catch(const std::invalid_argument&){rejected=true;}CHECK(rejected);
    }
}
void iq8_legacy_matrix_and_tail_padding() {
    for(unsigned r=0;r<4;++r)for(unsigned t=0;t<4;++t)if(r||t) {
        Fixture f(true,true);f.config.rx_mask=r;f.config.tx_mask=t;f.config.usb_iq_bits=12;
        ok(och_configure(&f.device,&f.config));CHECK(f.usb.iq_bits==12);
        if(r){auto* rx=f.create(OCH_RX,false);CHECK(rx->wire_bytes==3&&rx->group==(r==3?6u:3u));}
        if(t){auto* tx=f.create(OCH_TX,false);CHECK(tx->wire_bytes==3&&tx->group==(t==3?6u:3u));}
    }
    Fixture f(true,true);iq8_enable(f);auto* tx=f.create(OCH_TX,false,8,OCH_SC8);
    const uint8_t frame[]={0x12,0x34,0x9a,0xbc};size_t accepted=0;ok(och_send(tx,frame,1,&accepted,0));CHECK(accepted==1);
    tx->flushing=true;auto* slot=tx->slots.front().get();CHECK(tx->fill_tx(slot));CHECK(slot->used==49152);
    CHECK(std::equal(slot->bytes.get(),slot->bytes.get()+4,frame));
    CHECK(std::all_of(slot->bytes.get()+4,slot->bytes.get()+slot->used,[](uint8_t v){return v==0;}));
    CHECK(tx->padding==(49152-4)/4);tx->flushing=false;
}
void iq8_new_handle_resets_previous_format() {
    Fixture f(true,true);iq8_enable(f);f.device.configured=false;f.usb.clear_log();
    auto legacy=f.config;legacy.usb_iq_bits=0;legacy.rx_mask=1;legacy.tx_mask=0;
    ok(och_configure(&f.device,&legacy));CHECK(f.usb.iq_bits==12&&f.usb.rx_mask==1&&f.usb.tx_mask==0);
    auto log=f.usb.command_log();CHECK(std::find(log.begin(),log.end(),"iq_format=12")<std::find(log.begin(),log.end(),"rx_channel=1"));
}
void iq8_first_packet_prefill_starts_pending_usb() {
    Fixture f(true,true);iq8_enable(f);f.usb.tx_main_fifo_groups=2046;f.usb.freeze_tx(true);
    auto* tx=f.create(OCH_TX,false,8,OCH_SC8);f.prefill(tx);ok(och_start(tx));
    CHECK(tx->running&&tx->started&&tx->pending==2);CHECK(tx->usb_bytes==0);
    CHECK(contains(f.usb.command_log(),"tx_enable=1"));
}
