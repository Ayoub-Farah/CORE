/* SPDX-License-Identifier: Apache-2.0 */
#include "OtaAPI.h"
#include "ota_protocol.h"
#include <string.h>

static bool same_manifest(const ota_manifest &a, const ota_manifest &b)
{
    return a.campaign_id == b.campaign_id && a.image_size == b.image_size &&
        a.image_content_size == b.image_content_size && a.hardware_id == b.hardware_id &&
        a.layout_id == b.layout_id && a.bootloader_id == b.bootloader_id &&
        a.protocol_version == b.protocol_version &&
        !memcmp(a.artifact_sha256, b.artifact_sha256, 32) &&
        !memcmp(a.mcuboot_image_hash, b.mcuboot_image_hash, 32) &&
        !memcmp(a.version, b.version, sizeof(a.version)) &&
        !memcmp(a.build_id, b.build_id, sizeof(a.build_id));
}
static int journal(ota_participant *p, ota_state state, uint32_t commit = 0)
{
    if (!p->hooks.journal || p->hooks.journal(p->hooks.context, &p->manifest, state, commit))
        return OTA_ERR_JOURNAL;
    p->status.state = state;
    return OTA_OK;
}
static int fail(ota_participant *p, int error)
{
    p->status.error = error;
    (void)journal(p, OTA_FAILED, p->status.commit_id);
    p->status.state = OTA_FAILED;
    if (p->hooks.close) p->hooks.close(p->hooks.context);
    return error;
}
static bool campaign(const ota_participant *p, uint64_t id)
{ return id != 0 && id == p->status.campaign_id; }

