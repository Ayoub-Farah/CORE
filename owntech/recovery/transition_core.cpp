/* SPDX-License-Identifier: Apache-2.0 */
#include "transition_core.h"
#include <string.h>

namespace {
constexpr uint16_t MAINTENANCE=0x0501, LOCAL=0x0502, FLEET=0x0503, MARKER=0x0504;
constexpr uint32_t OTA1=0x3141544f, OTL2=0x324c544f, OTA3=0x3341544f, OTX1=0x3158544f;
constexpr unsigned CAPACITY=221184, SUCCESS=12, RECEIVER=1, LEAD=2;
constexpr int MISSING=-2;
uint16_t le16(const uint8_t *p) {return uint16_t(p[0])|uint16_t(p[1])<<8;}
uint32_t le32(const uint8_t *p) {return uint32_t(le16(p))|uint32_t(le16(p+2))<<16;}
void put16(uint8_t *p,uint16_t x) {p[0]=uint8_t(x);p[1]=uint8_t(x>>8);}
void put32(uint8_t *p,uint32_t x) {put16(p,uint16_t(x));put16(p+2,uint16_t(x>>16));}
bool nonzero(const uint8_t *p,size_t n) {uint8_t x=0;while(n--) x|=*p++;return x!=0;}
bool crc(const uint8_t *p,size_t n) {return le32(p+n)==ota_recovery_crc32(p,n);}

bool local_valid(const OtaTransitionConfig &c)
{
    const auto p=c.local;
    return c.local_length==168 && le32(p)==OTL2 && le16(p+4)==2 && le16(p+6)==168 && crc(p,164) &&
        nonzero(p+8,8) && le32(p+16) && le32(p+20) && le32(p+20)<=CAPACITY &&
        p[24]==SUCCESS && p[25]==2 && p[26]==RECEIVER && !p[27] &&
        nonzero(p+28,8) && memcmp(p+28,c.eui,8) && nonzero(p+36,32) && nonzero(p+68,32) &&
        !memcmp(p+68,c.original_hash,32);
}
bool fleet_valid(const OtaTransitionConfig &c)
{
    const auto p=c.fleet;
    if(c.fleet_length!=308 || le32(p)!=OTA3 || le32(p+4)!=308 || !crc(p,304) ||
       !nonzero(p+8,8) || !le32(p+16) || le32(p+16)>CAPACITY || le32(p+20)!=le32(p+16) ||
       p[36]!=2 || p[165]!=RECEIVER || !nonzero(p+37,32) || !nonzero(p+69,32) ||
       !p[294] || p[294]>16 || p[295] || p[296]!=SUCCESS || !le32(p+300)) return false;
    for(unsigned i=0;i<p[294];++i) {
        const auto eui=p+166+i*8;
        if(!nonzero(eui,8) || !memcmp(eui,c.eui,8)) return false;
        for(unsigned j=0;j<i;++j) if(!memcmp(eui,p+166+j*8,8)) return false;
    }
    return true;
}
bool config_valid(const OtaTransitionConfig &c)
{
    if(!nonzero(c.eui,8) || !nonzero(c.original_hash,32) || !nonzero(c.token,32)) return false;
    if(c.role==RECEIVER) return !c.fleet_length && (!c.local_length || local_valid(c));
    if(c.role==LEAD) return !c.local_length && (!c.fleet_length || fleet_valid(c));
    return false;
}
bool maintenance_valid(const uint8_t *p,int n)
{return n==MISSING || (n==12 && le32(p)==OTA1 && !le32(p+4) && crc(p,8));}
void make_marker(uint8_t p[100],const OtaTransitionConfig &c)
{
    memset(p,0,100);put32(p,OTX1);put32(p+4,1);memcpy(p+8,c.eui,8);
    memcpy(p+16,c.original_hash,32);memcpy(p+48,c.token,32);p[80]=c.role;
    put16(p+82,c.local_length);put16(p+84,c.fleet_length);
    /* Do not CRC an already CRC-suffixed record: that yields a fixed residue. */
    put32(p+88,c.local_length?le32(c.local+164):0);
    put32(p+92,c.fleet_length?le32(c.fleet+304):0);
    put32(p+96,ota_recovery_crc32(p,96));
}
bool record_matches(const uint8_t *actual,int n,const uint8_t *expected,size_t length,bool resume)
{
    if(!length) return n==MISSING;
    return (resume && n==MISSING) || (n==int(length) && !memcmp(actual,expected,length));
}
bool erase_verified(const OtaRecoveryIO &io,uint16_t key)
{
    if(io.erase(io.context,key)<0) return false;
    uint8_t byte;
    return io.read(io.context,key,&byte,1)==MISSING;
}
}

