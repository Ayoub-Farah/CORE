/* Run the production shared NVS adapter with flash and scheduler boundaries
 * replaced. The lock shim can change service state on lock acquisition. */
#include <string.h>
#include "OtaService.h"
#include "../../zephyr/modules/owntech_flash_driver/zephyr/public_api/nvs_storage.c"
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
#endif
#define CHECK(condition) do { if (!(condition)) return __LINE__; } while (0)

const struct device nvs_test_device = {"test flash"};
static bool inhibited, busy, device_ready = true, busy_on_lock;
static bool busy_during_write;
static int bad_locks, mounts, writes, clears, max_lock_depth;
static uint16_t stored_version;
static uint16_t last_key;
static uint32_t last_value;

extern "C" bool ota_safety_inhibited(void) { return inhibited; }
extern "C" bool ota_service_busy(void) { return busy; }
int printk(const char *, ...) { return 0; }
int k_mutex_lock(struct k_mutex *mutex, int)
{
    ++mutex->depth;
    if (mutex->depth > max_lock_depth) max_lock_depth = mutex->depth;
    if (busy_on_lock) { busy = true; busy_on_lock = false; }
    return 0;
}
int k_mutex_unlock(struct k_mutex *mutex)
{
    if (mutex->depth != 1) ++bad_locks;
    --mutex->depth;
    return 0;
}
static void require_lock()
{ if (storage_mutex.depth != 1) ++bad_locks; }
bool device_is_ready(const struct device *) { return device_ready; }
int flash_get_page_info_by_offs(const struct device *, size_t, struct flash_pages_info *info)
{ require_lock(); info->size = 2048; return 0; }
int nvs_mount(struct nvs_fs *) { require_lock(); ++mounts; return 0; }
int nvs_read(struct nvs_fs *, uint16_t key, void *data, size_t size)
{
    require_lock();
    if (key == VERSION && stored_version) {
        if (size >= sizeof(stored_version)) memcpy(data, &stored_version, sizeof(stored_version));
        return sizeof(stored_version);
    }
    if (key == last_key) {
        if (size >= sizeof(last_value)) memcpy(data, &last_value, sizeof(last_value));
        return sizeof(last_value);
    }
    return -ENOENT;
}
int nvs_write(struct nvs_fs *, uint16_t key, const void *data, size_t size)
{
    require_lock(); ++writes;
    if (key == VERSION) {
        if (size != sizeof(stored_version)) return -EIO;
        memcpy(&stored_version, data, size);
    }
    else {
        last_key = key;
        if (size == sizeof(last_value)) memcpy(&last_value, data, size);
    }
    if (busy_during_write) { busy = true; busy_during_write = false; }
    return static_cast<int>(size);
}
int nvs_clear(struct nvs_fs *)
{
    require_lock(); ++clears;
    stored_version = 0; last_key = 0; last_value = 0;
    return 0;
}
int nvs_calc_free_space(struct nvs_fs *) { require_lock(); return 1024; }

static int test_nvs()
{
    const uint16_t application_key = ADC_CALIBRATION + 1;
    uint32_t value = 42, readback = 0;
    /* Getters can be the first callers; mounting/version inspection and the
     * capacity calculation all use the same lock as writes. */
    CHECK(nvs_storage_get_current_version() == 1);
    CHECK(nvs_storage_get_version_in_nvs() == 0);
    CHECK(nvs_storage_get_free_space() == 1024);
    CHECK(mounts == 1 && !writes && !bad_locks);
    /* Reset volatile state so boot-inhibition tests also cover an unmounted
     * partition. No actual record was written by these read-only getters. */
    initialized = false; mounts = 0;
#ifdef CONFIG_OWNTECH_OTA
    /* Boot inhibition must reject even the first application write, without
     * mounting/writing a version or erasing any storage. */
    inhibited = true;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_store_data(application_key, &value, sizeof(value)) == -EBUSY);
    CHECK(!mounts && !writes);
    CHECK(nvs_storage_clear_all_stored_data() == -EPERM);
    CHECK(!mounts && !clears);

    /* Internal journal writes retain their implicit version write even while
     * inhibited, and all four actual OTA keys remain usable. */
    for (uint16_t key = 0x0500; key <= 0x0503; ++key)
        CHECK(nvs_storage_write(key, &value, sizeof(value)) == sizeof(value));
    CHECK(mounts == 1 && writes == 5 && stored_version == 1);
    CHECK(nvs_storage_write(0x04FF, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_write(0x0504, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_write(VERSION, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_read(0x0503, &readback, sizeof(readback)) == sizeof(readback));
    CHECK(readback == value);
    CHECK(nvs_storage_get_current_version() == 1);
    CHECK(nvs_storage_get_version_in_nvs() == 1);
    CHECK(nvs_storage_get_free_space() == 1024);

    inhibited = false; busy = true;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_write(0x0502, &value, sizeof(value)) == sizeof(value));
    busy = false;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == sizeof(value));

    /* A caller waiting for the lock must recheck service state once admitted. */
    busy_on_lock = true;
    const int before = writes;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == -EBUSY);
    CHECK(writes == before && !storage_mutex.depth);

    /* An already admitted write can finish. The following application write
     * is blocked, and a subsequent OTA record still progresses under the lock. */
    busy = false; busy_during_write = true;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == sizeof(value));
    CHECK(busy);
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == -EBUSY);
    CHECK(nvs_storage_write(0x0501, &value, sizeof(value)) == sizeof(value));
    busy = false;
    CHECK(nvs_storage_store_data(application_key, &value, sizeof(value)) == sizeof(value));
    CHECK(nvs_storage_clear_all_stored_data() == -EPERM && !clears);
#else
    /* Legacy non-OTA applications retain writes and explicit clear, regardless
     * of the mock service flags. A clear remounts and restores the version. */
    inhibited = busy = true;
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == sizeof(value));
    CHECK(nvs_storage_store_data(application_key, &value, sizeof(value)) == sizeof(value));
    CHECK(mounts == 1 && stored_version == 1);
    CHECK(nvs_storage_clear_all_stored_data() == 0 && clears == 1);
    CHECK(!initialized && !stored_version);
    CHECK(nvs_storage_write(application_key, &value, sizeof(value)) == sizeof(value));
    CHECK(mounts == 2 && stored_version == 1);
    CHECK(nvs_storage_read(application_key, &readback, sizeof(readback)) == sizeof(readback));
    CHECK(readback == value);
    CHECK(nvs_storage_clear_all_stored_data() == 0 && clears == 2);
    device_ready = false;
    CHECK(nvs_storage_clear_all_stored_data() < 0 && clears == 2);
    CHECK(nvs_storage_get_current_version() == 0);
    CHECK(nvs_storage_get_version_in_nvs() == 0);
    CHECK(nvs_storage_get_free_space() < 0);
#endif
    CHECK(!bad_locks && !storage_mutex.depth && max_lock_depth == 1);
    return 0;
}
extern "C" int ota_nvs_test_run() { return test_nvs(); }
#ifndef OWNTECH_FREESTANDING_TEST
int main() { int rc = test_nvs(); printf("%d\n", rc); return rc ? 1 : 0; }
#endif
