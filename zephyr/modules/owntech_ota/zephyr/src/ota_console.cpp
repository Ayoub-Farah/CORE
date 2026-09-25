/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaService.h"
#include "ota_storage.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

/* 2400 baud requests one read-only snapshot. There is no console RX consumer,
 * second CDC, SMP, or periodic publication. FIFO writes never wait for a host;
 * a congested console may truncate the line and the PC must retry explicitly.
 * The existing CDC driver serializes FIFO and console writers with irq_lock. */
static const device *console=DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static char output[768];
static ota_observation observation;
static char identity[17], hash[65];
static int64_t previous_request=-1000;
static void hex(const uint8_t *source,size_t length,char *dest)
{
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<length;i++) {dest[2*i]=digits[source[i]>>4];dest[2*i+1]=digits[source[i]&15];}
    dest[2*length]=0;
}
static void status_work(struct k_work *)
{
    if(!device_is_ready(console) || k_uptime_get()-previous_request<250) return;
    previous_request=k_uptime_get();
    ota_service_diagnostics d{};ota_service_snapshot(&observation,&d);
    const auto &o=observation;
    hex(o.identity.eui,8,identity);hex(o.active_mcuboot_image_hash,32,hash);
    /* Build identity is generated from restricted version/hash characters.
     * Explicit precision remains a bound even before the first publication. */
    int n=snprintk(output,sizeof(output),
        "\nOTAR2 {\"service\":\"owntech-ota\",\"protocol\":2,\"image_class\":\"%s\","
        "\"identity\":\"%s\",\"version\":\"%.31s\",\"build_id\":\"%.31s\","
        "\"mcuboot_image_hash\":\"%s\",\"active_confirmed\":%s,"
        "\"local_healthy\":%s,\"healthy\":%s,\"can_ready\":%s,\"maintenance\":%s,"
        "\"phase\":\"%s\",\"error\":%d,\"available\":%s,\"slot_available\":%s,"
        "\"deferred_arm_qualified\":%s,"
        "\"hardware_id\":%u,\"layout_id\":%u,\"bootloader_id\":%u,"
        "\"slot_size\":%u,\"useful_capacity\":%u}\n",
        d.is_lead?"lead":"receiver",identity,o.active_version,o.active_build_id,hash,
        o.confirmed?"true":"false",d.local_healthy?"true":"false",d.healthy?"true":"false",
        d.can_ready?"true":"false",ota_safety_inhibited()?"true":"false",d.phase,d.error,
        d.healthy&&!d.busy&&!d.error&&o.identity.slot_available?"true":"false",o.identity.slot_available?"true":"false",
        ota_storage_receiver_qualified()?"true":"false",
        o.identity.hardware_id,o.identity.layout_id,o.identity.bootloader_id,
        o.identity.usable_slot_size,o.identity.usable_image_size);
    if(n>0 && (size_t)n<sizeof(output)) (void)uart_fifo_fill(console,(const uint8_t *)output,n);
}
K_WORK_DEFINE(ota_console_work,status_work);
extern "C" void ota_console_request_status(void) { k_work_submit(&ota_console_work); }
