#include "internal.hpp"
using namespace och::core;
using namespace och::transport;

och_power_status och_device::get_power_status(uint32_t timeout_ms) {
    require(timeout_ms<=2000,OCH_INVALID,"power status timeout must be 0..2000 ms");
    const bool present=usb.has(0x83);
    // Diagnostics must not introduce USB traffic while a stream exists.
    if(present && timeout_ms && !rx && !tx) {
        usb.policy(0x83,RAW_IO,FALSE);
        usb.policy(0x83,ALLOW_PARTIAL_READS,TRUE);
        usb.policy(0x83,AUTO_FLUSH,FALSE);
        Handle event(CreateEventW(nullptr,TRUE,FALSE,nullptr));
        require(event.h!=nullptr,OCH_IO,"power status event allocation failed");
        std::array<uint8_t,4096> bytes{};
        const uint64_t deadline=now_ns()+uint64_t(timeout_ms)*1000000;
        size_t total=0;
        while(total<16384) {
            const auto now=now_ns();if(now>=deadline)break;
            const auto remaining=static_cast<ULONG>(std::max<uint64_t>(1,(deadline-now)/1000000));
            usb.timeout(0x83,std::min<ULONG>(remaining,25));
            ResetEvent(event.h);OVERLAPPED ov{};ov.hEvent=event.h;ULONG count=0;
            const auto requested=static_cast<ULONG>(std::min(bytes.size(),size_t(16384)-total));
            BOOL ok=usb.api.read(usb.h,0x83,bytes.data(),requested,&count,&ov);
            const auto submit_error=ok?ERROR_SUCCESS:GetLastError();
            if(!ok && submit_error!=ERROR_IO_PENDING && submit_error!=ERROR_SEM_TIMEOUT)
                throw Failure(OCH_IO,"power log submit: "+utf8(syserr(submit_error)));
            if(!ok && submit_error==ERROR_IO_PENDING && !usb.api.result(usb.h,&ov,&count,TRUE)) {
                const auto code=GetLastError();
                if(code!=ERROR_SEM_TIMEOUT)
                    throw Failure(OCH_IO,"power log completion: "+utf8(syserr(code)));
                count=0;
            }
            if(!ok && submit_error==ERROR_SEM_TIMEOUT)count=0;
            require(count<=requested,OCH_PROTOCOL,"power log transfer length exceeds request");
            if(count) {power_log.feed(bytes.data(),count);total+=count;}
            else if(power_log.status().snapshot_valid || power_log.status().conflicting_snapshots)break;
        }
    }
    auto snapshot=power_log.status();snapshot.log_endpoint_present=present?1u:0u;
    return snapshot;
}
