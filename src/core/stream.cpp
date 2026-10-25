#include "internal.hpp"
#include <cstdio>
using namespace och::core;
using namespace och::transport;

och_stream::och_stream(och_device* d,const och_stream_args& a):dev(d),args(a),
    nchan(channels(a.direction==OCH_RX?d->config.rx_mask:d->config.tx_mask)),wire_bytes(iq_bits(d->config)==8?2:3),group(nchan*wire_bytes),
    ring(std::max<size_t>(2,a.host_buffer_bytes/a.transfer_bytes),a.transfer_bytes),framer(nchan,wire_bytes),
    stop_event(CreateEventW(nullptr,TRUE,FALSE,nullptr)) {
    require(stop_event.h!=nullptr,OCH_IO,"stream stop event allocation failed");
    slots.reserve(args.usb_queue_depth);
    for(unsigned i=0;i<args.usb_queue_depth;++i)slots.push_back(std::make_unique<Slot>(dev->usb,args.transfer_bytes));
}
och_stream::~och_stream(){stop_requested=true;SetEvent(stop_event.h);ring.wake();if(worker.joinable())worker.join();}
void och_stream::fail(const std::string& s){{std::lock_guard<std::mutex> lock(failure_mutex);failure=s;}++errors;dev->emit(OCH_EVENT_TRANSPORT,s);}
std::string och_stream::error(){std::lock_guard<std::mutex> lock(failure_mutex);return failure;}
void och_stream::sink(void* self,const uint8_t* p,size_t n,uint64_t ep,bool begin){static_cast<och_stream*>(self)->rx_data(p,n,ep,begin);}
void och_stream::publish_rx() {
    if(!rx_filled)return;
    ring.publish(rx_filled,rx_meta);rx_filled=0;
    auto bytes=ring.queued();auto prev=peak.load();if(bytes>prev)peak=bytes;
}
void och_stream::rx_data(const uint8_t* p,size_t n,uint64_t ep,bool begin) {
    const auto local_epoch=ep;ep+=epoch_base;
    if(begin || ep!=epoch) {
        publish_rx();epoch=ep;index=0;headers=ep;resyncs=ep?ep-1:0;
        flags_pending=(flags_pending&OCH_META_HOST_OVERFLOW)|OCH_META_START|
            (local_epoch>1?(OCH_META_DISCONTINUITY|OCH_META_DEVICE_OVERFLOW):0);
        if(reconfigured_rx){flags_pending|=OCH_META_RECONFIGURED|OCH_META_DISCONTINUITY;reconfigured_rx=false;}
        dev->emit(OCH_EVENT_RX_SYNC,"RX preamble complete; new epoch="+std::to_string(ep),ep);
    }
    if(retuned.exchange(false)){publish_rx();flags_pending|=OCH_META_RECONFIGURED|OCH_META_DISCONTINUITY;}
    while(n) {
        auto b=ring.write();
        if(!b){
            auto lost=n/group;host_dropped+=lost;dropped_pending+=lost;index+=lost;
            flags_pending|=OCH_META_HOST_OVERFLOW|OCH_META_DISCONTINUITY;
            auto now=now_ns();if(now-last_drop_notice>=500000000ULL){dev->emit(OCH_EVENT_HOST_OVERFLOW,"RX host ring full; new samples dropped (see stats)",ep);last_drop_notice=now;}
            return;
        }
        if(!rx_filled){rx_meta={};rx_meta.struct_size=sizeof rx_meta;rx_meta.flags=flags_pending|OCH_META_TIME_UNKNOWN;rx_meta.epoch=ep;
            rx_meta.sample_index=index;rx_meta.host_dropped_samples=dropped_pending;rx_meta.device_dropped_samples=(flags_pending&(OCH_META_DEVICE_OVERFLOW|OCH_META_RECONFIGURED))?-1:0;
            rx_meta.host_monotonic_ns=now_ns();flags_pending=0;dropped_pending=0;}
        size_t take=std::min(n,args.transfer_bytes-rx_filled);take-=take%group;
        memcpy(b->data.get()+rx_filled,p,take);rx_filled+=take;p+=take;n-=take;index+=take/group;
        if(rx_filled==args.transfer_bytes)publish_rx();
    }
}
bool och_stream::fill_tx(Slot* s) {
    while(s->used<args.transfer_bytes) {
        auto b=ring.read();if(!b)break;
        auto n=std::min(b->size-tx_offset,args.transfer_bytes-s->used);
        memcpy(s->bytes.get()+s->used,b->data.get()+tx_offset,n);s->used+=n;tx_offset+=n;assembling=s->used;
        if(tx_offset==b->size){ring.release();tx_offset=0;}
    }
    assembling=s->used;
    if(s->used==args.transfer_bytes)return true;
    if(flushing && s->used){
        auto size=((s->used+49151)/49152)*49152;
        memset(s->bytes.get()+s->used,0,size-s->used);padding+=(size-s->used)/group;s->used=size;assembling=size;return true;
    }
    return false;
}
bool och_stream::submit(Slot* s) {
    auto& u=dev->usb;UCHAR ep=args.direction==OCH_RX?0x81:0x01;
    if(args.direction==OCH_RX)s->used=args.transfer_bytes;
    ResetEvent(s->ov.hEvent);BOOL ok=args.direction==OCH_RX?
        u.api.read(u.h,ep,s->bytes.get(),static_cast<ULONG>(s->used),nullptr,&s->ov):
        u.api.write(u.h,ep,s->bytes.get(),static_cast<ULONG>(s->used),nullptr,&s->ov);
    if(!ok && GetLastError()!=ERROR_IO_PENDING)throw Failure(OCH_IO,"USB streaming submission failed: "+utf8(syserr(GetLastError())));
    if(ok)SetEvent(s->ov.hEvent);
    s->pending=true;active.push_back(s);pending=active.size();assembling=0;return true;
}
void och_stream::cancel_all() {
    auto& u=dev->usb;UCHAR ep=args.direction==OCH_RX?0x81:0x01;
    if(!active.empty()) {
        u.api.abort(u.h,ep);
        while(!active.empty()){
            auto* s=active.front();active.pop_front();
            if(args.direction==OCH_TX)cancelled_bytes+=s->used;
#ifdef _WIN32
            CancelIoEx(u.file,&s->ov);
#endif
            ULONG n=0;u.api.result(u.h,&s->ov,&n,TRUE);s->pending=false;
        }
        active.clear();pending=0;
    }
    UCHAR off=FALSE;u.api.setPolicy(u.h,ep,RAW_IO,sizeof off,&off);
}
void och_stream::run() {
    auto& u=dev->usb;UCHAR ep=args.direction==OCH_RX?0x81:0x01;
    och::core::FixedQueue<Slot*,128> free;for(auto& s:slots)free.push_back(s.get());bool starved_notice=false;
    try {
#ifdef _WIN32
        SetThreadPriority(GetCurrentThread(),THREAD_PRIORITY_ABOVE_NORMAL);
#endif
        require(args.transfer_bytes<=u.maximum(ep),OCH_INVALID,"USB transfer block exceeds driver maximum");
        u.timeout(ep,0);u.policy(ep,RAW_IO,FALSE);
        if(args.direction==OCH_RX){u.policy(ep,IGNORE_SHORT_PACKETS,FALSE);u.policy(ep,ALLOW_PARTIAL_READS,TRUE);u.policy(ep,AUTO_FLUSH,FALSE);}
        else {u.policy(ep,SHORT_PACKET_TERMINATE,FALSE);u.parallelLimit(ep,args.usb_queue_depth);}
        u.policy(ep,RAW_IO,TRUE);
        while(!stop_requested) {
            while(!free.empty() && !stop_requested) {
                Slot* s=free.front();if(args.direction==OCH_TX && !fill_tx(s))break;
                free.pop_front();submit(s);starved_notice=false;
            }
            queued=true;
            if(active.empty()){
                if(args.direction==OCH_TX && started && !flushing && !starved_notice){++starved;dev->emit(OCH_EVENT_TX_STARVED,"TX application queue empty; producer must resume");starved_notice=true;}
                ring.wait_read(2,stop_requested);continue;
            }
            Slot* s=active.front();HANDLE events[2]={stop_event.h,s->ov.hEvent};
            DWORD w=WaitForMultipleObjects(2,events,FALSE,2);
            if(w==WAIT_OBJECT_0)break;
            if(w==WAIT_TIMEOUT)continue;
            require(w==WAIT_OBJECT_0+1,OCH_IO,"USB event wait failed");
            ULONG n=0;
            const auto completed=u.api.result(u.h,&s->ov,&n,FALSE);
            require(completed!=FALSE,OCH_IO,"USB transfer failed: "+utf8(syserr(GetLastError())));
            require(n<=s->used,OCH_IO,"driver reported invalid USB completion length");
            if(args.direction==OCH_TX)require(n==s->used,OCH_IO,"short TX completion; byte order cannot be retried safely");
            active.pop_front();s->pending=false;pending=active.size();usb_bytes+=n;
            if(args.direction==OCH_RX){
                framer.feed(s->bytes.get(),n,&och_stream::sink,this);publish_rx();
                const auto complete=epoch_base+framer.epoch();headers=complete;resyncs=complete?complete-1:0;
            }
            s->used=0;free.push_back(s);
        }
    }catch(const std::exception& e){fail(e.what());}
    cancel_all();ended_ns=now_ns();running=false;stop_requested=true;ring.wake();
}
void och_stream::start() {
    require(!ever_started && !stop_requested,OCH_BUSY,"stream is single-use; destroy and create a new stream after stop");
    require(dev->configured,OCH_INVALID,"configure the device before starting streams");
    if(dev->transport_dirty){require(!dev->rx_on&&!dev->tx_on,OCH_BUSY,"stop both directions before reopening a stream (CH32 buffer cleanup)");dev->reset_transport();}
    if(args.direction==OCH_TX)require(ring.queued()>=args.transfer_bytes*2ULL,OCH_BUSY,"queue at least two transfer blocks of TX samples before start");
    if(args.direction==OCH_TX){dev->command("usb_data_en="+std::to_string(dev->rx_on?1:0)+" 1");dev->tx_on=true;}
    // Producers may already be waiting on a full prefill ring. Publish running
    // first so they cannot mistake the startup transition for a stopped worker.
    running=true;ever_started=true;if(!started_ns)started_ns=now_ns();
    try {
        worker=std::thread([this]{run();});
        auto deadline=now_ns()+5000000000ULL;
        while(!queued && running && now_ns()<deadline)Sleep(1);
        require(queued&&running,OCH_IO,"USB queue startup failed: "+error());
        if(args.direction==OCH_RX){dev->command("usb_data_en=1 "+std::to_string(dev->tx_on?1:0));dev->rx_on=true;}
        else {
            // IQ8's first 16 KiB packet fills about 2046 raw FIFO groups.
            // Waiting for the legacy 2700 blocks the second packet before
            // TX_RUN drains the FIFO. Match the SDK's IQ8 first-packet gate.
            bool filled=false;unsigned threshold=wire_bytes==2?1536:(dev->config.tx_mask==3?2700:1350);
            while(now_ns()<deadline && running){
                auto reply=dev->command("usb_status?");auto i=reply.find("tx_main_fifo_groups=");
                if(i!=std::string::npos){unsigned groups=0;auto first=reply.c_str()+i+20;
                    if(sscanf(first,"%u",&groups)==1 && groups>=threshold){filled=true;break;}}
                Sleep(5);
            }
            require(filled,OCH_TIMEOUT,"TX prefill did not reach FPGA threshold: "+error());dev->command("tx_enable=1");
        }
        require(running && !stop_requested,OCH_IO,"USB worker stopped during startup: "+error());
        started=true;
    }catch(...){
        try{if(args.direction==OCH_RX)dev->command("usb_data_en=0 "+std::to_string(dev->tx_on?1:0));else dev->command("tx_enable=0");}catch(...){}
        if(args.direction==OCH_RX)dev->rx_on=false;else dev->tx_on=false;
        stop_requested=true;SetEvent(stop_event.h);ring.wake();if(worker.joinable())worker.join();
        running=false;ended_ns=now_ns();dev->transport_dirty=true;throw;
    }
}
void och_stream::flush(uint32_t timeout) {
    require(!reconfiguring,OCH_TIMEOUT,"TX flush interrupted by reconfiguration");
    require(args.direction==OCH_TX && started,OCH_INVALID,"flush requires a started TX stream");
    require(running && !stop_requested,OCH_IO,"TX worker stopped before flush: "+error());
    require(!flushing.exchange(true),OCH_BUSY,"TX flush already in progress");
    auto until=now_ns()+uint64_t(timeout)*1000000;
    try {
        while(ring.queued() || assembling || pending){
            require(!reconfiguring,OCH_TIMEOUT,"TX flush interrupted by reconfiguration");
            require(running && !stop_requested,OCH_IO,"TX worker stopped: "+error());
            require(now_ns()<until,OCH_TIMEOUT,"TX flush timed out");Sleep(1);
        }
        require(!reconfiguring,OCH_TIMEOUT,"TX flush interrupted by reconfiguration");
        require(running && !stop_requested,OCH_IO,"TX flush was interrupted: "+error());
    }catch(...){flushing=false;throw;}
    flushing=false;
}
void och_stream::stop(uint32_t timeout) {
    if(!ever_started){stop_requested=true;ring.wake();return;}
    std::string issue;
    try {
        if(args.direction==OCH_RX && dev->rx_on){dev->command("usb_data_en=0 "+std::to_string(dev->tx_on?1:0),timeout?timeout:3000);dev->rx_on=false;}
        if(args.direction==OCH_TX && dev->tx_on){dev->command("tx_enable=0",timeout?timeout:3000);dev->tx_on=false;}
    }catch(const std::exception& e){issue=e.what();}
    halt();
    dev->transport_dirty=true;
    if(!issue.empty())throw Failure(OCH_IO,"stop completed with an error: "+issue);
}

void och_stream::halt() {
    started=false;stop_requested=true;SetEvent(stop_event.h);ring.wake();
    if(worker.joinable())worker.join();
    running=false;
    if(started_ns)ended_ns=now_ns();
}

// Both the USB worker and the application IO endpoint are quiescent. Large
// ring/USB allocations and the public stream pointer deliberately survive.
void och_stream::prepare_restart(bool receive_gap) {
    epoch_base+=framer.epoch();framer.reset();
    reconfigured_rx=reconfigured_rx || (args.direction==OCH_RX && receive_gap);
    ring.reset();recv_offset=rx_filled=tx_offset=0;assembling=0;rx_meta={};
    active.clear();pending=0;
    for(auto& s:slots) {
        s->used=0;s->pending=false;s->ov={};s->ov.hEvent=s->event.h;ResetEvent(s->event.h);
    }
    ResetEvent(stop_event.h);
    queued=false;started=false;running=false;flushing=false;ever_started=false;stop_requested=false;
}
