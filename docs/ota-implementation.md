# Legacy v1 implementation reference

> Current receiver/Lead v2 workflows are documented in [minimal-can-ota.md](minimal-can-ota.md).
> The historical instructions below describe the former same-image, padded-transfer prototype.
> They do not authorize a v2 compact campaign or its USB repair.

# Collective ThingSet/CAN OTA prototype

Implementation reference: [the preserved design report](../Idea/thingset_can_ota_implementation_report.md).
Operator commands and recovery: [ota-client.md](ota-client.md).

This implementation targets SPIN 1.2.0 / TWIST 1.4.2, one classic CAN bus at
500 kbit/s, local route zero. A single application contains the participant,
coordinator and dedicated USB CDC/SMP endpoint. The Lead role is stored in the
existing Core NVS; it is not a different firmware image. Every follower must
first receive this application individually with its existing bootloader kept.

## Build and artifact contract

`OTA` and `USB_LEAD` compile the user's current `src/main.cpp` with the same
CAN/ThingSet/OTA service and hardware profile. `pio run -e OTA -t upload`
provisions one board through its existing USB bootloader. `pio run -e USB_LEAD
-t lead_update` builds the current application and distributes the complete
firmware to the explicit fleet, including the selected Lead. Subsequent images
retain the service, so later updates do not require a separate Lead application.
Ordinary `USB` and `STLink` environments retain their existing workflows.

`pre_ota_identity.py` generates a deterministic `ota-<24 hex digits>` build ID
from the effective build settings and application/framework integration sources.
It excludes deployment choices such as USB serial, expected fleet size and the
environment name: equivalent `OTA` and `USB_LEAD` builds share an identity.
Source or build-configuration changes produce a new identity automatically.
`board_build.zephyr.bootloader.app_version` defaults to `1.0.0` and appears as
`1.0.0+0` in runtime metadata. Projects may override it consistently in both OTA
environments; no manual build-ID change is needed. Identity is recorded in the
compiled firmware and generated manifest, and verified again after reboot.

A separate configuration fingerprint invalidates the environment's CMake cache
when profiles, overlays, Kconfig, CMake inputs or build settings change. This is
needed because the installed PlatformIO Zephyr builder does not watch all those
inputs itself. Identity is also retained under `.pio/ota-generated/<environment>/`
so CMake can restore its generated header after the builder cleans its build
directory. Editing the contents of an existing `main.cpp` still uses the normal
incremental compilation path.

The existing PlatformIO `mcuboot-image` builder and its existing resolved
signature key produce `firmware.mcuboot.bin`. No bootloader is built, installed,
downloaded or modified by `lead_update`; no key is generated. The manifest
records the resolved signing key path and the image's public key digest, without
copying private key material. The currently resolved fallback is MCUboot's
published example RSA key, as in the original chain. Both diagnosed boards'
installed bootloaders were subsequently matched byte for byte to OwnTech v1.1.0
using SWD backups. The complete update workflow subsequently passed on this
two-board bench; other boards still require their own qualification.

The full padded file must fit `0x37800` bytes. Useful content (header, program,
protected/unprotected TLVs) is checked separately against `0x36000`. This is the
report's provisional swap-move bound, not a measured bootloader guarantee.
Compatibility IDs `0x01020142`, `0x00010001`, `0x00010100` describe this prototype
profile; they do not attest the bootloader binary. Change the profile together
with its manifest when qualifying another hardware/layout/bootloader tuple.

The post-sign check preserves an exact image/manifest copy in
`.pio/ota-artifacts/<environment>/`. Default PC journals live in `ota-journals/`.
Both are outside `.pio/build`: PlatformIO can remove that directory when its
configuration or downloaded library structure changes. Retain these snapshots
when an initial installation or recovery must use a particular application build.

`artifact_sha256` covers every byte of the transferred file, including padding
and trailer. `mcuboot_image_hash` covers the MCUboot image's own hash domain and
identifies the active image after boot. The active slot's modified trailer is
never used to reconstruct the transfer hash.

## Service and ownership

- The vendored ThingSet fork is based on upstream
  `e57447bbeb7c165e14a273242e8495343c1c6f54`. Its provenance and normalized source
  hashes are checked in; no dependency cache source is patched. The project
  CMake extra module overrides the downloaded base. `west.yml` is consumed by
  PlatformIO's framework build script, not by a missing `pre_west.py`.
- USB user group 64 implements the commands in the operator guide. A separate
  CDC selected by `zephyr,uart-mcumgr` carries framed SMP only. The client probes
  interfaces of the same USB serial and retains the unique compatible service.