void ota_participant_init(ota_participant *p, const ota_identity *id,
                          const ota_participant_hooks *hooks, bool recovery)
{
    memset(p, 0, sizeof(*p));
    p->identity = *id; p->hooks = *hooks;
    p->status.state = recovery ? OTA_RECOVERY_REQUIRED : OTA_IDLE;
}
int ota_participant_prepare(ota_participant *p, const ota_manifest *m,
                            const uint8_t eui[8], uint8_t address, bool adopt)
{
    if (!p || !m || !eui || !m->campaign_id || !address || address >= 254 ||
        !m->image_size || !m->image_content_size || m->image_content_size > m->image_size)
        return OTA_ERR_ARGUMENT;
    uint8_t nonzero = 0; for (size_t i = 0; i < 8; ++i) nonzero |= eui[i];
    if (!nonzero) return OTA_ERR_IDENTITY;
    if (campaign(p, m->campaign_id)) {
        if (!same_manifest(p->manifest, *m) || memcmp(p->lead_eui, eui, 8) ||
            p->lead_address != address || p->status.adopted != adopt) return OTA_ERR_CONFLICT;
        return p->status.error ? p->status.error : OTA_OK;
    }
    if (p->status.state != OTA_IDLE && p->status.state != OTA_SUCCEEDED) return OTA_ERR_STATE;
    if (m->protocol_version != OTA_PROTOCOL_VERSION || m->hardware_id != p->identity.hardware_id ||
        m->layout_id != p->identity.layout_id || m->bootloader_id != p->identity.bootloader_id)
        return OTA_ERR_COMPATIBILITY;
    if (m->image_size > p->identity.usable_slot_size ||
        m->image_content_size > p->identity.usable_image_size) return OTA_ERR_CAPACITY;
    /* The staged Lead may already be pending. Its storage hook verifies exact
     * owner/manifest equality and adopts read-only without this erase gate. */
    if (!adopt && (!p->identity.active_confirmed || !p->identity.slot_available)) return OTA_ERR_STATE;
    /* A successfully reconciled previous campaign may start again. Never
     * restore a partial writer from journal offsets. */
    p->status = {};
    p->flush_attempted = p->finalize_attempted = p->commit_attempted = p->reboot_scheduled = false;
    p->pass_start = 0;
    p->manifest = *m; memcpy(p->lead_eui, eui, 8); p->lead_address = address;
    p->status.campaign_id = m->campaign_id; p->status.image_size = m->image_size;
    p->status.state = OTA_PREPARING; p->status.adopted = adopt;
    if (!p->hooks.prepare || p->hooks.prepare(p->hooks.context, m, adopt)) return fail(p, OTA_ERR_STORAGE);
    if (adopt) { p->status.offset = m->image_size; p->status.flash_complete = true; }
    if (journal(p, OTA_READY)) return fail(p, OTA_ERR_JOURNAL);
    return OTA_OK;
}
int ota_participant_begin_pass(ota_participant *p, uint64_t id, uint32_t pass, uint32_t start)
{
    if (!campaign(p, id) || !pass) return OTA_ERR_CONFLICT;
    if (p->status.adopted) return OTA_ERR_STATE;
    if (pass == p->status.pass_id) {
        return start == p->pass_start && (p->status.state == OTA_PASS_OPEN ||
               p->status.state == OTA_PASS_CLOSED) ? OTA_OK : OTA_ERR_CONFLICT;
    }
    if (pass <= p->status.pass_id || start > p->status.offset || start >= p->manifest.image_size)
        return OTA_ERR_OFFSET;
    if (p->status.state != OTA_READY && p->status.state != OTA_PASS_CLOSED) return OTA_ERR_STATE;
    p->status.pass_id = pass; p->pass_start = start; p->status.pass_has_hole = false;
    p->status.state = OTA_PASS_OPEN;
    return OTA_OK;
}
int ota_participant_end_pass(ota_participant *p, uint64_t id, uint32_t pass)
{
    if (!campaign(p, id) || pass != p->status.pass_id) return OTA_ERR_CONFLICT;
    if (p->status.state == OTA_PASS_CLOSED) return OTA_OK;
    if (p->status.state != OTA_PASS_OPEN) return OTA_ERR_STATE;
    if (p->status.queue_depth) return OTA_AGAIN;
    p->status.state = OTA_PASS_CLOSED;
    return OTA_OK;
}
int ota_participant_finalize(ota_participant *p, uint64_t id)
{
    if (!campaign(p, id)) return OTA_ERR_CONFLICT;
    if (p->finalize_attempted) return p->status.error ? p->status.error :
        (p->status.validated ? OTA_OK : OTA_ERR_STATE);
    if (p->status.queue_depth) return OTA_AGAIN;
    if (p->status.state != OTA_PASS_CLOSED && !(p->status.adopted && p->status.state == OTA_READY))
        return OTA_ERR_STATE;
    if (p->status.offset != p->manifest.image_size) return OTA_ERR_INCOMPLETE;
    p->finalize_attempted = true;
    if (!p->status.adopted) {
        p->flush_attempted = true;
        if (!p->hooks.flush || p->hooks.flush(p->hooks.context)) return fail(p, OTA_ERR_STORAGE);
    }
    p->status.flash_complete = true;
    p->status.state = OTA_VERIFYING;
    if (!p->hooks.validate || p->hooks.validate(p->hooks.context, &p->manifest)) return fail(p, OTA_ERR_IMAGE);
    p->status.validated = true;
    if (journal(p, OTA_VALID)) return fail(p, OTA_ERR_JOURNAL);
    return OTA_OK;
}
int ota_participant_commit(ota_participant *p, uint64_t id, uint32_t commit)
{
    if (!campaign(p, id) || !commit) return OTA_ERR_CONFLICT;
    if (p->commit_attempted) return p->status.commit_id == commit ?
        (p->status.error ? p->status.error : OTA_OK) : OTA_ERR_CONFLICT;
    if (p->status.state != OTA_VALID || !p->status.validated || !p->status.flash_complete) return OTA_ERR_STATE;
    p->commit_attempted = true; p->status.commit_id = commit;
    if (journal(p, OTA_COMMITTED, commit)) return fail(p, OTA_ERR_JOURNAL);
    return OTA_OK;
}
int ota_participant_abort(ota_participant *p, uint64_t id)
{
    if (!campaign(p, id)) return OTA_ERR_CONFLICT;
    if (p->status.state == OTA_ABORTED) return OTA_OK;
    /* Once a reboot timer exists, abort cannot promise cancellation. */
    if (p->reboot_scheduled) return OTA_ERR_STATE;
    if (p->hooks.close) p->hooks.close(p->hooks.context);
    if (journal(p, OTA_ABORTED, p->status.commit_id)) return fail(p, OTA_ERR_JOURNAL);
    return OTA_OK;
}
void ota_participant_note_loss(ota_participant *p, uint32_t count)
{
    p->status.rx_dropped += count;
    /* The next offset exposes a gap; a lost final block is seen at END. Queue
     * overflow notification may arrive before older queued writes, so it must
     * not prevent those contiguous writes from draining. */
}
int ota_participant_report(ota_participant *p, uint8_t source, const uint8_t *bytes,
                           size_t n, bool fd)
{
    if (source != p->lead_address) return OTA_IGNORED;
    ota_report r;
    int rc = ota_report_decode(bytes, n, fd, &r);
    if (rc) { ++p->status.rx_rejected; return rc; }
    if (!campaign(p, r.campaign_id)) return OTA_IGNORED;
    if (r.type == OTA_REPORT_REBOOT) {
        uint32_t commit = ota_read_le32(r.payload), delay = ota_read_le32(r.payload + 4);
        if (!commit || commit != p->status.commit_id || !delay || delay > 120000) return OTA_ERR_CONFLICT;
        if (p->reboot_scheduled) return OTA_OK;
        if (p->status.state != OTA_COMMITTED) return OTA_ERR_STATE;
        if (journal(p, OTA_REBOOTING, commit)) return fail(p, OTA_ERR_JOURNAL);
        if (!p->hooks.schedule_reboot || p->hooks.schedule_reboot(p->hooks.context, commit, delay))
            return fail(p, OTA_ERR_STATE);
        p->reboot_scheduled = true;
        return OTA_OK;
    }
    if (p->status.adopted || p->status.state != OTA_PASS_OPEN || r.pass_id != p->status.pass_id) return OTA_IGNORED;
    if (r.offset > p->manifest.image_size || r.payload_len > p->manifest.image_size - r.offset) {
        ++p->status.rx_rejected; return OTA_ERR_OFFSET;
    }
    if (r.offset < p->status.offset) {
        if (r.payload_len <= p->status.offset - r.offset) return OTA_IGNORED;
        ++p->status.rx_rejected; return OTA_ERR_OFFSET;
    }
    if (r.offset > p->status.offset) p->status.pass_has_hole = true;
    if (p->status.pass_has_hole) return OTA_ERR_INCOMPLETE;
    if (!p->hooks.append || p->hooks.append(p->hooks.context, r.offset, r.payload, r.payload_len))
        return fail(p, OTA_ERR_STORAGE);
    p->status.offset += r.payload_len;
    return OTA_OK;
}
int ota_participant_command(ota_participant *p, const ota_command *c)
{
    if (!p || !c) return OTA_ERR_ARGUMENT;
    switch (c->type) {
    case OTA_CMD_PREPARE: return ota_participant_prepare(p, &c->manifest, c->lead_eui, c->lead_address, c->adopt);
    case OTA_CMD_BEGIN_PASS: return ota_participant_begin_pass(p, c->manifest.campaign_id, c->pass_id, c->start_offset);
    case OTA_CMD_END_PASS: return ota_participant_end_pass(p, c->manifest.campaign_id, c->pass_id);
    case OTA_CMD_FINALIZE: return ota_participant_finalize(p, c->manifest.campaign_id);
    case OTA_CMD_COMMIT: return ota_participant_commit(p, c->manifest.campaign_id, c->commit_id);
    case OTA_CMD_ABORT: return ota_participant_abort(p, c->manifest.campaign_id);
    }
    return OTA_ERR_ARGUMENT;
}
