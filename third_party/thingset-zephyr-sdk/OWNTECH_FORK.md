# OwnTech vendored ThingSet SDK

Upstream: https://github.com/ThingSet/thingset-zephyr-sdk

Base commit: `e57447bbeb7c165e14a273242e8495343c1c6f54` (Apache-2.0).
The complete upstream source is retained here. OwnTech changes are versioned by
the enclosing Core Git commit; no PlatformIO or Zephyr package cache is patched.
`west.yml` keeps the upstream dependency manifest pin, while Core's CMake module
override selects this directory. ThingSet node C remains separately pinned at
`68c7544830df2ba23f67e31bad91e124377827a3`.

## Changes

- Client ISO-TP requests retain their transaction until response, correlated RX
  error, TX failure, or deadline. A spinlock protects the single terminal callback.
  Request payloads and server responses use separate bounded buffers, retained
  until the ISO-TP TX callback and submitting call have both completed. Only the
  shared-buffer acquiring path releases that buffer. Oversized RX is rejected
  before processing; delayed responses do not become requests.
- Report reassembly uses bounded static slots identified by instance, source and
  route, expires abandoned messages, resets on FIRST, checks lengths and sequence,
  and retains completed storage until the callback has returned. Counters expose
  expiry, malformed/out-of-order messages and overflow. The callback must copy
  immediately; flash and other blocking work belong to an application worker.
- `thingset_can_send_raw_report[_inst]` and the normal report sender share one
  serialized, bounded fragmentation path. Driver errors are propagated. A late
  driver callback after timeout keeps that transmitter unavailable until drained.
  Optional FD BRS and zero padding are explicit; the application owns logical
  payload length and integrity checks.
- `thingset_can_probe_address[_inst]` sends a bounded local-bus discovery probe.
  `thingset_can_announce_address[_inst]` republishes the local EUI/address claim
  before commands, so a newly booted receiver can establish the Lead mapping.
  `thingset_can_set_addr_claim_rx_callback[_inst]` exposes validated 8-byte claims.
  `thingset_can_get_request_source[_inst]` exposes the actual peer only during
  synchronous ThingSet command execution in the receiving thread.
- `ready` is published after CAN claims, filters and ISO-TP binding succeed.
  `THINGSET_CAN_ALLOW_ADDRESS_WRITE=n` protects the address object for OTA profiles.
  Legacy DFU fails on write/flush errors and cannot reboot an uninitialized writer;
  it must remain disabled in the OTA profile to preserve one slot owner.

## Verification and limits

Run `python tests/ota/sdk_transport_test.py` from Core. It compiles the actual
`src/can.c` and public header with clang/gcc, replacing only Zephyr/driver
boundaries. Both classical CAN and FD/BRS builds exercise delayed responses,
source/route mismatch, callback/timeout ordering, owned payload lifetime,
oversized RX, server buffer ownership, request peer scope, reassembly loss and
recovery, pool expiry, sequence wrapping, overflow, TX error/late completion,
fragmentation, padding, claims and discovery probes. It is a deterministic
transport regression test, not proof of interrupt scheduling or hardware timing.

Run `python tests/ota/sdk_fork_test.py` to verify the normalized-source SHA-256
inventory. Updating this fork requires reviewing changes and regenerating
`OWNTECH_SHA256.json`; Git pins both together. LF and CRLF normalize to LF for
text files, so Windows checkout settings do not change the result.

The first OTA profile uses one bus and route zero. Report callbacks receive only
local-route reports. Four-bit fragment sequence and two-bit message numbers
cannot detect loss of exactly sixteen fragments or every delayed-message alias;
the OTA envelope's length, CRC, campaign, pass and offset checks are mandatory.
ISO-TP has no request identifier: late responses from the same peer need
application-level correlation. SHA and EUI-64 are not sender authentication.
Physical CAN FD/BRS, bus-off recovery, scheduling under flash load and MCUboot
acceptance require board qualification.
