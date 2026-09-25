/* SPDX-License-Identifier: Apache-2.0 */
#if !defined(CONFIG_OWNTECH_OTA_RECOVERY) || defined(CONFIG_OWNTECH_OTA)
#error "This main belongs only to the dedicated, explicitly provisioned recovery image"
#endif
#include "recovery_core.h"
#include "owntech_ota_recovery_config.h"
#if defined(OWNTECH_OTA_TRANSITION) && OWNTECH_OTA_TRANSITION
#include "transition_core.h"
#endif
#include "SpinAPI.h"
#include "ShieldAPI.h"
#include "nvs_storage.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <stm32_ll_hrtim.h>
#include <string.h>

#if !defined(OWNTECH_OTA_TRANSITION) || !OWNTECH_OTA_TRANSITION
#ifndef OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY
#define OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY 0
#endif
#ifndef OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY
#define OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY 0
#endif
#ifndef OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY
#define OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY 0
#endif
#ifndef OWNTECH_OTA_RECOVERY_ARTIFACT_HASH_BYTES
#define OWNTECH_OTA_RECOVERY_ARTIFACT_HASH_BYTES {0}
#endif
#ifndef OWNTECH_OTA_RECOVERY_USEFUL_CAPACITY
#define OWNTECH_OTA_RECOVERY_USEFUL_CAPACITY 221184U
#endif
static_assert(OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY==0 || OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY==1,
              "explicit recovery mode must be 0 or 1");
static_assert(OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY==0 || OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY==1,
              "explicit follower recovery mode must be 0 or 1");
static_assert(!(OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY && OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY),
              "recovery modes are mutually exclusive");
static_assert(OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY==0 || OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY==1,
              "explicit compact recovery mode must be 0 or 1");
static_assert(!OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY ||
              !(OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY || OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY),
              "compact recovery is separate from legacy policies");
#endif

extern uint8_t dt_leg_count;
extern uint16_t dt_pin_driver[],dt_pin_capacitor[];