- The exact padded image is always uploaded through `stage_data` into the
  shared `flash_img` writer. This deliberately handles a target image already
  active on the Lead: there is no "already installed" shortcut. Standard image
  upload/erase/test/confirm and OS reset groups are not exposed; an early dispatch
  guard also rejects other groups. Legacy ThingSet DFU is excluded at build time.
- The Lead adopts its closed, verified secondary slot in read-only mode. It
  neither erases it nor consumes its own DATA broadcasts. A bounded FIFO copies
  participant commands/reports before SDK buffer release; a dedicated OTA thread
  serializes writes, end-of-pass and finalization. No flash access runs in CAN RX.
- Addressed ThingSet controls live in `DFUCampaign` (`0x0D`, items/functions
  `0x100`–`0x104`). `xCommand` carries the explicit command opcode, destination
  EUI, Lead EUI/address, campaign, manifest and pass parameters. The current CAN
  request source is checked. `rStatus` is a versioned bounded byte snapshot;
  `xRelease` binds the postboot result to the persisted Lead identity.
- DATA uses ThingSet multiframe report type 1 with the report's `OTAC` envelope,
  little-endian fields, logical length and CRC-32. Routes are not repurposed.
  CRC plus length reject silent wrap-sized fragment losses. A gap stops appends
  for that pass; the coordinator repairs from the minimum accepted offset.
- The coordinator retains every selected EUI. Expected identities or an expected
  total count are required by the PC before staging. Retries, passes, no-progress
  and total duration are bounded. Flash faults are fatal, not retransmission
  hints. An explicit `commit` crosses the all-validated barrier.

## Maintenance, reboot and recovery

The safety gate starts inhibited before application setup. Before the first
erase, the service stops PWM and shield drivers, verifies inactive GPIO/output
states, persists and rereads maintenance through Core's existing NVS owner.
Direct PWM and protected GPIO APIs also consult the gate; capacitor control
uses its actual active-low polarity. Critical tasks keep their safety
supervision. A queued 1200-baud request rechecks the gate before any reset.

## Application integration

The `OTA` environment builds the chosen `src/main.cpp` with independently
started Core CAN/OTA threads. No `owntech_ota_enter_maintenance()` or
`owntech_ota_check_health()` implementation is required or invoked. `main` may
return, sleep or run a normal busy loop: the OTA profiles give the services
higher scheduling priority (OTA 7, CAN/SDK 10, main 12), enforced at configure
time. Interrupts and preemptive scheduling must remain operational.

Core checks its own hardware inhibition, CAN controller startup, storage and
boot image before confirming. Confirmation means the update service can run,
not that the application is initialized or ready to drive power. Application
UIDs, RS485 peers, SYNC and control heartbeats do not gate this confirmation.
Journal and expected-image checks still reject unresolved rollback states.

Core enables the HRTIM clock and disables all twelve outputs directly, without
depending on application PWM setup, and verifies the shield power GPIOs.
During inhibition or an OTA operation, the shared NVS owner refuses writes to
application keys with `-EBUSY`; OTA records remain writable. Whole-partition
erase through the application API is refused with `-EPERM` in OTA builds.
Applications should handle deferred writes and keep their own power permission
and rearm logic; they may observe `ota_safety_inhibited()` for that purpose.

All code still shares a privileged MCU and address space. HardFaults, disabled
interrupts/scheduling, busy higher-priority tasks, stuck static constructors or
direct access to reserved CAN/flash/power registers can prevent OTA. Surviving
those requires a separately reachable recovery bootloader and reset/watchdog
policy, which this implementation does not add.

Direct PWM and protected GPIO APIs remain subject to the service's maintenance
gate. Application-specific electrical safety, interrupt behavior and flash timing
need bench tests. Public LED calls use the service's LED owner so OTA state
indications can temporarily take priority over the application's normal pattern.

The OTA profile sets `CONFIG_CONSOLE_GETCHAR_BUFSIZE=0` on the console CDC.
Zephyr 4.0's buffered TTY overflow handler can wait for TX space from inside
the shared USB workqueue, deadlocking both CDC interfaces when console input
is not consumed. Disabling that TTY RX buffer removes the blocking overflow
path; the separate SMP CDC retains interrupt-driven reception. `console_getchar()`
still works, but a caller waiting for keyboard input now polls with 1 ms sleeps.
The supplied LED application does not call it, so this adds no idle polling task.

### Initializing a board without a CAN peer

On a boot with no campaign journal or pending maintenance, local storage, active
image hashing, CAN controller startup and the application health hook suffice to
confirm MCUboot. CAN address negotiation may still be waiting for a peer's ACK:
the USB phase is then `WAITING_CAN`, local health is true, and fleet availability
is false. Discovery, staging and PREPARE remain gated on full CAN readiness.
The SDK notifies the existing OTA queue when startup state changes; completion
of CAN setup promotes the service to `IDLE` without a periodic polling task.

