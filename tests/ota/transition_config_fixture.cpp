/* Generated host policy is consumed by the actual freestanding helper core. */
#include "owntech_ota_recovery_config.h"
#include "transition_core.h"
#include <string.h>
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)

namespace {
constexpr uint16_t MAINTENANCE=0x501, LOCAL=0x502, FLEET=0x503, MARKER=0x504;
const OtaTransitionConfig config = {
    OWNTECH_OTA_TRANSITION_EUI_BYTES, OWNTECH_OTA_TRANSITION_ORIGINAL_HASH_BYTES,
    OWNTECH_OTA_TRANSITION_TOKEN_BYTES, OWNTECH_OTA_TRANSITION_ROLE,
    OWNTECH_OTA_TRANSITION_LOCAL_LENGTH, OWNTECH_OTA_TRANSITION_FLEET_LENGTH,
    OWNTECH_OTA_TRANSITION_LOCAL_BYTES, OWNTECH_OTA_TRANSITION_FLEET_BYTES
};
struct Record { uint16_t key; int length; uint8_t data[320]; };
struct Fixture {
    Record records[8]; unsigned count, writes, erases;
    bool confirmed, changed_identity, changed_backup;
};
Record *find(Fixture &f,uint16_t key)
{ for(unsigned i=0;i<f.count;++i) if(f.records[i].key==key) return f.records+i; return nullptr; }
Record &add(Fixture &f,uint16_t key,const uint8_t *data,size_t length)
{
    auto r=find(f,key); if(!r) { r=f.records+f.count++; r->key=key; }
    r->length=int(length); memcpy(r->data,data,length); return *r;
}
int read(void *context,uint16_t key,uint8_t *data,size_t length)
{
    auto r=find(*static_cast<Fixture *>(context),key);
    if(!r || r->length<0) return -2;
    memcpy(data,r->data,length<size_t(r->length)?length:size_t(r->length)); return r->length;
}
int write(void *context,uint16_t key,const uint8_t *data,size_t length)
{
    auto &f=*static_cast<Fixture *>(context); ++f.writes; add(f,key,data,length); return int(length);
}
int erase(void *context,uint16_t key)
{
    auto &f=*static_cast<Fixture *>(context); ++f.erases;
    auto r=find(f,key); if(r) r->length=-1; return 0;
}
int identity(void *context,uint8_t eui[8])
{ memcpy(eui,config.eui,8); if(static_cast<Fixture *>(context)->changed_identity) eui[0]^=1; return 0; }
int backup(void *context,uint8_t hash[32])
{ memcpy(hash,config.original_hash,32); if(static_cast<Fixture *>(context)->changed_backup) hash[0]^=1; return 0; }
int health(void *) { return 0; }
bool confirmed(void *context) { return static_cast<Fixture *>(context)->confirmed; }
int confirm(void *context) { static_cast<Fixture *>(context)->confirmed=true; return 0; }
int run(Fixture &f)
{
    const OtaRecoveryIO io={&f,read,write,erase,identity,backup,health,confirmed,confirm};
    return ota_transition_run(config,io);
}
void init(Fixture &f)
{
    memset(&f,0,sizeof(f));
    if(config.local_length) add(f,LOCAL,config.local,config.local_length);
    if(config.fleet_length) add(f,FLEET,config.fleet,config.fleet_length);
    uint8_t maintenance[12]={0x4f,0x54,0x41,0x31};
    const uint32_t crc=ota_recovery_crc32(maintenance,8);
    for(unsigned i=0;i<4;++i) maintenance[8+i]=uint8_t(crc>>(i*8));
    add(f,MAINTENANCE,maintenance,sizeof(maintenance));
    const uint8_t calibration[]={1,3,5,7}; add(f,0x201,calibration,sizeof(calibration));
    const uint8_t role[]={OWNTECH_OTA_TRANSITION_ROLE}; add(f,0x500,role,sizeof(role));
}
}

extern "C" int ota_transition_config_fixture_run()
{
    Fixture f; init(f);
    CHECK(run(f)==OTA_RECOVERED && f.confirmed && f.writes==1 && f.erases==4);
    uint8_t bytes[4];
    for(uint16_t key=MAINTENANCE;key<=MARKER;++key) CHECK(read(&f,key,bytes,sizeof(bytes))==-2);
    CHECK(read(&f,0x201,bytes,sizeof(bytes))==4 && bytes[0]==1 && bytes[3]==7);
    CHECK(read(&f,0x500,bytes,sizeof(bytes))==1 && bytes[0]==config.role);
    CHECK(run(f)==OTA_RECOVERY_ALREADY_DONE && f.writes==1 && f.erases==4);
    init(f); f.changed_identity=true;
    CHECK(run(f)==OTA_RECOVERY_IDENTITY && !f.writes && !f.erases);
    init(f); f.changed_backup=true;
    CHECK(run(f)==OTA_RECOVERY_BACKUP && !f.writes && !f.erases);
    /* A locally valid but different durable record must not be erased. */
    init(f); auto record=find(f,config.role==1?LOCAL:FLEET);
    if(record) {
        record->data[8]^=1;
        const unsigned payload=unsigned(record->length)-4;
        const uint32_t crc=ota_recovery_crc32(record->data,payload);
        for(unsigned i=0;i<4;++i) record->data[payload+i]=uint8_t(crc>>(i*8));
    } else {
        const uint8_t unexpected[]={1}; add(f,config.role==1?LOCAL:FLEET,unexpected,sizeof(unexpected));
    }
    CHECK(run(f)==(config.role==1?OTA_RECOVERY_JOURNAL:OTA_RECOVERY_FLEET) && !f.writes && !f.erases);
    return 0;
}
#ifndef OWNTECH_FREESTANDING_TEST
int main() { return ota_transition_config_fixture_run(); }
#endif
