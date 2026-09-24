/* SPDX-License-Identifier: Apache-2.0 */
#if !defined(CONFIG_OWNTECH_OTA_RECOVERY) || defined(CONFIG_OWNTECH_OTA)
#error "This main belongs only to the dedicated, explicitly provisioned recovery image"
#endif
#include "recovery_core.h"
#include "owntech_ota_recovery_config.h"
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

extern uint8_t dt_leg_count;
extern uint16_t dt_pin_driver[],dt_pin_capacitor[];

namespace {
static const uint8_t allowed_euis[][8]=OWNTECH_OTA_RECOVERY_TARGET_EUIS;
static const uint8_t original_hashes[][32]=OWNTECH_OTA_RECOVERY_ORIGINAL_HASHES;
static OtaRecoveryConfig config={OWNTECH_OTA_RECOVERY_CAMPAIGN_ID,
    OWNTECH_OTA_RECOVERY_LEAD_EUI_BYTES,OWNTECH_OTA_RECOVERY_IMAGE_HASH_BYTES,
    OWNTECH_OTA_RECOVERY_IMAGE_SIZE,OWNTECH_OTA_RECOVERY_TARGET_COUNT,{}};
static_assert(sizeof(allowed_euis)/8==OWNTECH_OTA_RECOVERY_TARGET_COUNT,"EUI count");
static_assert(sizeof(original_hashes)/32==OWNTECH_OTA_RECOVERY_TARGET_COUNT,"hash count");
static_assert(OWNTECH_OTA_RECOVERY_TARGET_COUNT>0 && OWNTECH_OTA_RECOVERY_TARGET_COUNT<=16,"bounded targets");
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
    if(config.image_size!=FIXED_PARTITION_SIZE(slot1_partition)) return -1;
    return safe_outputs();
}
bool confirmed(void *) {return boot_is_img_confirmed();}
int confirm(void *) {return boot_write_img_confirmed();}
}

int main(void)
{
    for(size_t i=0;i<config.board_count;i++) {
        memcpy(config.boards[i].eui,allowed_euis[i],8);
        memcpy(config.boards[i].original_hash,original_hashes[i],32);
    }
    OtaRecoveryIO io={nullptr,read_key,write_key,erase_key,identity,backup_hash,health,confirmed,confirm};
    uint8_t eui[8]={};(void)identity(nullptr,eui);
    int result=safe_outputs()?OTA_RECOVERY_HEALTH:ota_recovery_run(config,io);
    for(;;) {
        (void)safe_outputs();
        printk("OTA_RECOVERY %s rc=%d EUI=%02x%02x%02x%02x%02x%02x%02x%02x confirmed=%d; outputs inhibited\n",
            ota_recovery_result_name(result),result,eui[0],eui[1],eui[2],eui[3],eui[4],eui[5],eui[6],eui[7],
            boot_is_img_confirmed());
        k_sleep(K_SECONDS(2));
    }
}