Boots carrying a campaign journal retain the CAN deadline, expected hash/version/
build checks, conditional confirmation and persistent maintenance barrier.
Standalone initialization does not provide a recovery shortcut for a pending
campaign. USB `info` and `status` expose local health, CAN readiness, full service
health and runtime error independently so a real startup failure is diagnosable.

### Coexistence with real-time control outside a campaign

The supplied OTA profile keeps CAN available for requests, but does not enable
periodic ThingSet live-metric publication. FDCAN2's normal interrupts have logical
priority 2, below the HRTIM control interrupt at logical priority 0. The RS485 RX
DMA interrupt retains its separate zero-latency configuration. ThingSet CAN and
SDK work run in preemptible threads at priority 10; the kernel's shared workqueue
priority is unchanged.

The OTA worker blocks on its queue when no discovery, reconciliation or
coordinator deadline needs servicing. It publishes on handled events instead of
waking every 5 ms while idle. The 5 ms service cadence is retained during timed
campaign operations. CAN report reassembly expiry is armed only while incomplete
reports need a deadline; it has no permanent idle timer. Address claims and
responses to received requests remain part of the protocol, including at startup.

This reduces background work; it is not a measured bound on interrupt latency.
Short kernel critical sections, incoming CAN traffic and shared memory accesses
still exist. The LED owner keeps its existing 25 ms cadence, preserving the
atomic-only application LED API used from interrupt contexts. Measure control
latency and RS485 loss under the intended load before qualifying an application.
Use GPIO timing or a cycle counter to record maximum control entry delay and
execution time, plus the RS485 receive-to-transmit delay and missed-frame count.
Compare CAN disabled, enabled and quiet, then handling discovery/status traffic;
flash tests belong to the paused maintenance phase.

For the supplied TWIST 1.4.2 wiring, synchronization uses PB2/AF13 for HRTIM SCIN
and PB1/AF13 for SCOUT. The driver now selects PB2 for both TWIST 1.4.1 and 1.4.2,
leaving CAN TX on PB6. Other shield revisions retain their existing mapping.

Firmware updates assume the application has paused power conversion on the whole
fleet before the PC starts the campaign. Erase, programming, persistent role
changes and reboot are maintenance operations; this implementation does not
promise real-time control throughout them. Application maintenance callbacks must
prevent RS485 commands or another task from restarting conversion while inhibited.

NVS keys are reserved in category `0x0500`: role, maintenance, local campaign
journal and frozen fleet journal. They share the existing NVS mount and mutex.
A preflight budgets live-record space before image erase; calibration, thresholds
and metadata are not cleared. Corrupt/unreadable journal data retains inhibition.
No partial writer is resumed after a reset.

`FLASH_COMPLETE` means the final flush succeeded and the writer closed.
`VALID` also requires rereading the complete artifact and validating its structure
and hashes. Neither means the new application is executing. After the collective
reset, local health precedes confirmation. The PC and Lead check the exact frozen
identities, active MCUboot hash, version/build and health; maintenance is released
only after the fleet result is verified. Missing or wrong-image nodes remain in
the result as partial/failure. No atomic fleet update is claimed.

This prototype transfers an activation trailer. It assumes no unexpected reset
or power interruption between staging and the final reset command. Abort stops
automatic progression and retains maintenance; it does not erase/disarm that
trailer. Deferred arming with a compact artifact is intentionally outside this
prototype. USB/CAN are trusted bench interfaces, not authenticated authorities.

## Validation

### Complete two-board CAN update

Campaign `1cc90b2bd2929c1b` completed successfully on 2026-09-24 through the
PlatformIO `USB_LEAD` / `lead_update` target, with one Lead
(`1ccd6d8ab16b213e`) and one follower (`1ccd6d8a80f3af97`). The 250 ms application
image had 219444 useful bytes and 227328 transmitted bytes, build
`ota-cfa4068bc1039caefdfd16fc`, MCUboot hash
`c1c3f9690de7dce4a83fb4616c2ad5edbb8af7335a13bd40c1b6a5e9e8fa9aac`.
PlatformIO returned success in 122.097 seconds; the campaign journal spans
117.608 seconds. CAN required one pass, with no reported RX drops.

The journal records both complete `VALID` targets at `ALL_VALIDATED` before
its single `COMMIT_REQUEST`, then reboot, postboot reconciliation and `SUCCESS`.
There are no `FAILED`, `PARTIAL` or `STATUS_REJECTED` records. A fresh read-only
status afterward reports both exact identities in `SUCCESS`, on the expected
hash/build, healthy, confirmed, validated and available, with error zero. The
Lead also reports `slot_available: true`. This exercises the normal maintenance
release path; no new full flash/NVS comparison was performed.

