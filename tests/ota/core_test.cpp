/* Portable deterministic state-machine tests; no Zephyr, flash or CAN hardware. */
#include "OtaAPI.h"
#include "ota_protocol.h"
#include <string.h>
#define CHECK(expr) do { if (!(expr)) return __LINE__; } while (0)

struct node {
    ota_participant p;
    uint8_t bytes[1024];
    unsigned prepared, writes, flushes, validations, reboots;
    bool bad_flush, bad_write;
};
static int prepare(void *v, const ota_manifest *, bool adopt)
{ node *n = (node *)v; if (!adopt) { ++n->prepared; memset(n->bytes, 0, sizeof(n->bytes)); } return 0; }
static int append(void *v, uint32_t offset, const uint8_t *data, size_t len)
{ node *n = (node *)v; if (n->bad_write) return -1; memcpy(n->bytes + offset, data, len); ++n->writes; return 0; }
static int flush(void *v) { node *n = (node *)v; ++n->flushes; return n->bad_flush ? -1 : 0; }
static int validate(void *v, const ota_manifest *) { ++((node *)v)->validations; return 0; }
static int journal(void *, const ota_manifest *, ota_state, uint32_t) { return 0; }
static int reboot(void *v, uint32_t, uint32_t) { ++((node *)v)->reboots; return 0; }
static void close(void *) {}
static ota_manifest manifest()
{
    ota_manifest m = {}; m.campaign_id = 42; m.image_size = 768; m.image_content_size = 600;
    m.hardware_id = 1; m.layout_id = 2; m.bootloader_id = 3; m.protocol_version = OTA_PROTOCOL_VERSION;
    memset(m.artifact_sha256, 0xaa, 32); memset(m.mcuboot_image_hash, 0xbb, 32);
    memcpy(m.version, "1.2.3", 6); memcpy(m.build_id, "build-B", 8); return m;
}
static void init_node(node *n, uint8_t id)
{
    memset(n, 0, sizeof(*n)); ota_identity identity = {};
    identity.eui[7] = id; identity.address = id; identity.usable_slot_size = 1024;
    identity.usable_image_size = 900; identity.hardware_id = 1; identity.layout_id = 2;
    identity.bootloader_id = 3; identity.protocol_version = OTA_PROTOCOL_VERSION;
    identity.active_confirmed = identity.slot_available = true;
    ota_participant_hooks h = {n, prepare, append, flush, validate, journal, reboot, close};
    ota_participant_init(&n->p, &identity, &h, false);
}
static int data(node *n, uint32_t pass, uint32_t offset, size_t len, uint8_t value = 0x5a)
{
    uint8_t bytes[OTA_MAX_PAYLOAD], frame[OTA_MAX_REPORT_SIZE]; size_t length;
    memset(bytes, value, sizeof(bytes));
    ota_report r = {OTA_REPORT_DATA, 42, pass, offset, (uint16_t)len, bytes};
    int rc = ota_report_encode(&r, frame, sizeof(frame), &length);
    return rc ? rc : ota_participant_report(&n->p, 1, frame, length, false);
}
static int protocol_test()
{
    CHECK(ota_crc32((const uint8_t *)"123456789", 9) == 0xcbf43926U);
    uint8_t payload[256] = {}, frame[320] = {}; size_t len = 0; ota_report decoded;
    ota_report r = {OTA_REPORT_DATA, 0x123456789abcdef0ULL, 257, 65536, 33, payload};
    CHECK(ota_report_encode(&r, frame, sizeof(frame), &len) == 0 && len == 65);
    CHECK(ota_report_decode(frame, len, false, &decoded) == 0 && decoded.campaign_id == r.campaign_id);
    CHECK(ota_report_decode(frame, len - 1, false, &decoded) == OTA_ERR_FORMAT);
    frame[32] ^= 1; CHECK(ota_report_decode(frame, len, false, &decoded) == OTA_ERR_CRC); frame[32] ^= 1;
    r.payload_len = 37; CHECK(!ota_report_encode(&r, frame, sizeof(frame), &len));
    size_t wire = ota_report_wire_length(len, true); CHECK(wire == 69);
    r.payload_len = 41; CHECK(!ota_report_encode(&r, frame, sizeof(frame), &len));
    wire = ota_report_wire_length(len, true); CHECK(wire == 76);
    memset(frame + len, 0, wire - len); CHECK(!ota_report_decode(frame, wire, true, &decoded));
    frame[wire - 1] = 1; CHECK(ota_report_decode(frame, wire, true, &decoded) == OTA_ERR_FORMAT);
    CHECK(ota_report_decode(frame, wire + 1, true, &decoded) == OTA_ERR_FORMAT);
    return 0;
}
static int participant_test()
{
    node n; init_node(&n, 2); ota_manifest m = manifest(); uint8_t eui[8] = {0,0,0,0,0,0,0,1};
    CHECK(!ota_participant_prepare(&n.p, &m, eui, 1, false));
    CHECK(!ota_participant_prepare(&n.p, &m, eui, 1, false) && n.prepared == 1);
    ota_manifest wrong = m; ++wrong.image_content_size;
    CHECK(ota_participant_prepare(&n.p, &wrong, eui, 1, false) == OTA_ERR_CONFLICT);
    CHECK(!ota_participant_begin_pass(&n.p, 42, 1, 0));
    CHECK(!data(&n, 1, 0, 256)); CHECK(data(&n, 1, 0, 256) == OTA_IGNORED);
    CHECK(data(&n, 1, 200, 256) == OTA_ERR_OFFSET);
    CHECK(data(&n, 1, 512, 256) == OTA_ERR_INCOMPLETE && n.p.status.offset == 256);
    CHECK(data(&n, 1, 256, 256) == OTA_ERR_INCOMPLETE);
    n.p.status.queue_depth = 1;
    CHECK(ota_participant_end_pass(&n.p, 42, 1) == OTA_AGAIN);
    n.p.status.queue_depth = 0; CHECK(!ota_participant_end_pass(&n.p, 42, 1));
    CHECK(ota_participant_finalize(&n.p, 42) == OTA_ERR_INCOMPLETE && n.flushes == 0);
    CHECK(!ota_participant_begin_pass(&n.p, 42, 2, 256));
    CHECK(!data(&n, 2, 256, 256)); CHECK(!data(&n, 2, 512, 256));
    CHECK(data(&n, 2, 0xffffff00U, 256) == OTA_ERR_OFFSET);
    CHECK(n.p.status.offset == 768 && !n.p.status.flash_complete && n.flushes == 0);
    CHECK(!ota_participant_end_pass(&n.p, 42, 2));
    n.p.status.queue_depth = 1; CHECK(ota_participant_finalize(&n.p, 42) == OTA_AGAIN);
    n.p.status.queue_depth = 0; CHECK(!ota_participant_finalize(&n.p, 42));
    CHECK(!ota_participant_finalize(&n.p, 42) && n.flushes == 1 && n.validations == 1);
    CHECK(n.p.status.flash_complete && n.p.status.validated);
    uint8_t frame[64]; size_t len;
    CHECK(!ota_report_encode_reboot(42, 77, 1000, frame, sizeof(frame), &len));
    CHECK(ota_participant_report(&n.p, 1, frame, len, false) == OTA_ERR_CONFLICT);
    CHECK(!ota_participant_commit(&n.p, 42, 77)); CHECK(!ota_participant_commit(&n.p, 42, 77));
    CHECK(!ota_participant_report(&n.p, 1, frame, len, false));
    CHECK(!ota_participant_report(&n.p, 1, frame, len, false) && n.reboots == 1);
    CHECK(ota_participant_abort(&n.p, 42) == OTA_ERR_STATE);

    init_node(&n, 2); CHECK(!ota_participant_prepare(&n.p, &m, eui, 1, false));
    CHECK(!ota_participant_begin_pass(&n.p, 42, 1, 0)); n.bad_write = true;
    CHECK(data(&n, 1, 0, 256) == OTA_ERR_STORAGE && n.p.status.state == OTA_FAILED);
    CHECK(ota_participant_begin_pass(&n.p, 42, 2, 0) == OTA_ERR_STATE);
    CHECK(n.p.status.offset == 0);
    init_node(&n, 2); CHECK(!ota_participant_prepare(&n.p, &m, eui, 1, false));
    CHECK(!ota_participant_begin_pass(&n.p, 42, 1, 0));
    CHECK(!data(&n, 1, 0, 256)); CHECK(!data(&n, 1, 256, 256)); CHECK(!data(&n, 1, 512, 256));
    CHECK(!ota_participant_end_pass(&n.p, 42, 1)); n.bad_flush = true;
    CHECK(ota_participant_finalize(&n.p, 42) == OTA_ERR_STORAGE);
    CHECK(ota_participant_finalize(&n.p, 42) == OTA_ERR_STORAGE && n.flushes == 1);
    CHECK(!n.p.status.flash_complete && !n.p.status.validated);
    init_node(&n, 1); n.p.identity.slot_available = false;
    CHECK(!ota_participant_prepare(&n.p, &m, eui, 1, true));
    CHECK(!ota_participant_finalize(&n.p, 42) && !n.prepared && !n.flushes);
    CHECK(!ota_participant_commit(&n.p, 42, 77));
    CHECK(!ota_participant_report(&n.p, 1, frame, len, false) && n.reboots == 1);
    return 0;
}
struct fleet {
    node nodes[3]; ota_manifest m; ota_target targets[3]; ota_coordinator c;
    bool lose_once, lose_always, absent, bad_identity, bad_postboot;
    unsigned tx, local_reboots, persisted;
};
static int send_command(void *v, const ota_target *t, const ota_command *cmd)
{ fleet *f = (fleet *)v; return ota_participant_command(&f->nodes[t->identity.eui[7] - 1].p, cmd); }
static int status(void *v, const ota_target *t, ota_observation *o)
{
    fleet *f = (fleet *)v; unsigned index = t->identity.eui[7] - 1;
    if (f->absent && index == 2) return OTA_AGAIN;
    o->identity = f->nodes[index].p.identity; o->status = f->nodes[index].p.status;
    if (f->bad_identity && index == 2) ++o->identity.address;
    if (f->local_reboots) {
        o->healthy = o->confirmed = true;
        memcpy(o->active_mcuboot_image_hash, f->m.mcuboot_image_hash, 32);
        memcpy(o->active_version, f->m.version, 32); memcpy(o->active_build_id, f->m.build_id, 32);
        if (f->bad_postboot && index == 2) o->active_mcuboot_image_hash[0] ^= 1;
    }
    return 0;
}
static int read_image(void *, uint32_t, uint8_t *data, size_t n) { memset(data, 0x5a, n); return 0; }
static int send_report(void *v, const uint8_t *bytes, size_t n)
{
    fleet *f = (fleet *)v; ++f->tx; ota_report r;
    if (ota_report_decode(bytes, n, false, &r)) return -1;
    for (unsigned i = 1; i < 3; ++i) {
        if (r.type == OTA_REPORT_DATA && i == 2 &&
            (f->lose_always || (f->lose_once && r.pass_id == 1 && r.offset == 256))) continue;
        (void)ota_participant_report(&f->nodes[i].p, 1, bytes, n, false);
    }
    return 0;
}
static int complete(void *) { return 0; }
static int persist(void *v, const ota_manifest *, const ota_target *, size_t count, uint32_t, ota_state)
{ fleet *f = (fleet *)v; if (count != 3) return -1; ++f->persisted; return 0; }
static int reboot_lead(void *v, uint64_t, uint32_t, uint32_t) { ++((fleet *)v)->local_reboots; return 0; }
static int start(fleet *f)
{
    f->m = manifest();
    for (unsigned i = 0; i < 3; ++i) {
        init_node(&f->nodes[i], (uint8_t)(i + 1));
        f->targets[i].identity = f->nodes[i].p.identity; f->targets[i].is_lead = i == 0;
    }
    ota_coordinator_hooks h = {f, send_command, status, read_image, send_report, complete, persist, reboot_lead};
    ota_coordinator_options options; ota_coordinator_default_options(&options);
    options.retry_interval_ms = 10; options.command_timeout_ms = 200; options.total_timeout_ms = 10000;
    return ota_coordinator_start(&f->c, &f->m, f->targets, 3, 77, &options, &h, 0);
}
static int coordinator_test()
{
    fleet f = {}; CHECK(!start(&f));
    CHECK(ota_coordinator_step(&f.c, 1) == OTA_AGAIN);
    CHECK(ota_coordinator_step(&f.c, 30) == OTA_AGAIN && f.c.current_target == 1);
    memset(&f, 0, sizeof(f)); CHECK(!start(&f)); f.lose_once = true;
    uint64_t now;
    for (now = 1; now < 9000 && f.c.phase != OTA_COORD_VALIDATE_BARRIER; ++now)
        CHECK(ota_coordinator_step(&f.c, now) == OTA_AGAIN);
    CHECK(f.c.phase == OTA_COORD_VALIDATE_BARRIER && f.c.passes == 2 && f.c.pass_start == 256);
    CHECK(f.nodes[1].flushes == 1 && f.nodes[2].flushes == 1 && !f.nodes[0].flushes);
    CHECK(f.c.target_count == 3 && !f.local_reboots && !f.nodes[1].reboots);
    for (unsigned i = 0; i < 20; ++i) CHECK(ota_coordinator_step(&f.c, now++) == OTA_AGAIN);
    CHECK(!f.local_reboots); CHECK(!ota_coordinator_commit(&f.c, 42, 77));
    int rc = OTA_AGAIN;
    while (now < 9000 && rc == OTA_AGAIN) rc = ota_coordinator_step(&f.c, now++);
    CHECK(rc == OTA_OK && f.c.state == OTA_SUCCEEDED && f.local_reboots == 1);
    CHECK(f.nodes[1].reboots == 1 && f.nodes[2].reboots == 1);

    memset(&f, 0, sizeof(f)); CHECK(!start(&f)); f.lose_always = true;
    rc = OTA_AGAIN; for (now = 1; now < 9000 && rc == OTA_AGAIN; ++now) rc = ota_coordinator_step(&f.c, now);
    CHECK(rc == OTA_ERR_NO_PROGRESS && f.c.target_count == 3 && !f.local_reboots);
    memset(&f, 0, sizeof(f)); CHECK(!start(&f)); f.absent = true;
    rc = OTA_AGAIN; for (now = 1; now < 9000 && rc == OTA_AGAIN; ++now) rc = ota_coordinator_step(&f.c, now);
    CHECK(rc == OTA_ERR_TIMEOUT && f.c.target_count == 3 && !f.tx);
    memset(&f, 0, sizeof(f)); CHECK(!start(&f)); f.bad_identity = true;
    rc = OTA_AGAIN; for (now = 1; now < 9000 && rc == OTA_AGAIN; ++now) rc = ota_coordinator_step(&f.c, now);
    CHECK(rc == OTA_ERR_IDENTITY && !f.local_reboots);
    memset(&f, 0, sizeof(f)); CHECK(!start(&f)); f.bad_postboot = true;
    for (now = 1; now < 9000 && f.c.phase != OTA_COORD_VALIDATE_BARRIER; ++now)
        CHECK(ota_coordinator_step(&f.c, now) == OTA_AGAIN);
    CHECK(!ota_coordinator_commit(&f.c, 42, 77)); rc = OTA_AGAIN;
    for (; now < 9000 && rc == OTA_AGAIN; ++now) rc = ota_coordinator_step(&f.c, now);
    CHECK(rc == OTA_ERR_TIMEOUT && f.c.target_count == 3 && f.c.state != OTA_SUCCEEDED);
    return 0;
}
extern "C" int ota_test_run()
{
    int rc = protocol_test(); if (rc) return rc;
    rc = participant_test(); if (rc) return rc;
    return coordinator_test();
}
#ifndef OWNTECH_FREESTANDING_TEST
#include <stdio.h>
int main() { int rc = ota_test_run(); if (rc) fprintf(stderr, "core_test.cpp:%d\n", rc); return rc ? 1 : 0; }
#endif