namespace {
#if defined(OWNTECH_OTA_TRANSITION) && OWNTECH_OTA_TRANSITION
static const OtaTransitionConfig config={OWNTECH_OTA_TRANSITION_EUI_BYTES,
    OWNTECH_OTA_TRANSITION_ORIGINAL_HASH_BYTES,OWNTECH_OTA_TRANSITION_TOKEN_BYTES,
    OWNTECH_OTA_TRANSITION_ROLE,OWNTECH_OTA_TRANSITION_LOCAL_LENGTH,
    OWNTECH_OTA_TRANSITION_FLEET_LENGTH,OWNTECH_OTA_TRANSITION_LOCAL_BYTES,
    OWNTECH_OTA_TRANSITION_FLEET_BYTES};
static_assert(OWNTECH_OTA_TRANSITION==1,"explicit transition mode must be 1");
static_assert(OWNTECH_OTA_TRANSITION_USEFUL_CAPACITY==221184U,"qualified transition geometry");
static_assert(sizeof(OWNTECH_OTA_TRANSITION_TOKEN_HEX)==65,"exact transition token");
#else
static const uint8_t allowed_euis[][8]=OWNTECH_OTA_RECOVERY_TARGET_EUIS;
static const uint8_t original_hashes[][32]=OWNTECH_OTA_RECOVERY_ORIGINAL_HASHES;
static OtaRecoveryConfig config={OWNTECH_OTA_RECOVERY_CAMPAIGN_ID,
    OWNTECH_OTA_RECOVERY_LEAD_EUI_BYTES,OWNTECH_OTA_RECOVERY_IMAGE_HASH_BYTES,
    OWNTECH_OTA_RECOVERY_IMAGE_SIZE,OWNTECH_OTA_RECOVERY_TARGET_COUNT,{},
    OWNTECH_OTA_RECOVERY_STAGED_LEAD_ONLY!=0,OWNTECH_OTA_RECOVERY_PREPARED_FOLLOWER_ONLY!=0,
    OWNTECH_OTA_RECOVERY_COMPACT_RECEIVER_ONLY!=0,OWNTECH_OTA_RECOVERY_ARTIFACT_HASH_BYTES};
static_assert(sizeof(allowed_euis)/8==OWNTECH_OTA_RECOVERY_TARGET_COUNT,"EUI count");
static_assert(sizeof(original_hashes)/32==OWNTECH_OTA_RECOVERY_TARGET_COUNT,"hash count");
static_assert(OWNTECH_OTA_RECOVERY_TARGET_COUNT>0 && OWNTECH_OTA_RECOVERY_TARGET_COUNT<=16,"bounded targets");
#endif
uint16_t le16(const uint8_t *p) {return uint16_t(p[0])|uint16_t(p[1])<<8;}
uint32_t le32(const uint8_t *p) {return le16(p)|uint32_t(le16(p+2))<<16;}
int read_key(void *,uint16_t key,uint8_t *data,size_t n) {return nvs_storage_read(key,data,n);}
int write_key(void *,uint16_t key,const uint8_t *data,size_t n) {return nvs_storage_write(key,data,n);}
int erase_key(void *,uint16_t key) {return nvs_storage_write(key,nullptr,0);}
int identity(void *,uint8_t eui[8])
{
    uint8_t uid[12]={};if(hwinfo_get_device_id(uid,sizeof(uid))!=12) return -1;
    uint32_t a=ota_recovery_crc32(uid,8),b=ota_recovery_crc32(uid+4,8);
    for(unsigned i=0;i<4;i++) {eui[i]=uint8_t(a>>(8*i));eui[i+4]=uint8_t(b>>(8*i));}
    eui[0]&=~2U;return 0;
}
int backup_hash(void *,uint8_t hash[32])
{
    const flash_area *area;
    if(flash_area_open(FIXED_PARTITION_ID(slot1_partition),&area)) return -1;
    uint8_t hdr[32],tlv[4];int result=-1;
    do {
        if(flash_area_read(area,0,hdr,sizeof(hdr)) || le32(hdr)!=0x96f3b83d || le16(hdr+8)<32) break;
        uint32_t payload=le32(hdr+12),offset=le16(hdr+8);
        if(offset>area->fa_size || payload>area->fa_size-offset) break;
        offset+=payload;
        if(offset>area->fa_size-4 || flash_area_read(area,offset,tlv,4)) break;
        if(le16(tlv)==0x6908) {
            uint32_t length=le16(tlv+2);
            if(length<4 || length>area->fa_size-offset) break;
            offset+=length;
            if(offset>area->fa_size-4 || flash_area_read(area,offset,tlv,4)) break;
        }
        if(le16(tlv)!=0x6907 || le16(tlv+2)<4 || le16(tlv+2)>area->fa_size-offset) break;
        uint32_t end=offset+le16(tlv+2);offset+=4;
        while(offset<=end-4) {
            if(flash_area_read(area,offset,tlv,4)) break;
            uint32_t length=le16(tlv+2);offset+=4;
            if(length>end-offset) break;
            if(tlv[0]==0x10 && !tlv[1] && length==32) {
                result=flash_area_read(area,offset,hash,32);break;
            }
            offset+=length;
        }
    } while(false);
    flash_area_close(area);return result;
}
int safe_outputs()
{
    shield.power.stop(ALL);
    for(unsigned i=0;i<dt_leg_count;i++) {
        if(dt_pin_driver[i]) spin.gpio.configurePin(dt_pin_driver[i],GPIO_OUTPUT_INACTIVE);
        if(dt_pin_capacitor[i]) spin.gpio.configurePin(dt_pin_capacitor[i],GPIO_OUTPUT_ACTIVE);
    }
    if(HRTIM1->sCommonRegs.OENR&0xFFFU) return -1;
    for(unsigned i=0;i<dt_leg_count;i++) {
        if(dt_pin_driver[i] && spin.gpio.readPin(dt_pin_driver[i])!=0) return -1;
        if(dt_pin_capacitor[i] && spin.gpio.readPin(dt_pin_capacitor[i])!=1) return -1;
    }
    return 0;
}
int health(void *)
{
    const int type=mcuboot_swap_type();
    if(type!=BOOT_SWAP_TYPE_NONE && type!=BOOT_SWAP_TYPE_REVERT) return -1;
#if defined(OWNTECH_OTA_TRANSITION) && OWNTECH_OTA_TRANSITION
    if(OWNTECH_OTA_TRANSITION_USEFUL_CAPACITY>=FIXED_PARTITION_SIZE(slot1_partition)) return -1;
#else
    if(config.compact_receiver_only) {
        if(!config.image_size || config.image_size>OWNTECH_OTA_RECOVERY_USEFUL_CAPACITY ||
           OWNTECH_OTA_RECOVERY_USEFUL_CAPACITY>=FIXED_PARTITION_SIZE(slot1_partition)) return -1;
    }
    else if(config.image_size!=FIXED_PARTITION_SIZE(slot1_partition)) return -1;
#endif
    return safe_outputs();
}
bool confirmed(void *) {return boot_is_img_confirmed();}
int confirm(void *) {return boot_write_img_confirmed();}
}

int main(void)
{
#if !defined(OWNTECH_OTA_TRANSITION) || !OWNTECH_OTA_TRANSITION
    for(size_t i=0;i<config.board_count;i++) {
        memcpy(config.boards[i].eui,allowed_euis[i],8);
        memcpy(config.boards[i].original_hash,original_hashes[i],32);
    }
#endif
    OtaRecoveryIO io={nullptr,read_key,write_key,erase_key,identity,backup_hash,health,confirmed,confirm};
    uint8_t eui[8]={};(void)identity(nullptr,eui);
#if defined(OWNTECH_OTA_TRANSITION) && OWNTECH_OTA_TRANSITION
    int result=safe_outputs()?OTA_RECOVERY_HEALTH:ota_transition_run(config,io);
#else
    int result=safe_outputs()?OTA_RECOVERY_HEALTH:ota_recovery_run(config,io);
#endif
    for(;;) {
        (void)safe_outputs();
#if defined(OWNTECH_OTA_TRANSITION) && OWNTECH_OTA_TRANSITION
        printk("OTA_TRANSITION rc=%d EUI=%02x%02x%02x%02x%02x%02x%02x%02x token=%s confirmed=%d; outputs inhibited\n",
            result,eui[0],eui[1],eui[2],eui[3],eui[4],eui[5],eui[6],eui[7],
            OWNTECH_OTA_TRANSITION_TOKEN_HEX,boot_is_img_confirmed());
#else
        printk("OTA_RECOVERY %s rc=%d EUI=%02x%02x%02x%02x%02x%02x%02x%02x confirmed=%d; outputs inhibited\n",
            ota_recovery_result_name(result),result,eui[0],eui[1],eui[2],eui[3],eui[4],eui[5],eui[6],eui[7],
            boot_is_img_confirmed());
#endif
        k_sleep(K_SECONDS(2));
    }
}