Evidence: `ota-journals/campaign-1cc90b2bd2929c1b.jsonl` and, under
`.pio/swd-recovery-20260924/`, `complete-cycle-250ms.log`,
`complete-cycle-250ms-status.json`, and frozen `complete-cycle-250ms.bin` / `.json`.

The consecutive 1000 ms cycle, `0d8573b7b00837aa`, also returned `SUCCESS` through
the normal command `pio run -e USB_LEAD -t lead_update -j 8`. Its 234.268-second
PlatformIO duration includes a full rebuild of roughly 117 seconds. The image
has 219452 useful / 227328 transmitted bytes, build
`ota-cb61d951ce57143aa874b049`, MCUboot hash
`6af4fc8cfdc63279e641e99f337b94acf53357fd63255e3264558392c7b0fa48`.
A fresh status confirms the same two EUIs in `SUCCESS` on that exact image,
healthy, confirmed, validated and available, error zero; the Lead slot is
available. Evidence: `ota-journals/campaign-0d8573b7b00837aa.jsonl` and
`complete-cycle-1000ms.log`, `complete-cycle-1000ms-status.json`,
`complete-cycle-1000ms.bin` / `.json` in the same evidence directory.
The journal spans 117.490 seconds and records `ALL_VALIDATED` at line 262 before
`COMMIT_REQUEST` at line 263. Device event timestamps measure CAN transfer at
73.709 seconds on the Lead and 73.729 seconds on the follower.

The final `OTA` build also passed in 121.255 seconds and produces the same
`ota-cb61d951ce57143aa874b049` build identity, MCUboot hash and 219452 useful bytes
as `USB_LEAD`, verified by comparing their manifests. Its log is
`.pio/swd-recovery-20260924/final-ota-1000ms-build.log`.

Between these campaigns, no manual reset, BOOT entry, recovery utility or
ST-Link was used. The application delay was restored to 1000 ms in `src/main.cpp`
and the ignored local `src/app.ini` supplied the Lead serial, two frozen EUIs
and timeout. The second success establishes reuse of the deployed receiver,
image slots and campaign storage through the normal USB/CAN workflow, including
its automatic reboots and maintenance release. The source application is back
to its committed 1000 ms delay.

### Software checks and earlier hardware findings

Run the host suites with:

```sh
python -m unittest discover -s tests/ota -p '*test*.py' -v
pio run -e USB -e OTA -e USB_LEAD
pio run -e USB_LEAD --list-targets
```

The tests execute production C/C++ protocol, participant/coordinator, flash
adapter, CAN transport and ThingSet wire codecs against deterministic hardware
boundaries. The native USB suite compiles the actual handler with the installed
Zephyr/zcbor sources and connects it to the actual Python UART/CBOR client. It is
skipped when those external build dependencies are unavailable; the firmware CI
job runs it after installing the framework. These are software tests, not a
replacement for CAN arbitration, USB enumeration, flash latency or MCUboot tests
on real cards.

The CI builds `USB`, `OTA` and `USB_LEAD` and archives firmware, manifests,
generated configuration, devicetree and map files. Host tests also exercise the
actual runtime's admission queue, persisted target list, postboot reconciliation,
maintenance release and a subsequent campaign. They verify that a persisted
Lead preference does not prevent a board from participating in another Lead's
campaign.

The generic application workflow passed all 60 host tests and the three firmware
builds on 2026-09-24 (PlatformIO 6.2.0, Zephyr 4.0.0, GNU Arm 12.3.1).

Subsequent USB provisioning fixes passed 69 PC tests. A hardware check uploaded
all 227328 bytes with MCUmgr, rebooted into the corrected dual-CDC application,
and repeatedly selected its SMP interface after probing the unconsumed console.
The board reported build `ota-b24bcb19601a8ab5256acf65`; USB remained responsive.
This board was alone on CAN, so startup health stayed `FAILED` and the image was
not confirmed. This verifies USB transfer and service access, not fleet update
or CAN/postboot qualification. The corrected OTA build log is
`.pio/ota-usb-console-build.log`; its upload log is
`.pio/ota-usb-console-upload.log`.

Standalone initialization subsequently passed all 103 host/native tests and
both OTA builds. The tests include local confirmation without an ACK peer,
delayed controller startup, late CAN readiness with a full queue, indefinite
idle waits, coherent USB snapshots, and strict campaign postboot failures.
The new builds share `ota-e3c380391b56790f0217bde8` and MCUboot image hash
`78abe6d331679e0ae36fbd999103575c35d44e70fe3b1fd6e51b3aa492cc3216`.
The full signed file hashes differ because signing can produce different
signature bytes; each build's adjacent manifest remains authoritative.
Logs: `.pio/ota-standalone-tests.log`, `.pio/ota-standalone-build.log` and
`.pio/usb-lead-standalone-build.log`.

