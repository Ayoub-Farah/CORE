/* SPDX-License-Identifier: Apache-2.0 */
#include "transition_core.h"
#include <string.h>
#define CHECK(x) do {if(!(x)) return __LINE__;} while(0)

namespace {
constexpr uint16_t MAINTENANCE=0x501,LOCAL=0x502,FLEET=0x503,MARKER=0x504;
struct Record {uint16_t key;int size;uint8_t data[320];};
struct Fixture {
    OtaTransitionConfig config;
    Record records[8];unsigned count;
    uint8_t eui[8],backup[32];
    bool confirmed,cut_before,confirm_fail,confirm_lies,erase_lies,write_lies;
    unsigned calls,cut,health_calls,health_fail_at;
    uint16_t read_error,write_keys[16];
};
void put16(uint8_t *p,uint16_t x) {p[0]=uint8_t(x);p[1]=uint8_t(x>>8);}
void put32(uint8_t *p,uint32_t x) {put16(p,uint16_t(x));put16(p+2,uint16_t(x>>16));}
void checksum(uint8_t *p,size_t n) {put32(p+n,ota_recovery_crc32(p,n));}
Record *find(Fixture &f,uint16_t key)
{for(unsigned i=0;i<f.count;++i) if(f.records[i].key==key) return &f.records[i];return nullptr;}
Record &add(Fixture &f,uint16_t key,size_t n)
{
    auto r=find(f,key);if(!r) {r=&f.records[f.count++];r->key=key;}
    r->size=int(n);memset(r->data,0,sizeof(r->data));return *r;
}
int read(void *context,uint16_t key,uint8_t *p,size_t n)
{
    auto &f=*static_cast<Fixture *>(context);if(f.read_error==key) return -5;
    auto r=find(f,key);if(!r || r->size<0) return -2;
    memcpy(p,r->data,n<size_t(r->size)?n:size_t(r->size));return r->size;
}
bool before(Fixture &f,uint16_t key)
{if(f.calls<16) f.write_keys[f.calls]=key;++f.calls;return f.cut==f.calls && f.cut_before;}
int after(Fixture &f) {return f.cut==f.calls && !f.cut_before?-1:0;}
int write(void *context,uint16_t key,const uint8_t *p,size_t n)
{
    auto &f=*static_cast<Fixture *>(context);if(before(f,key)) return -1;
    if(!f.write_lies) {auto &r=add(f,key,n);memcpy(r.data,p,n);}
    return after(f);
}
int erase(void *context,uint16_t key)
{
    auto &f=*static_cast<Fixture *>(context);if(before(f,key)) return -1;
    auto r=find(f,key);if(r && !f.erase_lies) r->size=-1;
    return after(f);
}
int identity(void *context,uint8_t p[8]) {memcpy(p,static_cast<Fixture *>(context)->eui,8);return 0;}
int backup(void *context,uint8_t p[32]) {memcpy(p,static_cast<Fixture *>(context)->backup,32);return 0;}
int health(void *context)
{auto &f=*static_cast<Fixture *>(context);return ++f.health_calls==f.health_fail_at?-1:0;}
bool confirmed(void *context) {return static_cast<Fixture *>(context)->confirmed;}
int confirm(void *context)
{
    auto &f=*static_cast<Fixture *>(context);if(before(f,0xffff) || f.confirm_fail) return -1;
    if(!f.confirm_lies) f.confirmed=true;
    return after(f);
}
OtaRecoveryIO io(Fixture &f) {return {&f,read,write,erase,identity,backup,health,confirmed,confirm};}
void init(Fixture &f,uint8_t role=1,bool clean=false)
{
    memset(&f,0,sizeof(f));f.config.role=role;
    for(unsigned i=0;i<8;++i) f.eui[i]=f.config.eui[i]=uint8_t(i+1);
    for(unsigned i=0;i<32;++i) {
        f.backup[i]=f.config.original_hash[i]=uint8_t(i+60);f.config.token[i]=uint8_t(i+100);
    }
    if(!clean && role==1) {
        auto p=f.config.local;f.config.local_length=168;
        put32(p,0x324c544f);put16(p+4,2);put16(p+6,168);put32(p+8,123);put32(p+16,123);
        put32(p+20,200000);p[24]=12;p[25]=2;p[26]=1;
        for(unsigned i=0;i<8;++i) p[28+i]=uint8_t(i+20);
        for(unsigned i=0;i<32;++i) p[36+i]=uint8_t(i+140);
        memcpy(p+68,f.config.original_hash,32);memcpy(p+100,"1.2.3",6);memcpy(p+132,"receiver-build",15);
        checksum(p,164);memcpy(add(f,LOCAL,168).data,p,168);
    }
    if(!clean && role==2) {
        auto p=f.config.fleet;f.config.fleet_length=308;
        put32(p,0x3341544f);put32(p+4,308);put32(p+8,123);
        put32(p+16,200000);put32(p+20,200000);put32(p+24,1);put32(p+28,1);put32(p+32,1);
        p[36]=2;p[165]=1;
        for(unsigned i=0;i<64;++i) p[37+i]=uint8_t(i+140);
        memcpy(p+101,"1.2.3",6);memcpy(p+133,"receiver-build",15);
        for(unsigned i=0;i<16;++i) p[166+i]=uint8_t(i+20);
        p[294]=2;p[296]=12;put32(p+300,123);checksum(p,304);
        memcpy(add(f,FLEET,308).data,p,308);
    }
    auto p=add(f,MAINTENANCE,12).data;put32(p,0x3141544f);checksum(p,8);
    memset(add(f,0x500,12).data,0xa5,12);memset(add(f,0x211,37).data,0x5a,37);
    memset(add(f,0x100,4).data,0x33,4);
}
bool absent(Fixture &f,uint16_t key) {auto r=find(f,key);return !r || r->size==-1;}
bool clean(Fixture &f)
{for(uint16_t key=MAINTENANCE;key<=MARKER;++key) if(!absent(f,key)) return false;return true;}
bool preserved(Fixture &f)
{
    const uint16_t keys[]={0x500,0x211,0x100};const unsigned sizes[]={12,37,4};const uint8_t values[]={0xa5,0x5a,0x33};
    for(unsigned i=0;i<3;++i) {
        auto r=find(f,keys[i]);if(!r || r->size!=int(sizes[i])) return false;
        for(unsigned j=0;j<sizes[i];++j) if(r->data[j]!=values[i]) return false;
    }
    return true;
}
int run(Fixture &f) {return ota_transition_run(f.config,io(f));}
void sync_expected(Fixture &f)
{
    if(f.config.local_length) {checksum(f.config.local,164);memcpy(find(f,LOCAL)->data,f.config.local,168);}
    if(f.config.fleet_length) {checksum(f.config.fleet,304);memcpy(find(f,FLEET)->data,f.config.fleet,308);}
}
}

