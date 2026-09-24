/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaAPI.h"
#include "ota_protocol.h"
#include <string.h>

static int fail(ota_coordinator *c, int error)
{
    c->error = error; c->state = OTA_FAILED; c->phase = OTA_COORD_FAILED;
    /* Keep every frozen identity and observation for diagnostics/reconciliation. */
    if (c->hooks.persist) (void)c->hooks.persist(c->hooks.context, &c->manifest, c->targets,
                                               c->target_count, c->commit_id, OTA_FAILED);
    return error;
}
static void phase(ota_coordinator *c, ota_coordinator_phase p, uint64_t now)
{
    c->phase = p; c->phase_started_ms = now; c->current_target = 0;
    c->target_started_ms = now; c->command_sent = false; c->status_polled = false;
}
static void next(ota_coordinator *c, uint64_t now)
{ ++c->current_target; c->target_started_ms = now; c->command_sent = false; c->status_polled = false; }
void ota_coordinator_default_options(ota_coordinator_options *o)
{
    o->total_timeout_ms = 600000; o->command_timeout_ms = 20000; o->retry_interval_ms = 250;
    o->inter_block_ms = 10; o->reboot_delay_ms = 1000; o->max_passes = 5; o->max_stalled_passes = 2;
}
static int init(ota_coordinator *c, const ota_manifest *m, const ota_target *targets, size_t count,
                uint32_t commit, const ota_coordinator_options *options,
                const ota_coordinator_hooks *hooks, uint64_t now, bool recovery)
{
    if (!c || !m || !targets || !count || count > OTA_MAX_TARGETS || !commit || !hooks ||
        !m->campaign_id || !m->image_size || !m->image_content_size ||
        m->image_content_size > m->image_size || m->protocol_version != OTA_PROTOCOL_VERSION ||
        !hooks->send_command || !hooks->read_status || !hooks->read_image ||
        !hooks->send_report || !hooks->report_complete || !hooks->persist || !hooks->reboot_lead)
        return OTA_ERR_ARGUMENT;
    unsigned leads = 0;
    for (size_t i = 0; i < count; ++i) {
        const ota_identity &id = targets[i].identity;
        if (!id.address || id.address >= 254) return OTA_ERR_IDENTITY;
        uint8_t nonzero = 0; for (size_t k = 0; k < 8; ++k) nonzero |= id.eui[k];
        if (!nonzero) return OTA_ERR_IDENTITY;
        for (size_t j = 0; j < i; ++j)
            if (!memcmp(id.eui, targets[j].identity.eui, 8) || id.address == targets[j].identity.address)
                return OTA_ERR_IDENTITY;
        if (id.protocol_version != OTA_PROTOCOL_VERSION || id.hardware_id != m->hardware_id ||
            id.layout_id != m->layout_id || id.bootloader_id != m->bootloader_id) return OTA_ERR_COMPATIBILITY;
        if (id.usable_slot_size < m->image_size || id.usable_image_size < m->image_content_size)
            return OTA_ERR_CAPACITY;
        if (!recovery && !targets[i].is_lead && (!id.active_confirmed || !id.slot_available)) return OTA_ERR_STATE;
        if (targets[i].is_lead) ++leads;
    }
    if (leads != 1) return OTA_ERR_IDENTITY;
    memset(c, 0, sizeof(*c)); c->manifest = *m; c->hooks = *hooks; c->commit_id = commit;
    if (options) c->options = *options; else ota_coordinator_default_options(&c->options);
    if (!c->options.total_timeout_ms || !c->options.command_timeout_ms ||
        !c->options.retry_interval_ms || !c->options.max_passes || !c->options.max_stalled_passes ||
        !c->options.reboot_delay_ms || c->options.reboot_delay_ms > 120000) return OTA_ERR_ARGUMENT;
    memcpy(c->targets, targets, count * sizeof(*targets)); c->target_count = (uint8_t)count;
    for (size_t i = 0; i < count; ++i) if (targets[i].is_lead) c->lead_index = (uint8_t)i;
    c->started_ms = now; c->state = recovery ? OTA_REBOOTING : OTA_PREPARING;
    c->pass_id = 1; c->passes = 1; c->repair_offset = m->image_size;
    phase(c, recovery ? OTA_COORD_RECONCILE : OTA_COORD_PREPARE, now);
    return OTA_OK;
}
int ota_coordinator_start(ota_coordinator *c, const ota_manifest *m, const ota_target *targets,
                         size_t count, uint32_t commit, const ota_coordinator_options *options,
                         const ota_coordinator_hooks *hooks, uint64_t now)
{ return init(c, m, targets, count, commit, options, hooks, now, false); }
int ota_coordinator_resume_reconciliation(ota_coordinator *c, const ota_manifest *m,
        const ota_target *targets, size_t count, uint32_t commit,
        const ota_coordinator_options *options, const ota_coordinator_hooks *hooks, uint64_t now)
{ return init(c, m, targets, count, commit, options, hooks, now, true); }
int ota_coordinator_commit(ota_coordinator *c, uint64_t campaign, uint32_t commit)
{
    if (!c || campaign != c->manifest.campaign_id || commit != c->commit_id) return OTA_ERR_CONFLICT;
    if (c->commit_requested) return c->error ? c->error : OTA_OK;
    if (c->phase != OTA_COORD_VALIDATE_BARRIER || c->state != OTA_VALID) return OTA_ERR_STATE;
    if (c->hooks.persist(c->hooks.context, &c->manifest, c->targets, c->target_count,
                         c->commit_id, OTA_COMMITTED)) return fail(c, OTA_ERR_JOURNAL);
    c->commit_requested = true;
    return OTA_OK;
}
static int control(ota_coordinator *c, ota_command_type command, uint64_t now)
{
    ota_target *target = &c->targets[c->current_target];
    /* Always collect a status after an accepted command, even if the command
     * round trip itself exceeded the retry interval. */
    if (!c->command_sent || (c->status_polled && now - c->last_command_ms >= c->options.retry_interval_ms)) {
        ota_command cmd = {};
        cmd.type = command; cmd.manifest = c->manifest; cmd.pass_id = c->pass_id;
        cmd.start_offset = c->pass_start; cmd.commit_id = c->commit_id; cmd.adopt = target->is_lead;
        cmd.lead_address = c->targets[c->lead_index].identity.address;
        memcpy(cmd.lead_eui, c->targets[c->lead_index].identity.eui, 8);
        int rc = c->hooks.send_command(c->hooks.context, target, &cmd);
        if (rc < 0) return fail(c, rc);
        if (rc == OTA_AGAIN) return OTA_AGAIN;
        c->command_sent = true; c->status_polled = false; c->last_command_ms = now;
        if (command == OTA_CMD_COMMIT) c->commit_may_have_executed = true;
        return OTA_AGAIN;
    }
    ota_observation observation = {};
    c->status_polled = true;
    int rc = c->hooks.read_status(c->hooks.context, target, &observation);
    if (rc < 0) return fail(c, rc);
    if (rc == OTA_AGAIN) return rc;
    if (memcmp(observation.identity.eui, target->identity.eui, 8) ||
        observation.identity.address != target->identity.address) return fail(c, OTA_ERR_IDENTITY);
    c->observations[c->current_target] = observation;
    const ota_status &s = observation.status;
    if (s.error || s.state == OTA_FAILED || s.state == OTA_ABORTED || s.state == OTA_RECOVERY_REQUIRED)
        return fail(c, s.error ? s.error : OTA_ERR_STATE);
    if (s.campaign_id != c->manifest.campaign_id) return OTA_AGAIN;
    if (s.offset > c->manifest.image_size || s.image_size != c->manifest.image_size) return fail(c, OTA_ERR_OFFSET);
    bool ready = false;
    switch (command) {
    case OTA_CMD_PREPARE: ready = s.state == OTA_READY; break;
    case OTA_CMD_BEGIN_PASS: ready = s.state == OTA_PASS_OPEN && s.pass_id == c->pass_id; break;
    case OTA_CMD_END_PASS: ready = s.state == OTA_PASS_CLOSED && s.pass_id == c->pass_id && !s.queue_depth; break;
    case OTA_CMD_FINALIZE: ready = s.state == OTA_VALID && s.flash_complete && s.validated; break;
    case OTA_CMD_COMMIT: ready = s.state == OTA_COMMITTED && s.commit_id == c->commit_id; break;
    default: break;
    }
    if (!ready) return OTA_AGAIN;
    if (command == OTA_CMD_END_PASS && s.offset < c->repair_offset) c->repair_offset = s.offset;
    if (command == OTA_CMD_COMMIT) ++c->committed_count;
    next(c, now);
    return OTA_AGAIN;
}
int ota_coordinator_step(ota_coordinator *c, uint64_t now)
{
    if (!c || c->phase == OTA_COORD_IDLE) return OTA_ERR_STATE;
    if (c->phase == OTA_COORD_DONE) return OTA_OK;
    if (c->phase == OTA_COORD_FAILED) return c->error;
    if (now < c->started_ms || now - c->started_ms > c->options.total_timeout_ms) return fail(c, OTA_ERR_TIMEOUT);
    if (c->phase == OTA_COORD_VALIDATE_BARRIER) {
        if (c->commit_requested) { c->state = OTA_COMMITTED; phase(c, OTA_COORD_COMMIT, now); }
        return OTA_AGAIN;
    }
    if (c->phase == OTA_COORD_STREAM) {
        if (c->tx_pending) {
            int rc = c->hooks.report_complete(c->hooks.context);
            if (rc < 0) return fail(c, OTA_ERR_TRANSPORT);
            if (rc == OTA_AGAIN) return rc;
            c->tx_pending = false; c->tx_offset += (uint32_t)(c->tx_length - OTA_HEADER_SIZE);
            c->next_block_ms = now + c->options.inter_block_ms;
        }
        if (c->tx_offset == c->manifest.image_size) {
            c->repair_offset = c->manifest.image_size; phase(c, OTA_COORD_END, now); return OTA_AGAIN;
        }
        if (now < c->next_block_ms) return OTA_AGAIN;
        uint8_t data[OTA_MAX_PAYLOAD];
        size_t n = c->manifest.image_size - c->tx_offset;
        if (n > OTA_MAX_PAYLOAD) n = OTA_MAX_PAYLOAD;
        if (c->hooks.read_image(c->hooks.context, c->tx_offset, data, n)) return fail(c, OTA_ERR_STORAGE);
        ota_report report = {OTA_REPORT_DATA, c->manifest.campaign_id, c->pass_id,
                             c->tx_offset, (uint16_t)n, data};
        if (ota_report_encode(&report, c->tx_buffer, sizeof(c->tx_buffer), &c->tx_length)) return fail(c, OTA_ERR_FORMAT);
        int rc = c->hooks.send_report(c->hooks.context, c->tx_buffer, c->tx_length);
        if (rc < 0) return fail(c, OTA_ERR_TRANSPORT);
        c->tx_pending = rc == OTA_OK;
        return OTA_AGAIN;
    }
    if (c->phase == OTA_COORD_REBOOT) {
        if (!c->tx_pending) {
            int rc = ota_report_encode_reboot(c->manifest.campaign_id, c->commit_id,
                c->options.reboot_delay_ms, c->tx_buffer, sizeof(c->tx_buffer), &c->tx_length);
            if (rc) return fail(c, rc);
            rc = c->hooks.send_report(c->hooks.context, c->tx_buffer, c->tx_length);
            if (rc < 0) return fail(c, OTA_ERR_TRANSPORT);
            c->tx_pending = rc == OTA_OK;
            return OTA_AGAIN;
        }
        int rc = c->hooks.report_complete(c->hooks.context);
        if (rc < 0) return fail(c, OTA_ERR_TRANSPORT);
        if (rc == OTA_AGAIN) return rc;
        if (c->hooks.persist(c->hooks.context, &c->manifest, c->targets, c->target_count,
                             c->commit_id, OTA_REBOOTING)) return fail(c, OTA_ERR_JOURNAL);
        if (c->hooks.reboot_lead(c->hooks.context, c->manifest.campaign_id, c->commit_id,
                                c->options.reboot_delay_ms)) return fail(c, OTA_ERR_STATE);
        c->state = OTA_REBOOTING; phase(c, OTA_COORD_RECONCILE, now);
        return OTA_AGAIN;
    }
    if (c->current_target < c->target_count && now - c->target_started_ms > c->options.command_timeout_ms)
        return fail(c, OTA_ERR_TIMEOUT);
    if ((c->phase == OTA_COORD_BEGIN || c->phase == OTA_COORD_END) &&
        c->current_target < c->target_count && c->targets[c->current_target].is_lead) {
        next(c, now); return OTA_AGAIN;
    }
    if (c->current_target == c->target_count) {
        switch (c->phase) {
        case OTA_COORD_PREPARE: phase(c, OTA_COORD_BEGIN, now); break;
        case OTA_COORD_BEGIN: c->state = OTA_PASS_OPEN; phase(c, OTA_COORD_STREAM, now); break;
        case OTA_COORD_END:
            if (c->repair_offset == c->manifest.image_size) { phase(c, OTA_COORD_FINALIZE, now); break; }
            if (c->repair_offset <= c->last_min_offset) ++c->stalled_passes; else c->stalled_passes = 0;
            if (c->stalled_passes >= c->options.max_stalled_passes) return fail(c, OTA_ERR_NO_PROGRESS);
            if (c->passes >= c->options.max_passes) return fail(c, OTA_ERR_PASSES);
            ++c->passes; ++c->pass_id; c->last_min_offset = c->repair_offset;
            c->pass_start = c->repair_offset; c->tx_offset = c->repair_offset;
            phase(c, OTA_COORD_BEGIN, now); break;
        case OTA_COORD_FINALIZE:
            if (c->hooks.persist(c->hooks.context, &c->manifest, c->targets, c->target_count,
                                 c->commit_id, OTA_VALID)) return fail(c, OTA_ERR_JOURNAL);
            c->state = OTA_VALID; phase(c, OTA_COORD_VALIDATE_BARRIER, now); break;
        case OTA_COORD_COMMIT: phase(c, OTA_COORD_REBOOT, now); break;
        case OTA_COORD_RECONCILE:
            if (c->hooks.persist(c->hooks.context, &c->manifest, c->targets, c->target_count,
                                 c->commit_id, OTA_SUCCEEDED)) return fail(c, OTA_ERR_JOURNAL);
            c->state = OTA_SUCCEEDED; c->phase = OTA_COORD_DONE; return OTA_OK;
        default: return fail(c, OTA_ERR_STATE);
        }
        return OTA_AGAIN;
    }
    if (c->phase == OTA_COORD_RECONCILE) {
        ota_target *t = &c->targets[c->current_target];
        ota_observation observation = {};
        int rc = c->hooks.read_status(c->hooks.context, t, &observation);
        if (rc < 0) return fail(c, rc);
        if (rc == OTA_AGAIN) return rc;
        if (memcmp(t->identity.eui, observation.identity.eui, 8)) return fail(c, OTA_ERR_IDENTITY);
        c->observations[c->current_target] = observation;
        if (observation.rolled_back) return fail(c, OTA_ERR_IMAGE);
        if (!observation.healthy || !observation.confirmed ||
            memcmp(observation.active_mcuboot_image_hash, c->manifest.mcuboot_image_hash, 32) ||
            memcmp(observation.active_version, c->manifest.version, OTA_IDENTITY_TEXT_SIZE) ||
            memcmp(observation.active_build_id, c->manifest.build_id, OTA_IDENTITY_TEXT_SIZE)) return OTA_AGAIN;
        uint8_t address = observation.identity.address;
        if (!address || address >= 254 || (c->seen_postboot_addresses[address / 32] & (1U << (address % 32))))
            return fail(c, OTA_ERR_IDENTITY);
        c->seen_postboot_addresses[address / 32] |= 1U << (address % 32);
        next(c, now); return OTA_AGAIN;
    }
    switch (c->phase) {
    case OTA_COORD_PREPARE: return control(c, OTA_CMD_PREPARE, now);
    case OTA_COORD_BEGIN: return control(c, OTA_CMD_BEGIN_PASS, now);
    case OTA_COORD_END: return control(c, OTA_CMD_END_PASS, now);
    case OTA_COORD_FINALIZE: return control(c, OTA_CMD_FINALIZE, now);
    case OTA_COORD_COMMIT: return control(c, OTA_CMD_COMMIT, now);
    default: return fail(c, OTA_ERR_STATE);
    }
}
int ota_coordinator_abort(ota_coordinator *c)
{
    if (!c || c->state == OTA_REBOOTING || c->state == OTA_SUCCEEDED) return OTA_ERR_STATE;
    ota_command cmd = {}; cmd.type = OTA_CMD_ABORT; cmd.manifest = c->manifest;
    cmd.lead_address = c->targets[c->lead_index].identity.address;
    memcpy(cmd.lead_eui, c->targets[c->lead_index].identity.eui, 8);
    int result = OTA_OK;
    for (size_t i = 0; i < c->target_count; ++i) {
        int rc = c->hooks.send_command(c->hooks.context, &c->targets[i], &cmd);
        if (rc != OTA_OK) result = rc;
    }
    (void)fail(c, result < 0 ? result : OTA_ERR_STATE); c->state = OTA_ABORTED;
    return result;
}
