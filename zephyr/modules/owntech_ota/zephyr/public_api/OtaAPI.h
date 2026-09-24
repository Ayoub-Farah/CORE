/* SPDX-License-Identifier: Apache-2.0 */
#ifndef OWNTECH_OTA_API_H
#define OWNTECH_OTA_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_PROTOCOL_VERSION 1U
#define OTA_MAX_PAYLOAD 256U
#define OTA_HEADER_SIZE 32U
#define OTA_MAX_REPORT_SIZE 320U
#define OTA_MAX_TARGETS 16U
#define OTA_IDENTITY_TEXT_SIZE 32U

enum ota_state {
    OTA_IDLE, OTA_PREPARING, OTA_READY, OTA_PASS_OPEN, OTA_PASS_CLOSED,
    OTA_VERIFYING, OTA_VALID, OTA_COMMITTED, OTA_REBOOTING, OTA_FAILED,
    OTA_ABORTED, OTA_RECOVERY_REQUIRED, OTA_SUCCEEDED
};

enum ota_result {
    OTA_OK = 0, OTA_IGNORED = 1, OTA_AGAIN = 2,
    OTA_ERR_ARGUMENT = -1, OTA_ERR_STATE = -2, OTA_ERR_CONFLICT = -3,
    OTA_ERR_IDENTITY = -4, OTA_ERR_CAPACITY = -5, OTA_ERR_COMPATIBILITY = -6,
    OTA_ERR_STORAGE = -7, OTA_ERR_SAFETY = -8, OTA_ERR_JOURNAL = -9,
    OTA_ERR_FORMAT = -10, OTA_ERR_CRC = -11, OTA_ERR_OFFSET = -12,
    OTA_ERR_INCOMPLETE = -13, OTA_ERR_TIMEOUT = -14, OTA_ERR_PASSES = -15,
    OTA_ERR_NO_PROGRESS = -16, OTA_ERR_TRANSPORT = -17,
    OTA_ERR_HEALTH = -18, OTA_ERR_IMAGE = -19, OTA_ERR_QUEUE_FULL = -20
};

/* IDs identify the complete provisioned compatibility tuple, including revisions.
 * artifact_sha256 is SHA256 of the exact padded firmware.mcuboot.bin file, not MCUboot's image hash. */
struct ota_manifest {
    uint64_t campaign_id;
    uint32_t image_size; /* Exact padded file length. */
    uint32_t image_content_size; /* Header + program + protected/unprotected TLVs. */
    uint32_t hardware_id;
    uint32_t layout_id;
    uint32_t bootloader_id;
    uint8_t protocol_version;
    uint8_t artifact_sha256[32];
    uint8_t mcuboot_image_hash[32];
    char version[OTA_IDENTITY_TEXT_SIZE];
    char build_id[OTA_IDENTITY_TEXT_SIZE];
};

struct ota_identity {
    uint8_t eui[8];
    uint8_t address;
    uint8_t protocol_version;
    uint32_t usable_slot_size; /* Maximum padded artifact length. */
    uint32_t usable_image_size; /* Qualified useful content bound. */
    uint32_t hardware_id;
    uint32_t layout_id;
    uint32_t bootloader_id;
    bool active_confirmed;
    bool slot_available;
};

struct ota_status {
    uint64_t campaign_id;
    uint32_t pass_id;
    uint32_t offset; /* Accepted by writer; may still be buffered, not durable. */
    uint32_t image_size;
    uint32_t commit_id;
    enum ota_state state;
    int error;
    bool pass_has_hole;
    bool adopted;
    bool flash_complete;
    bool validated;
    uint32_t rx_rejected;
    uint32_t rx_dropped;
    uint16_t queue_depth; /* Adapter reports its ordered queue depth. */
};

/* All callbacks run in the OTA worker, never CAN RX/ISR or under ThingSet locks.
 * prepare must atomically reserve the shared slot owner and revalidate MCUboot
 * state, establish safe maintenance, persist/verify its marker BEFORE erase,
 * then initialize the writer. adopt=true instead claims a closed, fully staged
 * USB image for exclusive reading; it MUST NOT erase or initialize a writer.
 * prepare failure must clean up any partial acquisition, retaining maintenance.
 * validate rereads exact image_size bytes, checks artifact SHA256, padded image
 * bounds/TLV and compatibility. It is not authoritative signature validation.
 * journal persists campaign/state/commit and maintenance through the existing
 * NVS owner. Commit only authorizes reset: padded files may already be pending.
 * All callbacks return 0 on success.
 * close releases resources WITHOUT clearing maintenance or erasing an armed slot.
 * schedule_reboot must preserve the first deadline for the same commit.
 */