extern "C" int ota_transition_test_run()
{
    Fixture f;
    for(uint8_t role=1;role<=2;++role) for(unsigned empty=0;empty<=1;++empty) {
        init(f,role,empty!=0);CHECK(run(f)==OTA_RECOVERED);CHECK(f.confirmed && clean(f) && preserved(f));
        const uint16_t order[]={MARKER,0xffff,FLEET,LOCAL,MAINTENANCE,MARKER};
        CHECK(f.calls==6 && !memcmp(f.write_keys,order,sizeof(order)));
        CHECK(run(f)==OTA_RECOVERY_ALREADY_DONE && f.calls==6);
        /* Every mutation is interrupted both before and after persistence.
         * No metadata disappears before confirmation; a reset resumes exactly
         * this policy, without touching calibration, role or NVS version. */
        for(unsigned cut=1;cut<=6;++cut) for(unsigned cut_before=0;cut_before<=1;++cut_before) {
            init(f,role,empty!=0);f.cut=cut;f.cut_before=cut_before!=0;
            CHECK(run(f)<0 && preserved(f));
            if(!f.confirmed) {
                CHECK(!absent(f,MAINTENANCE));
                if(!empty) CHECK(!absent(f,role==1?LOCAL:FLEET));
            }
            f.cut=0;const int rc=run(f);
            CHECK((rc==OTA_RECOVERED || rc==OTA_RECOVERY_ALREADY_DONE) && clean(f) && f.confirmed && preserved(f));
        }
        init(f,role,empty!=0);find(f,MAINTENANCE)->size=-1;CHECK(run(f)==OTA_RECOVERED && clean(f));
    }
    init(f);f.eui[0]^=1;CHECK(run(f)==OTA_RECOVERY_IDENTITY && !f.calls);
    init(f);f.backup[0]^=1;CHECK(run(f)==OTA_RECOVERY_BACKUP && !f.calls);
    init(f);memset(f.config.token,0,32);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);memset(f.config.eui,0,8);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);memset(f.config.original_hash,0,32);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);f.config.role=3;CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);f.config.local_length=167;CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f,2);f.config.fleet_length=307;CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);f.config.fleet_length=308;CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f,2);f.config.local_length=168;CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);auto bad_io=io(f);bad_io.confirm=nullptr;CHECK(ota_transition_run(f.config,bad_io)==OTA_RECOVERY_CONFIG && !f.calls);

    for(uint8_t state=0;state<=13;++state) if(state!=12) for(uint8_t role=1;role<=2;++role) {
        init(f,role);if(role==1) f.config.local[24]=state;else f.config.fleet[296]=state;
        sync_expected(f);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    }
    const unsigned bad_local_fields[]={0,4,6,25,26,27,68,164};
    for(unsigned offset:bad_local_fields) {
        init(f);f.config.local[offset]^=1;if(offset!=164) sync_expected(f);
        CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    }
    const unsigned local_zero_offsets[]={8,16,20,28,36};
    const unsigned local_zero_lengths[]={8,4,4,8,32};
    for(unsigned i=0;i<5;++i) {
        init(f);memset(f.config.local+local_zero_offsets[i],0,local_zero_lengths[i]);sync_expected(f);
        CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    }
    init(f);put32(f.config.local+20,221185);sync_expected(f);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f);memcpy(f.config.local+28,f.eui,8);sync_expected(f);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);

    const unsigned bad_fleet_fields[]={0,4,20,36,165,295,304};
    for(unsigned offset:bad_fleet_fields) {
        init(f,2);f.config.fleet[offset]^=1;if(offset!=304) sync_expected(f);
        CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    }
    const unsigned fleet_zero_offsets[]={8,16,37,69,166,294,300};
    const unsigned fleet_zero_lengths[]={8,4,32,32,8,1,4};
    for(unsigned i=0;i<7;++i) {
        init(f,2);memset(f.config.fleet+fleet_zero_offsets[i],0,fleet_zero_lengths[i]);sync_expected(f);
        CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    }
    init(f,2);f.config.fleet[294]=17;sync_expected(f);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f,2);memcpy(f.config.fleet+174,f.config.fleet+166,8);sync_expected(f);
    CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f,2);memcpy(f.config.fleet+166,f.eui,8);sync_expected(f);CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);
    init(f,2);put32(f.config.fleet+16,221185);put32(f.config.fleet+20,221185);sync_expected(f);
    CHECK(run(f)==OTA_RECOVERY_CONFIG && !f.calls);

    for(uint8_t role=1;role<=2;++role) {
        const uint16_t key=role==1?LOCAL:FLEET;const int error=role==1?OTA_RECOVERY_JOURNAL:OTA_RECOVERY_FLEET;
        init(f,role);find(f,key)->data[100]^=1;CHECK(run(f)==error && !f.calls);
        init(f,role);find(f,key)->size-=1;CHECK(run(f)==error && !f.calls);
        init(f,role);find(f,key)->size=-1;CHECK(run(f)==error && !f.calls);
        init(f,role);f.read_error=key;CHECK(run(f)==error && !f.calls);
        init(f,role,true);add(f,key,1);CHECK(run(f)==error && !f.calls);
        init(f,role);add(f,role==1?FLEET:LOCAL,1);
        CHECK(run(f)==(role==1?OTA_RECOVERY_FLEET:OTA_RECOVERY_JOURNAL) && !f.calls);
        /* A matching intent alone cannot explain deleted records before the
         * helper has been confirmed. Surviving records never get a waiver. */
        init(f,role);f.cut=1;CHECK(run(f)==OTA_RECOVERY_STORAGE);f.cut=0;
        find(f,key)->size=-1;unsigned calls=f.calls;CHECK(run(f)==error && f.calls==calls);
        init(f,role);f.cut=2;CHECK(run(f)==OTA_RECOVERY_CONFIRM && f.confirmed);f.cut=0;
        find(f,key)->data[100]^=1;calls=f.calls;CHECK(run(f)==error && f.calls==calls);
    }
    const unsigned bad_maintenance_fields[]={0,4,8};
    for(unsigned offset:bad_maintenance_fields) {
        init(f);find(f,MAINTENANCE)->data[offset]^=1;
        if(offset==4) checksum(find(f,MAINTENANCE)->data,8);
        CHECK(run(f)==OTA_RECOVERY_MAINTENANCE && !f.calls);
    }
    init(f);find(f,MAINTENANCE)->size=11;CHECK(run(f)==OTA_RECOVERY_MAINTENANCE && !f.calls);
    init(f);f.read_error=MAINTENANCE;CHECK(run(f)==OTA_RECOVERY_MAINTENANCE && !f.calls);
    init(f);add(f,MARKER,64);CHECK(run(f)==OTA_RECOVERY_MARKER && !f.calls);
    init(f);f.read_error=MARKER;CHECK(run(f)==OTA_RECOVERY_MARKER && !f.calls);
    init(f);f.cut=1;CHECK(run(f)==OTA_RECOVERY_STORAGE);f.cut=0;f.config.token[0]^=1;
    CHECK(run(f)==OTA_RECOVERY_MARKER && f.calls==1);
    init(f);f.cut=1;CHECK(run(f)==OTA_RECOVERY_STORAGE);f.cut=0;find(f,MARKER)->data[99]^=1;
    CHECK(run(f)==OTA_RECOVERY_MARKER && f.calls==1);
    init(f);f.cut=1;CHECK(run(f)==OTA_RECOVERY_STORAGE);f.cut=0;f.config.local[100]^=1;sync_expected(f);
    CHECK(run(f)==OTA_RECOVERY_MARKER && f.calls==1);
    init(f);f.write_lies=true;CHECK(run(f)==OTA_RECOVERY_STORAGE && f.calls==1 && !f.confirmed);
    init(f);f.erase_lies=true;CHECK(run(f)==OTA_RECOVERY_STORAGE && !absent(f,LOCAL) && !absent(f,MARKER));
    init(f);f.confirm_fail=true;CHECK(run(f)==OTA_RECOVERY_CONFIRM && !absent(f,LOCAL) && !absent(f,MARKER));
    init(f);f.confirm_lies=true;CHECK(run(f)==OTA_RECOVERY_CONFIRM && !absent(f,LOCAL) && !absent(f,MARKER));
    for(unsigned fail=1;fail<=2;++fail) {
        init(f);f.health_fail_at=fail;CHECK(run(f)==OTA_RECOVERY_HEALTH && !f.confirmed && !absent(f,LOCAL));
        CHECK(f.calls==fail-1);
    }
    init(f);CHECK(run(f)==OTA_RECOVERED);f.eui[0]^=1;CHECK(run(f)==OTA_RECOVERY_IDENTITY);
    init(f);CHECK(run(f)==OTA_RECOVERED);f.backup[0]^=1;CHECK(run(f)==OTA_RECOVERY_BACKUP);
    init(f);CHECK(run(f)==OTA_RECOVERED);f.health_fail_at=f.health_calls+1;CHECK(run(f)==OTA_RECOVERY_HEALTH);
    return 0;
}
#ifndef OWNTECH_FREESTANDING_TEST
int main() {return ota_transition_test_run();}
#endif
