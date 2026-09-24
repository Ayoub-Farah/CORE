/* SPDX-License-Identifier: Apache-2.0 */
#include "recovery_core.h"
#include <string.h>

namespace {
constexpr uint16_t MAINTENANCE = 0x0501, JOURNAL = 0x0502, FLEET = 0x0503, MARKER = 0x0504;
constexpr uint32_t OTA1 = 0x3141544f, REC1 = 0x3152434f;
constexpr int MISSING = -2;
uint32_t le32(const uint8_t *p)
{ return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint64_t le64(const uint8_t *p) { return le32(p) | uint64_t(le32(p + 4)) << 32; }
void put32(uint8_t *p, uint32_t x) { for (unsigned i=0;i<4;i++) p[i]=uint8_t(x>>(8*i)); }
void put64(uint8_t *p, uint64_t x) { put32(p,uint32_t(x));put32(p+4,uint32_t(x>>32)); }
bool nonzero(const uint8_t *p, size_t n) { uint8_t v=0;while(n--) v|=*p++;return v!=0; }
bool crc(const uint8_t *p, size_t offset) { return le32(p+offset)==ota_recovery_crc32(p,offset); }

bool journal_ok(const uint8_t *p, int n, const OtaRecoveryConfig &c)
{
    /* Legacy OTA1 Cortex-M ABI uses -fshort-enums: state is one byte.
     * local_record=240, journal at8, CRC at232; padding is CRC-covered. */
    return n==240 && le32(p)==OTA1 && crc(p,232) && le64(p+8)==c.campaign &&
        le32(p+16)==0 && le32(p+20)==c.image_size && p[24]==6 &&
        !memcmp(p+25,c.lead_eui,8) && !memcmp(p+65,c.image_hash,32);
}
bool maintenance_ok(const uint8_t *p, int n)
{ return n==12 && le32(p)==OTA1 && le32(p+4)==1 && crc(p,8); }
bool fleet_ok(const uint8_t *p, int n, const OtaRecoveryConfig &c)
{
    /* Legacy fixed16-target fleet_record, ARM ABI: 824 bytes. */
    if(n!=824 || le32(p)!=OTA1 || le32(p+4)!=824 || !crc(p,820) ||
       le64(p+8)!=c.campaign || le32(p+16)!=c.image_size ||
       memcmp(p+69,c.image_hash,32) || le32(p+812)!=c.board_count || !le32(p+808) ||
       (p[816]!=6 && p[816]!=9)) return false;
    uint32_t seen=0;unsigned leads=0;
    for(size_t i=0;i<c.board_count;i++) {
        const uint8_t *target=p+168+i*40;
        bool found=false;
        for(size_t j=0;j<c.board_count;j++) if(!memcmp(target,c.boards[j].eui,8)) {
            if(seen&(1U<<j)) return false;
            seen|=1U<<j;found=true;break;
        }
        if(!found || target[36]>1) return false;
        if(target[36]) {if(memcmp(target,c.lead_eui,8)) return false;++leads;}
    }
    return leads==1;
}
void make_marker(uint8_t p[64],const OtaRecoveryConfig &c,const uint8_t eui[8])
{
    memset(p,0,64);put32(p,REC1);put32(p+4,1);put64(p+8,c.campaign);
    memcpy(p+16,eui,8);memcpy(p+24,c.image_hash,32);put32(p+56,c.image_size);
    put32(p+60,ota_recovery_crc32(p,60));
}
bool erase_verified(const OtaRecoveryIO &io,uint16_t key)
{
    if(io.erase(io.context,key)<0) return false;
    uint8_t byte;
    return io.read(io.context,key,&byte,1)==MISSING;
}
}

uint32_t ota_recovery_crc32(const uint8_t *p,size_t n)
{
    uint32_t v=~0U;
    while(n--) {v^=*p++;for(unsigned i=0;i<8;i++) v=(v>>1)^(0xedb88320U&(0U-(v&1)));}
    return ~v;
}
int ota_recovery_run(const OtaRecoveryConfig &c,const OtaRecoveryIO &io)
{
    if(!c.campaign || !c.image_size || !c.board_count || c.board_count>16 ||
       !nonzero(c.lead_eui,8) || !nonzero(c.image_hash,32) || !io.read || !io.write ||
       !io.erase || !io.identity || !io.backup_hash || !io.health || !io.confirmed || !io.confirm)
        return OTA_RECOVERY_CONFIG;
    unsigned lead_count=0;
    for(size_t i=0;i<c.board_count;i++) {
        if(!nonzero(c.boards[i].eui,8) || !nonzero(c.boards[i].original_hash,32)) return OTA_RECOVERY_CONFIG;
        for(size_t j=0;j<i;j++) if(!memcmp(c.boards[i].eui,c.boards[j].eui,8)) return OTA_RECOVERY_CONFIG;
        if(!memcmp(c.boards[i].eui,c.lead_eui,8)) ++lead_count;
    }
    if(lead_count!=1) return OTA_RECOVERY_CONFIG;
    uint8_t eui[8],backup[32];
    if(io.identity(io.context,eui)) return OTA_RECOVERY_IDENTITY;
    const OtaRecoveryBoard *board=nullptr;
    for(size_t i=0;i<c.board_count;i++) if(!memcmp(c.boards[i].eui,eui,8)) board=&c.boards[i];
    if(!board) return OTA_RECOVERY_IDENTITY;
    if(io.backup_hash(io.context,backup) || memcmp(backup,board->original_hash,32)) return OTA_RECOVERY_BACKUP;

    uint8_t expected_marker[64],marker[64],journal[240],maintenance[12],fleet[824];
    make_marker(expected_marker,c,eui);
    int mr=io.read(io.context,MARKER,marker,sizeof(marker));
    int jr=io.read(io.context,JOURNAL,journal,sizeof(journal));
    int ar=io.read(io.context,MAINTENANCE,maintenance,sizeof(maintenance));
    int fr=io.read(io.context,FLEET,fleet,sizeof(fleet));
    const bool resume=mr==64 && !memcmp(marker,expected_marker,64);
    if(mr!=MISSING && !resume) return OTA_RECOVERY_MARKER;
    if(mr==MISSING && jr==MISSING && ar==MISSING && fr==MISSING && io.confirmed(io.context))
        return io.health(io.context)?OTA_RECOVERY_HEALTH:OTA_RECOVERY_ALREADY_DONE;
    /* An existing valid marker permits only keys this routine already deleted
     * to be absent. Any remaining record must still pass every original guard. */
    if(!(resume && jr==MISSING) && !journal_ok(journal,jr,c)) return OTA_RECOVERY_JOURNAL;
    if(!(resume && ar==MISSING) && !maintenance_ok(maintenance,ar)) return OTA_RECOVERY_MAINTENANCE;
    if(fr!=MISSING && !fleet_ok(fleet,fr,c)) return OTA_RECOVERY_FLEET;
    if(io.health(io.context)) return OTA_RECOVERY_HEALTH;

    if(!resume) {
        if(io.write(io.context,MARKER,expected_marker,sizeof(expected_marker))<0 ||
           io.read(io.context,MARKER,marker,sizeof(marker))!=64 || memcmp(marker,expected_marker,64))
            return OTA_RECOVERY_STORAGE;
    }
    /* Confirm only this locally healthy recovery image, AFTER all guards and
     * durable intent. Power loss after destructive steps then returns here,
     * rather than rolling back into the old application with partial cleanup. */
    if(io.health(io.context)) return OTA_RECOVERY_HEALTH;
    if(!io.confirmed(io.context) && io.confirm(io.context)) return OTA_RECOVERY_CONFIRM;
    if(!io.confirmed(io.context)) return OTA_RECOVERY_CONFIRM;
    if(!erase_verified(io,FLEET) || !erase_verified(io,JOURNAL) || !erase_verified(io,MAINTENANCE))
        return OTA_RECOVERY_STORAGE;
    if(!erase_verified(io,MARKER)) return OTA_RECOVERY_STORAGE;
    return OTA_RECOVERED;
}
const char *ota_recovery_result_name(int result)
{
    switch(result) {
    case OTA_RECOVERED:return "RECOVERED";case OTA_RECOVERY_ALREADY_DONE:return "ALREADY_RECOVERED";
    case OTA_RECOVERY_CONFIG:return "CONFIG_REFUSED";case OTA_RECOVERY_IDENTITY:return "IDENTITY_REFUSED";
    case OTA_RECOVERY_BACKUP:return "BACKUP_HASH_REFUSED";case OTA_RECOVERY_JOURNAL:return "JOURNAL_REFUSED";
    case OTA_RECOVERY_MAINTENANCE:return "MAINTENANCE_REFUSED";case OTA_RECOVERY_FLEET:return "FLEET_REFUSED";
    case OTA_RECOVERY_MARKER:return "RECOVERY_MARKER_REFUSED";case OTA_RECOVERY_STORAGE:return "STORAGE_FAILED";
    case OTA_RECOVERY_HEALTH:return "HEALTH_REFUSED";case OTA_RECOVERY_CONFIRM:return "CONFIRM_FAILED";
    default:return "UNKNOWN_ERROR";
    }
}
