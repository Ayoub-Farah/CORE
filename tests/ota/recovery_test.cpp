/* SPDX-License-Identifier: Apache-2.0 */
#include "recovery_core.h"
#include <string.h>
#ifdef OWNTECH_ARM_FIXTURE
#include "recovery_arm_fixture.h"
#endif
#define CHECK(x) do {if(!(x)) return __LINE__;} while(0)

namespace {
struct Record {uint16_t key;int size;uint8_t bytes[1024];};
struct Fixture {
    Record records[8];size_t count;
    OtaRecoveryConfig config;
    uint8_t eui[8],backup[32];
    bool confirmed,health_fail,confirm_fail;
    unsigned mutations,cut,confirm_calls;
};
void put32(uint8_t *p,uint32_t x) {for(unsigned i=0;i<4;i++) p[i]=uint8_t(x>>(8*i));}
void put64(uint8_t *p,uint64_t x) {put32(p,uint32_t(x));put32(p+4,uint32_t(x>>32));}
void checksum(uint8_t *p,size_t n) {put32(p+n,ota_recovery_crc32(p,n));}
Record *find(Fixture &f,uint16_t key)
{for(size_t i=0;i<f.count;i++) if(f.records[i].key==key) return &f.records[i];return nullptr;}
Record &add(Fixture &f,uint16_t key,int size)
{Record *r=find(f,key);if(!r) {r=&f.records[f.count++];r->key=key;}r->size=size;memset(r->bytes,0,sizeof(r->bytes));return *r;}
int read(void *ctx,uint16_t key,uint8_t *p,size_t n)
{auto &f=*static_cast<Fixture *>(ctx);Record *r=find(f,key);if(!r || r->size<0) return -2;
 memcpy(p,r->bytes,n<size_t(r->size)?n:size_t(r->size));return r->size;}
int mutation(Fixture &f) {return ++f.mutations==f.cut?-1:0;}
int write(void *ctx,uint16_t key,const uint8_t *p,size_t n)
{auto &f=*static_cast<Fixture *>(ctx);auto &r=add(f,key,int(n));memcpy(r.bytes,p,n);return mutation(f)?-1:int(n);}
int erase(void *ctx,uint16_t key)
{auto &f=*static_cast<Fixture *>(ctx);Record *r=find(f,key);if(r) r->size=-1;return mutation(f);}
int identity(void *ctx,uint8_t p[8]) {memcpy(p,static_cast<Fixture *>(ctx)->eui,8);return 0;}
int backup(void *ctx,uint8_t p[32]) {memcpy(p,static_cast<Fixture *>(ctx)->backup,32);return 0;}
int health(void *ctx) {return static_cast<Fixture *>(ctx)->health_fail?-1:0;}
bool confirmed(void *ctx) {return static_cast<Fixture *>(ctx)->confirmed;}
int confirm(void *ctx)
{auto &f=*static_cast<Fixture *>(ctx);++f.confirm_calls;if(f.confirm_fail) return -1;f.confirmed=true;return mutation(f);}
OtaRecoveryIO io(Fixture &f) {return {&f,read,write,erase,identity,backup,health,confirmed,confirm};}
void init(Fixture &f,bool lead=true)
{
    memset(&f,0,sizeof(f));f.config.campaign=0x123456789abcdefULL;f.config.image_size=227328;f.config.board_count=2;
    for(unsigned i=0;i<8;i++) {f.config.lead_eui[i]=uint8_t(i+1);f.config.boards[0].eui[i]=uint8_t(i+1);f.config.boards[1].eui[i]=uint8_t(i+20);}
    for(unsigned i=0;i<32;i++) {f.config.image_hash[i]=uint8_t(i+70);f.config.boards[0].original_hash[i]=uint8_t(i+100);f.config.boards[1].original_hash[i]=uint8_t(i+130);}
    memcpy(f.eui,f.config.boards[lead?0:1].eui,8);memcpy(f.backup,f.config.boards[lead?0:1].original_hash,32);
    auto &role=add(f,0x500,12);put32(role.bytes,0x3141544f);put32(role.bytes+4,lead?1:0);checksum(role.bytes,8);
    auto &maint=add(f,0x501,12);put32(maint.bytes,0x3141544f);put32(maint.bytes+4,1);checksum(maint.bytes,8);
    auto &journal=add(f,0x502,240);put32(journal.bytes,0x3141544f);put64(journal.bytes+8,f.config.campaign);
    put32(journal.bytes+20,f.config.image_size);journal.bytes[24]=6;memcpy(journal.bytes+25,f.config.lead_eui,8);
    memcpy(journal.bytes+65,f.config.image_hash,32);checksum(journal.bytes,232);
#ifdef OWNTECH_ARM_FIXTURE
    memcpy(journal.bytes,ARM_JOURNAL,sizeof(ARM_JOURNAL));
#endif
    if(lead) {
        auto &fleet=add(f,0x503,824);put32(fleet.bytes,0x3141544f);put32(fleet.bytes+4,824);put64(fleet.bytes+8,f.config.campaign);
        put32(fleet.bytes+16,f.config.image_size);memcpy(fleet.bytes+69,f.config.image_hash,32);
        for(size_t i=0;i<2;i++) {memcpy(fleet.bytes+168+i*40,f.config.boards[i].eui,8);fleet.bytes[168+i*40+36]=i==0;}
        put32(fleet.bytes+808,42);put32(fleet.bytes+812,2);fleet.bytes[816]=6;checksum(fleet.bytes,820);
#ifdef OWNTECH_ARM_FIXTURE
        memcpy(fleet.bytes,ARM_FLEET,sizeof(ARM_FLEET));
#endif
    }
    auto &cal=add(f,0x201,37);memset(cal.bytes,0x5a,37);
    auto &version=add(f,0x100,2);version.bytes[0]=1;
}
bool clean(Fixture &f)
{for(uint16_t key=0x501;key<=0x504;key++) {auto r=find(f,key);if(r && r->size!=-1) return false;}return true;}
bool preserved(Fixture &f,const uint8_t role[12])
{auto a=find(f,0x500),b=find(f,0x201),c=find(f,0x100);if(!a||!b||!c||a->size!=12||b->size!=37||c->size!=2||memcmp(a->bytes,role,12)||c->bytes[0]!=1||c->bytes[1]) return false;
 for(unsigned i=0;i<37;i++) if(b->bytes[i]!=0x5a) return false;return true;}
}