int ota_transition_run(const OtaTransitionConfig &c,const OtaRecoveryIO &io)
{
    if(!config_valid(c) || !io.read || !io.write || !io.erase || !io.identity || !io.backup_hash ||
       !io.health || !io.confirmed || !io.confirm) return OTA_RECOVERY_CONFIG;
    uint8_t eui[8],backup[32];
    if(io.identity(io.context,eui) || memcmp(eui,c.eui,8)) return OTA_RECOVERY_IDENTITY;
    if(io.backup_hash(io.context,backup) || memcmp(backup,c.original_hash,32)) return OTA_RECOVERY_BACKUP;

    uint8_t expected_marker[100],marker[100],local[168],fleet[308],maintenance[12];
    make_marker(expected_marker,c);
    const int mr=io.read(io.context,MARKER,marker,sizeof(marker));
    const int lr=io.read(io.context,LOCAL,local,sizeof(local));
    const int fr=io.read(io.context,FLEET,fleet,sizeof(fleet));
    const int ar=io.read(io.context,MAINTENANCE,maintenance,sizeof(maintenance));
    const bool resume=mr==int(sizeof(marker)) && !memcmp(marker,expected_marker,sizeof(marker));
    if(mr!=MISSING && !resume) return OTA_RECOVERY_MARKER;
    if(mr==MISSING && lr==MISSING && fr==MISSING && ar==MISSING && io.confirmed(io.context))
        return io.health(io.context)?OTA_RECOVERY_HEALTH:OTA_RECOVERY_ALREADY_DONE;
    /* A durable matching intent permits absence only for keys deleted by this
     * exact signed policy. Every surviving record must still match byte-for-byte. */
    const bool may_be_deleted=resume && io.confirmed(io.context);
    if(!record_matches(local,lr,c.local,c.local_length,may_be_deleted)) return OTA_RECOVERY_JOURNAL;
    if(!record_matches(fleet,fr,c.fleet,c.fleet_length,may_be_deleted)) return OTA_RECOVERY_FLEET;
    if(!maintenance_valid(maintenance,ar)) return OTA_RECOVERY_MAINTENANCE;
    if(io.health(io.context)) return OTA_RECOVERY_HEALTH;
    if(!resume && (io.write(io.context,MARKER,expected_marker,sizeof(expected_marker))<0 ||
       io.read(io.context,MARKER,marker,sizeof(marker))!=int(sizeof(marker)) ||
       memcmp(marker,expected_marker,sizeof(marker)))) return OTA_RECOVERY_STORAGE;
    /* Once a key is removed, a reset must return to this utility, never to the
     * previous application with only part of its metadata still present. */
    if(io.health(io.context)) return OTA_RECOVERY_HEALTH;
    if(!io.confirmed(io.context) && io.confirm(io.context)) return OTA_RECOVERY_CONFIRM;
    if(!io.confirmed(io.context)) return OTA_RECOVERY_CONFIRM;
    if(!erase_verified(io,FLEET) || !erase_verified(io,LOCAL) || !erase_verified(io,MAINTENANCE) ||
       !erase_verified(io,MARKER)) return OTA_RECOVERY_STORAGE;
    return OTA_RECOVERED;
}