The first hardware attempt to install this standalone build was rejected before
accepting data: the bootloader's standard image service returned `rc=6`
(`EBADSTATE`) at offset zero. A diagnostic retry with one outstanding chunk
returned the same rejection. The old `ota-b24bcb19601a8ab5256acf65` application
was then unconfirmed and in `FAILED`; the new standalone build had not yet been
validated on that physical board. No forced image confirmation was sent.
Logs: `.pio/ota-standalone-upload.log` and `.pio/ota-standalone-window1.log`.

A second board, starting from a confirmed USB application, subsequently received
all 227328 bytes of the standalone build in 16 seconds. After reboot, the USB
client verified the exact build/hash, `local_healthy: true`,
`active_confirmed: true`, `slot_available: true`, `error: 0` and `WAITING_CAN`
while that board was alone on CAN. A second invocation returned
`ALREADY_INITIALIZED` without upload or reset. This validates standalone USB
initialization on hardware. Logs: `.pio/ota-second-board-boot-upload.log` and
`.pio/ota-second-board-recheck.log`.

With both boards powered and connected by CAN, resetting the first board let
its original application pass health checks and confirm itself normally. USB
status reported `IDLE`, confirmed and available. The second board joined CAN
from its standalone waiting state. An explicit two-EUI preflight verified both
healthy, confirmed and available (`.pio/ota-two-board-preflight.log`).

Campaign `61c6c4384740260e` then transferred and validated 227328 bytes on both
boards in one pass, with no RX drops. It failed during the Lead's collective
journal replacement, before any participant COMMIT or REBOOT (`-9`, journal
error). Both running images remained confirmed; the staged images still had
activation trailers. This exposed a missing NVS replacement-space reservation.
The corrected fleet record is 304 bytes instead of 824, and admission reserves
space for a replacement before image erase. Host tests reproduce the original
failure and exercise repeated replacements with garbage collection and existing
calibration. A later full flash dump proved the original replacement was short
by 192 bytes: 640 free versus 832 required for data and its NVS allocation entry.
See [the recovery evidence and procedures](ota-recovery.md). This earlier failed
campaign is distinct from the complete successful cycle above.

Historical linker/artifact measurements before the later runtime fixes:

| Environment | Linker flash | Linker RAM | Signed useful bytes | Transmitted bytes |
|---|---:|---:|---:|---:|
| USB | 96872 | 31616 | 97208 | 227328 |
| OTA | 218484 | 97572 | 218820 | 227328 |
| USB_LEAD | 218484 | 97572 | 218820 | 227328 |
| OTA_RECOVERY (separate maintenance image) | 105588 | 31360 | 105924 | 227328 |

After the NVS correction, all 130 host/native tests pass. Both normal OTA
environments compile to `ota-0dcaaff159634ec78f2ca713`, MCUboot hash
`ef76db03b2465e703c6441cfb028cb5bb4449fc55ca03a29b0ba97551fcd9c82`.
Logs: `.pio/ota-nvs-recovery-tests.log`, `.pio/ota-nvs-fixed-build.log`,
`.pio/usb-lead-nvs-fixed-build.log`. The separate recovery build also passes;
its final ELF contains the recovery entry point and excludes automatic image
confirmation and the ordinary user main. These build checks preceded the
hardware recovery and complete campaign validation described here.

