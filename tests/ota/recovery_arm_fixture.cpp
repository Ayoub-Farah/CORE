/* SPDX-License-Identifier: Apache-2.0 */
/* Compiled for the deployed Cortex-M4 ABI, never executed on hardware. The
 * fixture uses production types so host tests cannot invent their offsets. */
#include "ota_storage.h"
#include <stddef.h>

struct LegacyLocal { uint32_t magic; ota_storage_journal journal; uint32_t crc; };
struct LegacyFleet {
    uint32_t magic, length;
    ota_manifest manifest;
    ota_target targets[16];
    uint32_t commit_id, count;
    ota_state state;
    uint32_t crc;
};
struct CompactFleet {
    uint32_t magic, length;
    uint8_t manifest[157], eui[16][8];
    uint8_t count, lead_index, state;
    uint32_t commit_id, crc;
};
static_assert(sizeof(ota_state)==1, "deployed firmware uses short enums");
static_assert(sizeof(LegacyLocal)==240 && offsetof(LegacyLocal,crc)==232, "legacy local ABI");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,state)==24, "state offset");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,lead_eui)==25, "lead offset");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,mcuboot_image_hash)==65, "hash offset");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,event_mask)==164, "event mask offset");
static_assert(sizeof(LegacyFleet)==824 && offsetof(LegacyFleet,crc)==820, "legacy fleet ABI");
static_assert(offsetof(LegacyFleet,manifest)+offsetof(ota_manifest,mcuboot_image_hash)==69, "fleet hash offset");
static_assert(offsetof(LegacyFleet,targets)==168 && sizeof(ota_target)==40, "fleet targets ABI");
static_assert(offsetof(ota_target,is_lead)==36, "fleet lead offset");
static_assert(offsetof(LegacyFleet,commit_id)==808 && offsetof(LegacyFleet,count)==812 &&
              offsetof(LegacyFleet,state)==816, "fleet state offsets");
static_assert(sizeof(CompactFleet)==304 && offsetof(CompactFleet,crc)==300, "compact fleet ABI");
static_assert(offsetof(CompactFleet,eui)==165 && offsetof(CompactFleet,count)==293 &&
              offsetof(CompactFleet,lead_index)==294 && offsetof(CompactFleet,state)==295 &&
              offsetof(CompactFleet,commit_id)==296, "compact fleet field offsets");
constexpr LegacyLocal local_fixture()
{
    LegacyLocal result{};
    result.magic=0x3141544f;
    result.journal.campaign_id=0x123456789abcdefULL;
    result.journal.image_size=227328;
    result.journal.state=OTA_VALID;
    for(unsigned i=0;i<8;i++) result.journal.lead_eui[i]=i+1;
    for(unsigned i=0;i<32;i++) result.journal.mcuboot_image_hash[i]=i+70;
    return result;
}
constexpr LegacyFleet fleet_fixture()
{
    LegacyFleet result{};
    result.magic=0x3141544f;result.length=sizeof(result);
    result.manifest.campaign_id=0x123456789abcdefULL;
    result.manifest.image_size=227328;
    for(unsigned i=0;i<32;i++) result.manifest.mcuboot_image_hash[i]=i+70;
    for(unsigned i=0;i<8;i++) {
        result.targets[0].identity.eui[i]=i+1;
        result.targets[1].identity.eui[i]=i+20;
    }
    result.targets[0].is_lead=true;
    result.commit_id=42;result.count=2;result.state=OTA_VALID;
    return result;
}
constexpr CompactFleet compact_fixture()
{
    CompactFleet result{};
    result.magic=0x3241544f;result.length=sizeof(result);
    for(unsigned i=0;i<8;i++) result.manifest[i]=uint8_t(0x123456789abcdefULL>>(i*8));
    for(unsigned i=0;i<4;i++) result.manifest[8+i]=uint8_t(227328U>>(i*8));
    for(unsigned i=0;i<32;i++) result.manifest[61+i]=i+70;
    for(unsigned i=0;i<8;i++) {result.eui[0][i]=i+1;result.eui[1][i]=i+20;}
    result.count=2;result.lead_index=0;result.state=OTA_FAILED;result.commit_id=0x89abcdefU;
    return result;
}
__attribute__((used,section(".fixture_local"))) const LegacyLocal local_bytes=local_fixture();
__attribute__((used,section(".fixture_fleet"))) const LegacyFleet fleet_bytes=fleet_fixture();
__attribute__((used,section(".fixture_compact"))) const CompactFleet compact_bytes=compact_fixture();