extern "C" int ota_recovery_test_run()
{
    Fixture f;uint8_t original_role[12];
    init(f);memcpy(original_role,find(f,0x500)->bytes,12);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERED);CHECK(f.confirmed && clean(f));CHECK(preserved(f,original_role));
    unsigned mutations=f.mutations;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_ALREADY_DONE);CHECK(f.mutations==mutations);
    init(f,false);memcpy(original_role,find(f,0x500)->bytes,12);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERED);CHECK(clean(f) && preserved(f,original_role));

    /* Both mutation-before-failure and confirmation persist across simulated
     * power cuts. No step can discard intent before all cleanup is durable. */
    for(unsigned cut=1;cut<=6;cut++) {
        init(f);memcpy(original_role,find(f,0x500)->bytes,12);f.cut=cut;
        CHECK(ota_recovery_run(f.config,io(f))<0);CHECK(preserved(f,original_role));
        f.cut=0;int rc=ota_recovery_run(f.config,io(f));CHECK(rc==OTA_RECOVERED || rc==OTA_RECOVERY_ALREADY_DONE);
        CHECK(f.confirmed && clean(f) && preserved(f,original_role));
    }
    init(f);f.eui[0]^=0x80;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_IDENTITY);CHECK(!f.mutations);
    init(f);f.backup[0]^=1;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_BACKUP);CHECK(!f.mutations);
    init(f);++f.config.campaign;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    init(f);find(f,0x502)->bytes[65]^=1;checksum(find(f,0x502)->bytes,232);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    init(f);find(f,0x502)->bytes[25]^=1;checksum(find(f,0x502)->bytes,232);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    for(unsigned state=7;state<=12;state++) {init(f);find(f,0x502)->bytes[24]=uint8_t(state);checksum(find(f,0x502)->bytes,232);
        CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);}
    init(f);put32(find(f,0x502)->bytes+16,42);checksum(find(f,0x502)->bytes,232);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    init(f);find(f,0x502)->bytes[100]^=1;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    init(f);find(f,0x502)->size=236;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    init(f);find(f,0x503)->bytes[816]=7;checksum(find(f,0x503)->bytes,820);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_FLEET);CHECK(!f.mutations);
    init(f);find(f,0x501)->bytes[4]=0;checksum(find(f,0x501)->bytes,8);
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_MAINTENANCE);CHECK(!f.mutations);
    init(f);f.health_fail=true;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_HEALTH);CHECK(!f.mutations);
    init(f);f.confirm_fail=true;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_CONFIRM);
    CHECK(find(f,0x504)->size==64 && find(f,0x502)->size==240 && find(f,0x501)->size==12);
    f.confirm_fail=false;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERED);
    init(f);f.cut=1;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_STORAGE);f.cut=0;
    put32(find(f,0x502)->bytes+16,42);checksum(find(f,0x502)->bytes,232);mutations=f.mutations;
    CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(f.mutations==mutations);
    init(f);add(f,0x504,64);CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_MARKER);CHECK(!f.mutations);
    init(f);find(f,0x502)->size=-1;CHECK(ota_recovery_run(f.config,io(f))==OTA_RECOVERY_JOURNAL);CHECK(!f.mutations);
    return 0;
}
#ifndef OWNTECH_FREESTANDING_TEST
int main() {return ota_recovery_test_run();}
#endif