struct ota_participant_hooks {
    void *context;
    int (*prepare)(void *, const struct ota_manifest *, bool adopt);
    int (*append)(void *, uint32_t offset, const uint8_t *, size_t);
    int (*flush)(void *);
    int (*validate)(void *, const struct ota_manifest *);
    int (*journal)(void *, const struct ota_manifest *, enum ota_state, uint32_t commit_id);
    int (*schedule_reboot)(void *, uint32_t commit_id, uint32_t delay_ms);
    void (*close)(void *);
};

struct ota_participant {
    struct ota_identity identity;
    struct ota_manifest manifest;
    struct ota_status status;
    struct ota_participant_hooks hooks;
    uint8_t lead_address;
    uint8_t lead_eui[8];
    uint32_t pass_start;
    bool flush_attempted;
    bool finalize_attempted;
    bool commit_attempted;
    bool reboot_scheduled;
};

/* The adapter copies commands and raw reports into ONE bounded FIFO. Only its
 * single worker calls this API; end_pass thus executes after preceding writes.
 * A status snapshot must be synchronized with that worker. On queue exhaustion
 * enqueue no partial item, count the loss and call note_loss in worker order.
 * No partial transfer is restored after reset. Persisted campaigns enter
 * RECOVERY_REQUIRED until journal, running image and MCUboot are reconciled.
 */
void ota_participant_init(struct ota_participant *, const struct ota_identity *,
                          const struct ota_participant_hooks *, bool recovery_required);
int ota_participant_prepare(struct ota_participant *, const struct ota_manifest *,
                            const uint8_t lead_eui[8], uint8_t lead_address, bool adopt);
int ota_participant_begin_pass(struct ota_participant *, uint64_t campaign_id,
                               uint32_t pass_id, uint32_t start_offset);
int ota_participant_end_pass(struct ota_participant *, uint64_t campaign_id, uint32_t pass_id);
int ota_participant_finalize(struct ota_participant *, uint64_t campaign_id);
int ota_participant_commit(struct ota_participant *, uint64_t campaign_id, uint32_t commit_id);
int ota_participant_abort(struct ota_participant *, uint64_t campaign_id);
int ota_participant_report(struct ota_participant *, uint8_t source_address,
                           const uint8_t *, size_t length, bool can_fd);
void ota_participant_note_loss(struct ota_participant *, uint32_t count);

enum ota_command_type {
    OTA_CMD_PREPARE, OTA_CMD_BEGIN_PASS, OTA_CMD_END_PASS,
    OTA_CMD_FINALIZE, OTA_CMD_COMMIT, OTA_CMD_ABORT
};

/* A transport must copy this command before returning accepted. PREPARE's
 * manifest is by value so retries never refer to mutable parser storage. */
struct ota_command {
    enum ota_command_type type;
    struct ota_manifest manifest;
    uint8_t lead_eui[8];
    uint8_t lead_address;
    bool adopt;
    uint32_t pass_id;
    uint32_t start_offset;
    uint32_t commit_id;
};

int ota_participant_command(struct ota_participant *, const struct ota_command *);

struct ota_observation {
    struct ota_identity identity;
    struct ota_status status;
    uint8_t active_mcuboot_image_hash[32];
    char active_version[OTA_IDENTITY_TEXT_SIZE];
    char active_build_id[OTA_IDENTITY_TEXT_SIZE];
    bool healthy;
    bool confirmed;
    bool rolled_back;
};

struct ota_target {
    struct ota_identity identity;
    bool is_lead;
};

struct ota_coordinator_options {
    uint32_t total_timeout_ms;
    uint32_t command_timeout_ms;
    uint32_t retry_interval_ms;
    uint32_t inter_block_ms;
    uint32_t reboot_delay_ms;
    uint16_t max_passes;
    uint16_t max_stalled_passes;
};