The subsequent read-only recovery inspection of the first board's bootloader
returned `{"images": [], "splitStatus": 0}` twice. No image identity could be
verified, so recovery stopped before erase, upload or reset. An empty list does
not prove erased flash: the audited image service also omits unreadable or
unrecognized images. This was a historical stop. Later halted SWD reads found
both images intact, with matching recomputed header/TLV hashes, and the installed
bootloader exactly matched the OwnTech v1.1.0 release. A later physical bootloader
entry listed both images. The earlier empty response was not reproduced and its
cause remains unknown. See [the recorded stop and diagnostic limits](ota-recovery.md#historical-usb-stop-no-recognized-images).

The first board (`3232500B002B002D`, EUI `1ccd6d8a80f3af97`) was subsequently
repaired with its guarded `OTA_RECOVERY` image, programmed into primary slot 0
through SWD after full backups. An initial CubeProgrammer loader failure changed
no flash bytes, as verified by complete readback. Hot-plug reads while the core
was asleep had produced invalid zero/stale data; halted, repeated reads were
used as evidence. OpenOCD with reset/halt provided verified programming.

Writing the complete padded helper first prevented its normal confirmation
(`-19`). Erasing the complete primary slot and programming only its 105928-byte
aligned useful prefix plus the 16-byte magic left the `image_ok` doubleword
physically erased. That sparse retry succeeded: `RECOVERED rc=0 confirmed=1`.
The failure/retry is consistent with STM32G4's hidden flash ECC making programmed
`FF` padding unsuitable for a later confirmation write; the helper's safety and
identity guards were retained. See [the exact sparse ranges and ECC rationale](ota-recovery.md#sparse-primary-programming-and-flash-ecc).

The complete post-repair dump confirms that the bootloader and backup slot are
unchanged. All eight non-OTA NVS values (six calibrations, storage version and
Lead role) are byte-for-byte preserved; OTA keys `0501` through `0504` are removed
from the live NVS view by tombstones. The primary helper's recomputed hash matches
its TLV and `image_ok=1`. Permanent before/after dumps, analysis and option/UID
data are stored in
[`recovery-backups/2026-09-24-3232500B002B002D/`](../recovery-backups/2026-09-24-3232500B002B002D/).
This validates first-board recovery; the second board followed the separate
USB procedure below.

The second board (`3232500B00290043`, EUI `1ccd6d8ab16b213e`) initially listed
only its secondary through USB. Identical full SWD reads proved an interrupted
REVERT: 34 of 107 move operations complete, with a coherent MCUboot progress
journal. Reconstructing the displaced sectors produced the expected `78abe6d3...`
hash for both logical images. This explains its omitted primary image; it does
not establish the cause of the first board's earlier empty list. One USB reset
then let MCUboot complete the revert. Full readback confirmed both physical
image hashes, completed progress, an active confirmed original, and byte-for-byte
preservation of the bootloader and NVS.

With ST-Link physically disconnected, the second board subsequently received
the guarded helper entirely through USB using `recover_ota.py --after-revert
--apply`. The client verified the original confirmed primary and exact nonpending
campaign secondary, erased only the secondary, uploaded the helper in 12 seconds,
checked its pending hash, and requested reset. The console reported
`RECOVERED rc=0 EUI=1ccd6d8ab16b213e confirmed=1`; the helper performed its own
guarded confirmation. No flash or RAM programming through SWD was used on this
board. The last full dump preceded the helper, so the NVS preservation proof
applies to the revert, not a complete post-helper comparison.

The explicit `--after-revert` mode does not trigger a rollback and cannot bypass
empty/partial image lists or unconfirmed primaries. Its client suite has 17
passing tests. See [the second-board evidence and mode safeguards](ota-recovery.md#second-board-interrupted-revert-and-usb-repair).
Both boards subsequently passed individual normal USB initialization with
ST-Link disconnected. These checks establish initialization after the earlier
recovery work; they do not erase the distinction between the first board's SWD
repair and the second board's USB repair.

- Board `3232500B002B002D` (`1ccd6d8a80f3af97`) was initialized through the
  provision CLI with `--legacy-console`. It returned `PROVISIONED` for build
  `ota-0dcaaff159634ec78f2ca713`, MCUboot hash
  `ef76db03b2465e703c6441cfb028cb5bb4449fc55ca03a29b0ba97551fcd9c82`.
  Repeating the ordinary provision command without `--legacy-console` returned
  `ALREADY_INITIALIZED` without another upload or reset. Logs:
  `.pio/swd-recovery-20260924/standalone-usb-init-retest.log` and
  `.pio/swd-recovery-20260924/standalone-usb-init-idempotent.log`.
- Board `3232500B00290043` (`1ccd6d8ab16b213e`) passed the actual PlatformIO
  target `pio run -e USB_LEAD -t ota_init` in 46.818 seconds. It returned
  `PROVISIONED` for build `ota-d91a70a053916c82d4b7cf5c`, MCUboot hash
  `25336f5e339f0be51b3a87276a0aca8f5f3430dd215f3a9ad161c925da5c087e`.
  Log: `.pio/swd-board2-20260924/usb-lead-init-retest.log`.

In both cases the isolated board reported `WAITING_CAN`, `local_healthy: true`,
`active_confirmed: true`, `slot_available: true` and `error: 0`. The
`WAITING_FOR_PEER` result with `can_ready: false` and `available: false` is
expected before connecting a CAN peer. The `ota_init` target initializes a
follower even when invoked from `USB_LEAD`; it does not start a fleet campaign.

The PlatformIO task correction attaches `ota_init`, `lead_update` and artifact
validation to the actual `env.Alias("mcuboot-image")` node. This avoids treating
the alias name as a file dependency and lets the framework resolve the final
signed image name before validation and USB access. The host regression uses
real SCons with parallel jobs and a changed final `PROGNAME`: valid artifacts
must be signed and validated before the USB action, and corrupt artifacts must
stop that action. The successful hardware `ota_init` invocation above also
exercises the real PlatformIO task graph independently of the later CAN cycle.

The next two-board campaign, `071755d86a3704ed`, attempted a 250 ms application
image. USB staging on Lead `1ccd6d8ab16b213e` completed and validated all 227328
bytes, but about 35 ms after `START_REQUEST` the client rejected an `invalid
device event history`. No `COMMIT_REQUEST` was sent. The runtime had cleared
`staged` before the Lead participant adopted the campaign, publishing its old
campaign ID zero together with the new USB-stage event mask 463. Commit
`07b9a69` keeps the staged observation until adoption and reports coherent
campaign, size, validation and abort fields. Commit `fc688a5` records future
invalid responses as `STATUS_REJECTED` before abort, preserving the diagnostic
evidence. Logs: `ota-journals/campaign-071755d86a3704ed.jsonl` and
`.pio/swd-recovery-20260924/fleet-250ms-failure-status.json`.

The failed campaign's follower row was a cached observation. It showed the old
image and `IDLE`, but does not prove the follower's current flash or readiness;
a fresh inventory remains necessary. The Lead subsequently required another
guarded repair because abort retained maintenance and the padded secondary.
Physical bootloader entry initially listed only that secondary. One separately
requested USB reset let the 250 ms trial boot with hash `5d65217a...`,
unconfirmed and error `-18`. Physical RESET without BOOT then restored the old
500 ms image, hash `25336f5e...`, confirmed. Another physical bootloader entry
listed both slots. No ST-Link/SWD access occurred during these new attempts;
the exact interrupted-move progress was not measured by a flash dump.

The dedicated `--staged-lead-only` recovery mode now covers this pre-COMMIT
case. Its host configuration requires a complete Lead `STAGED` observation,
exactly one START, terminal failure, and no COMMIT or CAN-transfer evidence.
Only the frozen Lead can run the helper. Firmware checks the matching local
OTA1 journal with `VALID`/`ABORTED`, commit zero and USB-only event mask, plus
the mandatory 304-byte OTA2 fleet CRC, roster, Lead index, campaign token and
`PREPARING`/`FAILED` state. Its version-2 recovery marker keeps resumptions
separate from the original recovery policy; role and calibration keys remain
outside the cleanup. See [the mode safeguards and exact USB repair evidence](ota-recovery.md#failed-staging-handoff-and-lead-only-usb-repair).

The generated header SHA-256 was
`a1e4942d9c2dd7d77af7a03523307c301b235139ca6f2b0c3e3720f83663ff7d`.
The helper built in 80.372 seconds with 106300 useful bytes and MCUboot hash
`b549250871598b9c92577e4c13a17b524912d8f7d6d305f8969e8f3e9e9ec311`.
Using `--staged-lead-only --after-revert --apply`, the client transferred it
over USB in 12 seconds and requested reset only after verifying the pending
helper hash. The console reported `RECOVERED rc=0 EUI=1ccd6d8ab16b213e
confirmed=1`, with outputs inhibited. Evidence is in
`.pio/swd-board2-20260924/staged-lead-recovery-build.log`,
`staged-lead-recovery-apply.log` and `staged-lead-recovery-console.log`.
This proves the guarded helper completed; no new full flash/NVS comparison was
performed. The corrected normal Lead was subsequently installed over USB:
`.pio/swd-board2-20260924/corrected-lead-provision.log` returned `PROVISIONED`,
CAN `READY`, local/network health, active confirmation, slot availability and
error zero. Its 218916-byte useful image is build
`ota-014e997f0c2c99f4dd7fe714`, MCUboot hash
`51317c5351dcd8b3252979b68fac97008575e22b46199a48a46b2209ca603cd2`.

A fresh CAN discovery then showed follower `1ccd6d8a80f3af97` still `READY`
for campaign `071755d86a3704ed`, with offset zero, pass zero, 227328-byte image
size, erase-event mask 3, original `ef76db03...` hash, health and confirmation.
It was unavailable because PREPARE had reached it before the earlier abort.
This newer observation supersedes the cached follower IDLE row at failure.
Physical bootloader entry listed its original confirmed primary alone.
Evidence: `.pio/swd-recovery-20260924/corrected-can-preflight.json` and
`prepared-follower-bootloader.json`.

The separate `--prepared-follower-only` helper policy accepts only frozen
non-Lead EUIs, a matching local journal in `PREPARING`/`READY`, commit zero,
erase-only mask 1/3 and no fleet record, including during recovery resumption.
Its version-3 marker separates it from the other modes. The USB client requires
the original confirmed primary and no listed secondary, uploads the helper
without a separate erase request, then verifies the primary and exact pending
helper before reset. Neither an omitted secondary nor the durable READY journal
alone proves no CAN data: BEGIN_PASS does not rewrite the journal. The fresh
CAN observation and host history are part of the diagnosis. See
[the prepared-follower procedure](ota-recovery.md#explicit-recovery-of-a-prepared-follower).
The generated header hash is
`ee6317a8c0c97809d76b889a1e9147171e6f383a0dba60df8111860498247ebf`.
The version-3 helper built in 110.924 seconds with 106404 useful bytes and hash
`dd49e0921c0f5f31fb4e4377af9782a53a45b41bb14f8a8dd95ea2f645fc340e`.
Its USB transfer completed in 12 seconds, with no separate erase request;
`prepared-follower-recovery-console.log` reported
`RECOVERED rc=0 EUI=1ccd6d8a80f3af97 confirmed=1`, outputs inhibited.
The build, inspection and upload logs share the `prepared-follower-recovery-`
prefix under `.pio/swd-recovery-20260924/`. No SWD access was used.

The follower then received the same normal 500 ms build `ota-014e997f0c2c99f4dd7fe714`
and hash `51317c53...` as the corrected Lead. Its
`.pio/swd-recovery-20260924/corrected-follower-provision.log` reports
`PROVISIONED`, CAN `READY`, `IDLE`, local/network health, active confirmation,
slot availability and error zero. The next 250 ms `lead_update` attempt stopped
at preflight on a stale CAN discovery result, before any firmware staging or
flash write; see `corrected-fleet-cycle-250ms.log` and
`corrected-fleet-preflight-refusal.json` in the same evidence directory.
Commit `11241e1` fixes that stale preflight by binding discovery to a campaign
token. The PC sends the same token while polling and paging one inventory;
a different campaign token requests a fresh CAN scan. The runtime reserves
discovery while it is queued or active and publishes no target pages until
the new table is complete. Thus subsequent campaigns cannot silently reuse an
earlier campaign's inventory. The complete 250 ms cycle above passed after this
correction.

The second cycle also exposed a host-journal issue: discovery retained the
previous campaign's successful device trace, which the client re-emitted as
foreign top-level events. The client now validates those traces but keeps them
only inside observations. Recovery accepts a historical SUCCESS snapshot only
when its campaign, identity, image, state and complete trace exactly match the
frozen discovery; current activation or divergent evidence still forbids repair.
The original hardware journals are unchanged, and mixed top-level campaign IDs
remain rejected. These host-only changes do not alter the validated firmware.

After the discovery, handoff, recovery and journal changes, all 172 host/native
tests passed in 49.444 seconds:
`.pio/swd-recovery-20260924/all-tests-final-usb-can.log`.
They include coherent runtime observations during Lead adoption, preservation
of rejected host status, all three recovery policies, refusal of committed or
wrong-role repairs, compact ARM fleet fixtures, recovery across interrupted
mutations, fresh discovery across consecutive campaign tokens and strict
separation of previous successful traces from current activation evidence.

An actual build check changed the LED delay in `src/main.cpp` from 1000 to
750 ms, rebuilt, then restored 1000 ms and rebuilt incrementally with cached
CMake configuration. The edit changed both the embedded build ID and MCUboot
image hash; restoration reproduced their original values. The final `OTA` and
`USB_LEAD` artifacts have the same build ID and MCUboot image hash. The delay
change was only a build check; later hardware attempts used the separately
identified 500 ms and 250 ms artifacts above.

Both generated configurations were checked for disabled live metrics, CAN/SDK
thread priority 10 and FDCAN2 interrupt priority 2. Host regressions exercise the
real runtime returning to indefinite queue waits and waking for requests, plus
CAN reassembly deadlines that stop when the pool becomes idle. They also verify
configuration-cache invalidation and identity restoration with real CMake after
build-directory removal.

The useful image must remain below the provisional 221184-byte capacity. The
final 219452-byte 1000 ms image leaves 1732 bytes within that bound;
application code and enabled libraries share that budget with the service.
Linker RAM allocations are not measured stack high-water marks. Generated
binaries and build/test logs are local artifacts, not committed source.

The consecutive bench cycles cover one Lead and one follower. Broader qualification
still includes multiple followers, electrical behavior under load, injected
packet loss, measured power inhibition and stack high-water marks, and systematic
power-cut/partial-fleet recovery. The observed rollback and scoped repairs above
do not replace those fault campaigns. CAN FD/BRS is disabled in prototype
profiles; its codec tests are not bus qualification.
