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
static_assert(sizeof(ota_state)==1, "deployed firmware uses short enums");
static_assert(sizeof(LegacyLocal)==240 && offsetof(LegacyLocal,crc)==232, "legacy local ABI");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,state)==24, "state offset");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,lead_eui)==25, "lead offset");
static_assert(offsetof(LegacyLocal,journal)+offsetof(ota_storage_journal,mcuboot_image_hash)==65, "hash offset");
static_assert(sizeof(LegacyFleet)==824 && offsetof(LegacyFleet,crc)==820, "legacy fleet ABI");
static_assert(offsetof(LegacyFleet,manifest)+offsetof(ota_manifest,mcuboot_image_hash)==69, "fleet hash offset");
static_assert(offsetof(LegacyFleet,targets)==168 && sizeof(ota_target)==40, "fleet targets ABI");
static_assert(offsetof(ota_target,is_lead)==36, "fleet lead offset");
static_assert(offsetof(LegacyFleet,commit_id)==808 && offsetof(LegacyFleet,count)==812 &&
              offsetof(LegacyFleet,state)==816, "fleet state offsets");
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
__attribute__((used,section(".fixture_local"))) const LegacyLocal local_bytes=local_fixture();
__attribute__((used,section(".fixture_fleet"))) const LegacyFleet fleet_bytes=fleet_fixture();