/* Nonblocking transport adapters: send_command/report may return OTA_AGAIN
 * (busy), OTA_OK (accepted, not necessarily executed) or a negative error.
 * read_status addresses the frozen EUI and returns a fresh observation, or
 * OTA_AGAIN until available. Before reboot, EUI AND address must be unchanged.
 * After reboot the discovery layer may resolve that EUI at a new address.
 * report_complete returns OTA_OK only after true TX completion, OTA_AGAIN while
 * in flight, negative on failure. send_report must retain/copy bytes until then.
 * read_image reads the exclusively owned Lead slot; it never changes its writer.
 * persist records the manifest, COMPLETE frozen target list and commit before
 * COMMIT; return failure if durability is unavailable. reboot_lead schedules the
 * local reboot only after broadcast TX completion. On restart, reconstruct the
 * same list/manifest from the journal and call resume_reconciliation.
 */
struct ota_coordinator_hooks {
    void *context;
    int (*send_command)(void *, const struct ota_target *, const struct ota_command *);
    int (*read_status)(void *, const struct ota_target *, struct ota_observation *);
    int (*read_image)(void *, uint32_t offset, uint8_t *, size_t);
    int (*send_report)(void *, const uint8_t *, size_t);
    int (*report_complete)(void *);
    int (*persist)(void *, const struct ota_manifest *, const struct ota_target *,
                    size_t target_count, uint32_t commit_id, enum ota_state);
    int (*reboot_lead)(void *, uint64_t campaign_id, uint32_t commit_id, uint32_t delay_ms);
};

enum ota_coordinator_phase {
    OTA_COORD_IDLE, OTA_COORD_PREPARE, OTA_COORD_BEGIN, OTA_COORD_STREAM,
    OTA_COORD_END, OTA_COORD_FINALIZE, OTA_COORD_VALIDATE_BARRIER,
    OTA_COORD_COMMIT, OTA_COORD_REBOOT, OTA_COORD_RECONCILE,
    OTA_COORD_DONE, OTA_COORD_FAILED, OTA_COORD_RECOVERY
};

struct ota_coordinator {
    struct ota_manifest manifest;
    struct ota_target targets[OTA_MAX_TARGETS];
    struct ota_coordinator_options options;
    struct ota_coordinator_hooks hooks;
    enum ota_coordinator_phase phase;
    enum ota_state state;
    int error;
    uint32_t commit_id;
    uint32_t pass_id;
    uint32_t pass_start;
    uint32_t tx_offset;
    uint32_t repair_offset;
    uint32_t last_min_offset;
    uint16_t passes;
    uint16_t stalled_passes;
    uint8_t target_count;
    uint8_t current_target;
    uint8_t lead_index;
    uint8_t committed_count;
    uint64_t started_ms;
    uint64_t phase_started_ms;
    uint64_t target_started_ms;
    uint64_t last_command_ms;
    uint64_t next_block_ms;
    bool command_sent;
    bool tx_pending;
    bool commit_may_have_executed;
    uint8_t tx_buffer[OTA_HEADER_SIZE + OTA_MAX_PAYLOAD];
    size_t tx_length;
    uint32_t seen_postboot_addresses[8];
    struct ota_observation observations[OTA_MAX_TARGETS];
    bool commit_requested;
};

void ota_coordinator_default_options(struct ota_coordinator_options *);
int ota_coordinator_start(struct ota_coordinator *, const struct ota_manifest *,
                           const struct ota_target *, size_t target_count,
                           uint32_t commit_id, const struct ota_coordinator_options *,
                           const struct ota_coordinator_hooks *, uint64_t now_ms);
/* Call regularly from a worker with a monotonic, non-wrapping millisecond clock.
 * One call performs bounded work (one control/status exchange or one block).
 * OTA_AGAIN denotes ongoing work; terminal errors remain observable. */
int ota_coordinator_step(struct ota_coordinator *, uint64_t now_ms);
/* Explicit all-VALID barrier; never requests an MCUboot arm operation. */
int ota_coordinator_commit(struct ota_coordinator *, uint64_t campaign_id, uint32_t commit_id);
int ota_coordinator_abort(struct ota_coordinator *);
int ota_coordinator_resume_reconciliation(struct ota_coordinator *,
        const struct ota_manifest *, const struct ota_target *, size_t target_count,
        uint32_t commit_id, const struct ota_coordinator_options *,
        const struct ota_coordinator_hooks *, uint64_t now_ms);

#ifdef __cplusplus
}
#endif
#endif
