# Recovering an uncommitted OTA campaign

The default procedure applies to an explicitly identified campaign that reached
`ALL_VALIDATED` but failed before any participant committed or rebooted. A
separate explicit `--staged-lead-only` mode repairs a fully staged Lead after
a failed START, with no COMMIT or CAN-transfer progress. The mutually exclusive
`--prepared-follower-only` mode covers a follower left prepared by that early
failure. None of these modes accepts an unverified trial image. Normal operation
continues to use `OTA` for initialization and `USB_LEAD` for fleet updates.

## Observed failure

On 2026-09-24, campaign `61c6c4384740260e` transferred and validated all 227328
bytes on two CAN-connected boards in one pass, with no reported RX drops. The
Lead then reported `OTA_ERR_JOURNAL` (`-9`) before participant COMMIT or REBOOT.
Both applications still responded, with their original active images confirmed.

The original 824-byte fleet record could be written once but its replacement
exhausted NVS: garbage collection preserves the previous value until the
new value is durable. The first board's flash dump proves the capacity problem:
the compacted active sector had 640 bytes available, while the replacement
required 824 data bytes plus an 8-byte allocation-table entry (ATE), or 832
bytes. It was short by **192 bytes**. The preceding `VALID` journal remained
intact, with a valid CRC. A host regression reproduces this failure with other
metadata present. The corrected record is 304 bytes; admission also reserves
replacement space, GC overhead, and the implicit storage-version entry before
erasing an image slot. Existing OTA1 fleet records remain readable. These
semantics follow [Zephyr NVS](https://docs.zephyrproject.org/4.0.0/services/storage/nvs/nvs.html).

The affected application's API cannot resume this failed COMMIT. Its abort
operation neither removes the padded activation trailer nor clears maintenance.
For a board still in that state, do not reset normally while the staged image
remains pending.

## Why physical bootloader entry is required

In the inspected OwnTech bootloader v1.1.0 source, the physical user button is
checked **before** `boot_go()`. Hold BOOT, press and release RESET, keep BOOT
held for about a second, then release it. This stops in the USB image-management
server without swapping the pending image. Software boot-mode entry is checked
after `boot_go()`, so a 1200-baud reset is unsuitable for this procedure.

The bootloader configuration enables `CONFIG_MCUMGR_GRP_IMG_ALLOW_ERASE_PENDING`.
The repair client still checks the actual USB receiver and image slots before
performing any erase. If the installed bootloader differs or any USB check fails,
stop and preserve the diagnostic log; do not force a confirmation or erase the
primary slot through this USB procedure.

References: [OwnTech v1.1.0 bootloader](https://github.com/owntech-foundation/bootloader/tree/v1.1.0)
and the [OwnTech bootloader guide](https://docs.owntech.org/latest/bootloader/docs/getting_started/).

<a id="current-hardware-stop-no-recognized-images"></a>

## Historical USB stop: no recognized images

The first board (`3232500B002B002D`, EUI `1ccd6d8a80f3af97`) entered its USB
bootloader on 2026-09-24. At 16:35 UTC, the read-only recovery inspection returned
`{"images": [], "splitStatus": 0}`. A second fresh read on the same USB serial
returned the same state. That inspection stopped before any erase, upload or
reset; the recovery utility had not then been installed. Additional read-only OS parameters,
OS information and image-slot-information requests returned unsupported (`rc=8`).
Local logs:
`.pio/ota-recovery-61c6c4384740260e-1ccd6d8a80f3af97.jsonl` and
`.pio/ota-first-board-empty-images-diagnostics.log`.

An empty list does **not** establish that flash is erased or the board is
irreparably damaged. In the audited Zephyr image-management implementation,
`img_mgmt_state_encode_slot()` omits a slot whenever `img_mgmt_read_info()` fails,
including flash-read, header or TLV errors. The overall request can still
succeed. This response neither identifies the installed bootloader version nor
reports an RSA-signature validation result. See the
[state encoder](https://github.com/zephyrproject-rtos/zephyr/blob/v3.5.0/subsys/mgmt/mcumgr/grp/img_mgmt/src/img_mgmt_state.c)
and [image reader](https://github.com/zephyrproject-rtos/zephyr/blob/v3.5.0/subsys/mgmt/mcumgr/grp/img_mgmt/src/img_mgmt.c).

Those responses prevented the USB recovery preflight from checking the required
image hashes. No guard was bypassed. After an ST-Link became available, halted
SWD reads established the actual contents. A later physical BOOT + RESET entry
returned both images through USB. The earlier empty-list response was not
reproduced and its cause remains unknown; neither the NVS capacity failure nor
the later debugger read failures establish its cause.

## First-board SWD diagnosis and successful repair

Before any programming, two complete 512 KiB flash reads were compared and
found identical. Their SHA-256 is
`2a14ba31d9ecfaabed86a2b22e0c79f90d227f18af9182f0dc1332b84f139f73`.
The permanent evidence directory is
[`recovery-backups/2026-09-24-3232500B002B002D/`](../recovery-backups/2026-09-24-3232500B002B002D/):
`flash-before-repair.bin`, `flash-after-repair.bin`, `before-analysis.json`,
`after-analysis.json`, and `options-and-uid.log`. These are local board backups;
retain them outside disposable build directories.

STM32CubeProgrammer hot-plug reads while the core was asleep produced invalid
zero/stale values. They were rejected as evidence. Halt the core and compare
two full reads before interpreting flash or option bytes. The validated reads
showed 512 KiB flash, `OPTR=0xFFEFF8AA` (`DBANK=1`, `BFB2=0`, RDP level 0),
`MEMRMP=0`, and UID words `002B002D 3232500B 2037304B`. The derived ThingSet EUI
matched `1ccd6d8a80f3af97`.

The installed bootloader matched the OwnTech v1.1.0 release byte for byte:
65344 release bytes followed by 192 erased bytes. Both application images had
valid headers/TLVs and matching recomputed image hashes. At this snapshot,
slot 0 held the unconfirmed trial image (`78abe6d3...`), with a completed TEST
swap trailer; slot 1 retained the original image (`de4c2d2a...`). The NVS local
journal remained `VALID`, commit ID zero, and the frozen fleet remained `VALID`.
The flash contents therefore supported a scoped repair without replacing the
bootloader or losing the retained original image.

The verified partition ranges are half-open:

| Region | Address range |
|---|---|
| Bootloader | `[0x08000000, 0x08010000)` |
| Primary slot 0 | `[0x08010000, 0x08047800)` |
| Backup slot 1 | `[0x08047800, 0x0807f000)` |
| NVS, two 2048-byte pages | `[0x0807f000, 0x08080000)` |

### Sparse primary programming and flash ECC

This was a repair of the identified first board with its verified campaign-bound
helper, not the normal USB provisioning procedure below. All flash and option
data were backed up first. The helper's EUI, campaign, original-backup hash,
staged hash and journal guards remained enabled. Only primary slot 0 was erased
and programmed; slot 1, NVS, the bootloader and option bytes were excluded.

An initial STM32CubeProgrammer write attempt failed in flash-loader
initialization. A subsequent full read proved that it had changed no flash
bytes. OpenOCD with an explicit reset followed by halt successfully erased,
programmed and verified the primary slot. Reset/halt before the programming
sequence was necessary for the reliable bench procedure; attaching to the
sleeping application was not sufficient.

Programming the complete padded helper file first caused its own confirmation
to fail with `CONFIRM_FAILED rc=-19`. The helper kept outputs inhibited and
retained its recovery marker. The observed failure and successful sparse retry
are consistent with programming `FF` padding consuming the hidden ECC bits of
the MCUboot `image_ok` doubleword. A readback of `FF` alone cannot establish
that this doubleword is still physically erased. STM32G4 programming covers
64 data bits plus 8 ECC bits; an already programmed doubleword cannot generally
be programmed again. See [ST RM0440, flash main-memory programming sequences](https://www2.st.com/resource/en/reference_manual/dm00355726-stm32g4-series-advanced-armbased-32bit-mcus-stmicroelectronics.pdf).

The successful retry reset and halted the core, erased **all of slot 0**, then
programmed only these two ranges from the verified helper artifact:

| Content | Address | Bytes |
|---|---|---:|
| Header, image and TLVs, padded to an 8-byte boundary | `0x08010000` | 105928 |
| MCUboot magic | `0x080477f0` | 16 |

The useful content was 105924 bytes; the first write included four alignment
bytes. The `image_ok` doubleword at `[0x080477e8, 0x080477f0)` was left physically
erased, as was all other padding. Do not program the full padded file through
SWD for this procedure. These lengths belong only to this exact helper artifact
(SHA-256 `6e094cf259a2f7589540e17d3c8012f539f37df97d7e75b6db6c532879f34c8b`);
another image requires fresh header/TLV, layout and guard verification.

Before reset, full readback still matched every byte of the padded artifact,
and the bootloader, backup slot and NVS including the existing recovery marker
were unchanged. The helper resumed through that marker, performed its normal
safety and identity checks, confirmed itself, then removed only OTA keys.
No external forced-confirmation command or confirmation bypass was used.

### Verified result

The console reported `OTA_RECOVERY RECOVERED rc=0`, the expected EUI and
`confirmed=1`, with outputs inhibited. The post-repair dump proves:

- Bootloader and complete slot 1 are unchanged byte for byte.
- Slot 0 equals the verified helper artifact except for the intentional
  `image_ok` byte at `0x080477e8`, changed from `FF` to `01`. Its recomputed
  MCUboot image hash is
  `b31ad7a0211127ef0fec521f1200cc4550611a526caf13ff1189b5e940dbc524`, matching its TLV.
- All eight non-OTA NVS values are unchanged byte for byte: storage version
  `0100`, six calibrations (`0211`, `0212`, `0219`, `0226`, `0227`, `0228`) and
  persisted Lead role `0500`.
- Keys `0501` through `0504` are absent from the resolved live NVS view. Their
  latest ATEs are zero-length tombstones; historical data remains in flash.
  Raw NVS changes are limited to the recovery marker and five appended ATEs.

The post-repair dump SHA-256 is
`f9ea7578f34dd5cccd5747000ec1a2b0b2c095df03fdf6c2939b3b5eca37b1ed`.
The detailed comparison is preserved in the permanent `after-analysis.json`.
Local execution logs are under `.pio/swd-recovery-20260924/`, including
`openocd-sparse-recovery-reset-halt.log` and `sparse-recovery-console.log`.
This establishes successful guarded recovery of the first board. The second
board's separate USB recovery is recorded below; subsequent complete fleet
validation is summarized in [Validation status](#validation-status).

## Second-board interrupted revert and USB repair

The second board (`3232500B00290043`, EUI `1ccd6d8ab16b213e`) initially returned
only slot 1 through the bootloader image service. Two identical halted SWD
backups showed an interrupted **REVERT**, with 34 of 107 primary-sector moves
complete and no swap half-operation complete. The installed bootloader again
matched OwnTech v1.1.0 exactly. Its primary trailer had valid magic,
`swap_info=4`, `copy_done=FF` and `image_ok=1`; the secondary trailer was erased.

This establishes why slot 0 was omitted on this board. The move algorithm
copies primary sectors upward by one 2048-byte sector, starting at the end.
During that process its normal header/TLV offsets need not describe a complete
image. MCUboot reconstructs the locations from its durable progress entries;
the Zephyr image-list service reads the ordinary offsets. Offline reconstruction
of both logical images produced the expected `78abe6d3...` hash. The local OTA1
journal and maintenance marker had valid CRCs, with `VALID`, commit zero and the
expected campaign. Holding BOOT stops before `boot_go()`, so it also stops an
already interrupted swap from resuming. See the
[v1.1.0 move algorithm](https://github.com/owntech-foundation/bootloader/blob/v1.1.0/boot/bootutil/src/swap_move.c).
This does not retroactively establish the cause of the first board's earlier
empty-list response.

After these checks, one explicit USB OS-reset request let MCUboot finish the
recorded revert. A fresh full read verified 107 completed moves and 214 completed
swap half-operations, `copy_done=1`, `image_ok=1`, and intact physical images in
both slots with the expected hash. Bootloader and all 4096 NVS bytes were
unchanged. The restored application reported its original hash, local health
and active confirmation; it remained in campaign failure while isolated from
CAN. Evidence is in `.pio/swd-board2-20260924/after-usb-revert-analysis.json`,
`flash-first-read.bin`, `flash-after-usb-revert.bin` and
`after-revert-app-info.json`. No flash or RAM programming through SWD was used
on this board; the debugger was used for diagnosis and backups.

The ST-Link was physically disconnected before installing the repair image.
After another physical BOOT + RESET entry, the USB client used
`--after-revert --apply`: the original primary was confirmed, and the exact
campaign secondary was nonpending. It erased only that secondary, verified the
unchanged primary, uploaded the helper in 12 seconds, verified its exact hash
and pending state, then requested one USB reset. The helper reported
`OTA_RECOVERY RECOVERED rc=0 EUI=1ccd6d8ab16b213e confirmed=1`, with outputs
inhibited. It confirmed itself through its guarded recovery transaction; no
external confirmation was forced.

The USB journal is
`.pio/ota-recovery-61c6c4384740260e-1ccd6d8ab16b213e.jsonl`; upload and console
evidence are `.pio/swd-board2-20260924/usb-recovery-apply.log` and
`usb-recovery-console.log`. The last complete second-board dump was taken
**after the revert and before the helper**. Its NVS preservation comparison must
not be presented as a post-helper flash comparison. Helper completion is
established by the USB transaction and explicit recovery-success console report.

## Failed staging handoff and Lead-only USB repair

Campaign `071755d86a3704ed` attempted a 250 ms application image after the
individual USB initialization checks. The Lead (`3232500B00290043`, EUI
`1ccd6d8ab16b213e`) completely staged and validated all 227328 bytes. About
35 ms after `START_REQUEST`, the client stopped with `invalid device event
history`, before any `COMMIT_REQUEST`. The old runtime cleared its `staged`
flag before the local participant adopted the new campaign. Publication then
combined that participant's previous campaign ID zero with the new campaign's
event mask 463. The host correctly rejected the inconsistent observation.

Commit `07b9a69` retains the staged Lead observation until participant adoption,
including coherent campaign, size and validation fields. Commit `fc688a5`
adds `STATUS_REJECTED` evidence to the host journal before aborting, so future
invalid responses remain available for diagnosis. The original failure is in
`ota-journals/campaign-071755d86a3704ed.jsonl` and
`.pio/swd-recovery-20260924/fleet-250ms-failure-status.json`. That status's
follower row was cached; it does not independently prove the follower's flash
or current availability. A fresh inventory is required before another campaign.

Physical BOOT + RESET again exposed only the secondary image, so USB repair
preflight initially refused it. A separately requested USB OS reset let MCUboot
boot the 250 ms trial, hash `5d65217a...`; the application refused confirmation
and reported error `-18`. A subsequent physical RESET without BOOT restored the
old 500 ms image, hash `25336f5e...`, with active confirmation. Another physical
bootloader entry then exposed both image slots. These USB observations establish
trial boot and rollback; no SWD dump was taken during this attempt to establish
the precise interrupted-move offset. Logs are
`.pio/swd-board2-20260924/staged-abort-bootloader.json`,
`staged-abort-after-resume-status.json`, `staged-abort-after-revert-status.json`
and `staged-abort-after-revert-bootloader.json`.

The Lead-only configuration generated from the failed campaign has header
SHA-256 `a1e4942d9c2dd7d77af7a03523307c301b235139ca6f2b0c3e3720f83663ff7d`.
Its dedicated helper built in 80.372 seconds, with 106300 useful bytes and
MCUboot image hash
`b549250871598b9c92577e4c13a17b524912d8f7d6d305f8969e8f3e9e9ec311`.
The explicit `--staged-lead-only --after-revert --apply` USB transaction uploaded
the helper in 12 seconds. Its console reported
`OTA_RECOVERY RECOVERED rc=0 EUI=1ccd6d8ab16b213e confirmed=1`, with outputs
inhibited. All operations in this attempt used USB and the physical buttons;
no ST-Link/SWD access or externally forced confirmation was used.

Evidence: `.pio/swd-board2-20260924/staged-lead-recovery-build.log`,
`staged-lead-recovery-inspect.log`, `staged-lead-recovery-apply.log`,
`staged-lead-recovery-console.log` and
`.pio/ota-recovery-071755d86a3704ed-1ccd6d8ab16b213e.jsonl`.
The console proves helper completion, not a new byte-for-byte flash/NVS
comparison. Corrected normal initialization and the subsequent complete CAN
cycle are recorded below and in [Validation status](#validation-status).

## Prepared-follower USB repair and normal initialization

The corrected 500 ms normal image was installed on `3232500B00290043` through
USB. It returned `PROVISIONED`, CAN status `READY`, local/network health,
active confirmation, an available slot and error zero. Its build is
`ota-014e997f0c2c99f4dd7fe714`, MCUboot hash
`51317c5351dcd8b3252979b68fac97008575e22b46199a48a46b2209ca603cd2`, with 218916
useful bytes. The provisioning operation initializes the follower role; the
subsequent fresh discovery identified it as the selected Lead. Evidence:
`.pio/swd-board2-20260924/corrected-lead-provision.log` and
`.pio/swd-recovery-20260924/corrected-can-preflight.json`.

That fresh CAN discovery identified the other board (`3232500B002B002D`, EUI
`1ccd6d8a80f3af97`) in `READY` for campaign `071755d86a3704ed`, image size
227328, offset zero, pass zero and erase-event mask 3. Its original `ef76db03...`
image was healthy and confirmed, but `available` was false. Thus PREPARE had
reached this follower before the client's earlier abort, despite the cached
IDLE row in the failure snapshot. The current discovery must take precedence
over that cached observation.

After physical bootloader entry, the read-only
`.pio/swd-recovery-20260924/prepared-follower-bootloader.json` listed only slot 0,
with that original hash active and confirmed. These are the pre-repair facts;
an omitted secondary is not proof that every byte of its flash is erased.
The durable `READY` journal alone also does not prove no CAN data was written:
BEGIN_PASS changes RAM state without rewriting that journal. The fresh
READY/offset/pass/event observation and the host campaign evidence are
therefore material to selecting the prepared-follower procedure.

The prepared-follower configuration has header SHA-256
`ee6317a8c0c97809d76b889a1e9147171e6f383a0dba60df8111860498247ebf`.
The version-3 helper built successfully in 110.924 seconds after a compiler
temporary-file error on the first build, before any device programming. It has
106404 useful bytes and MCUboot hash
`dd49e0921c0f5f31fb4e4377af9782a53a45b41bb14f8a8dd95ea2f645fc340e`.
USB installation took 12 seconds with no separate erase request; its console
reported `RECOVERED rc=0 EUI=1ccd6d8a80f3af97 confirmed=1`, outputs inhibited.
No SWD access was used. Evidence under `.pio/swd-recovery-20260924/`:
`prepared-follower-recovery-build-retry.log`, `prepared-follower-recovery-inspect.log`,
`prepared-follower-recovery-apply.log` and `prepared-follower-recovery-console.log`.

The follower then received the same corrected normal 500 ms image as the Lead:
build `ota-014e997f0c2c99f4dd7fe714`, hash `51317c53...`.
`corrected-follower-provision.log` reports `PROVISIONED`, CAN `READY`, `IDLE`,
local/network health, active confirmation, an available slot and error zero.
Both boards therefore passed normal USB initialization after their scoped
repairs. The next 250 ms fleet attempt stopped at preflight because of a stale
CAN discovery result, before any firmware staging or flash write. That historical
preflight refusal is recorded in
`corrected-fleet-cycle-250ms.log` and `corrected-fleet-preflight-refusal.json`.
Commit `11241e1` subsequently bound inventory refresh to a campaign token:
repeated requests share one scan, a new campaign starts a fresh scan, and its
rows are exposed only after publication completes. The later complete cycle
passed with this correction.

## Prepare a repair image

Keep the original `ota-journals/campaign-*.jsonl` and the current board status.
Generate a configuration from that journal, then build the dedicated utility:

```powershell
python owntech/tools/prepare_ota_recovery.py --journal ota-journals/campaign-61c6c4384740260e.jsonl
pio run -e OTA_RECOVERY
```

The configuration is local to `.pio/ota-recovery-config/`. It binds the utility
to the frozen campaign, Lead EUI, allowed board EUIs, original active-image
hashes, and staged-image hash. It is not committed to the repository. The build
checks that its JSON metadata and header agree. The signed image and manifest
are preserved in `.pio/ota-artifacts/OTA_RECOVERY/`.

`OTA_RECOVERY` is build-only: ordinary Upload is rejected. It runs its own
maintenance entry point, with CAN and the user's `main.cpp` excluded. It does
not run a power-control task. Normal `OTA`, `USB_LEAD`, and `USB` builds do not
contain this repair program.

### Explicit recovery of a staged Lead before COMMIT

Use this separate mode only when the campaign journal proves one completed
Lead stage followed by exactly one START, ends in `FAILED`, and contains no
COMMIT, CAN-transfer, collective-validation, reboot or success evidence:

```powershell
python owntech/tools/prepare_ota_recovery.py --journal ota-journals/campaign-071755d86a3704ed.jsonl --staged-lead-only
pio run -e OTA_RECOVERY
```

The generated configuration retains the complete frozen roster for comparison,
but authorizes repair only of its Lead. Both the PC client and firmware reject
a follower identity. The helper requires the matching local OTA1 journal,
CRC, campaign, staged hash, image size and Lead EUI, with commit ID zero and
state `VALID` (6) or `ABORTED` (10). The USB-stage event mask must be exactly
207 or 463: `VERIFY_END` may have been recorded after the last journal write,
but no CAN-transfer, collective-validation or reboot event is permitted.
The old abort status can be a RAM-only change, so the durable journal is not
assumed to have changed from `VALID` to `ABORTED`.

An OTA2 fleet record is mandatory before the first mutation: exactly 304 bytes,
valid CRC, matching campaign/hash/size and complete distinct roster, correct
Lead index, and state `PREPARING` (1) or `FAILED` (9). Its coordinator token must
equal the low 32 campaign bits, or 1 if those bits are zero. That token is
allocated at START and does not imply a participant COMMIT. The recovery marker
uses version 2 for this mode; a marker from the default mode cannot authorize
its cleanup or vice versa. Confirmation still follows the verified marker and
local safety checks, and only keys `0501` through `0504` are removed.

Pass `--staged-lead-only` to both inspection and application. If a rollback
has already completed and both slots meet the checks below, also pass
`--after-revert`, as in the verified Lead repair:

```powershell
python owntech/tools/recover_ota.py --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial 3232500B00290043 --identity 1ccd6d8ab16b213e --staged-lead-only --after-revert --inspect
```

After the exact inspection succeeds, replace `--inspect` with `--apply` and
select the existing MCUmgr executable. This mode does not broaden the slot
checks, authorize a partial follower repair, or resume an interrupted swap.

### Explicit recovery of a prepared follower

This mode uses the same failed-START host evidence as the staged-Lead mode,
but targets only frozen non-Lead identities. Preserve a fresh observation of
the selected follower's preparation state before entering the bootloader.
Generate a separate configuration and rebuild the helper:

```powershell
python owntech/tools/prepare_ota_recovery.py --journal ota-journals/campaign-071755d86a3704ed.jsonl --prepared-follower-only
pio run -e OTA_RECOVERY
python owntech/tools/recover_ota.py --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial 3232500B002B002D --identity 1ccd6d8a80f3af97 --prepared-follower-only --inspect
```

After successful inspection, replace `--inspect` with `--apply`, selecting the
existing MCUmgr executable. `--prepared-follower-only` cannot be combined with
`--staged-lead-only` or `--after-revert`. It requires the original confirmed,
active primary and **no listed secondary**. It issues no separate secondary
erase request: it uploads the helper, then verifies the unchanged primary and
exact pending helper before requesting one reset. Empty image lists, a missing
primary, any listed secondary, and selection of the Lead are refused.

The firmware requires the campaign-bound local OTA1 journal and valid CRC,
matching Lead EUI, image hash and size, commit zero, state `PREPARING` (1) or
`READY` (2), and erase-only event mask 1 or 3. A fleet record must be absent on
every attempt, including marker-based resumptions. The local EUI must match a
frozen follower and the retained original image must match its original hash.
Its version-3 recovery marker cannot be reused by another recovery policy.
The existing sequence remains: verified marker, local safety checks,
self-confirmation, removal of only OTA recovery keys, marker removed last.
Host and firmware guards complement the fresh diagnostic evidence; they do not
turn an absent image-list entry into a byte-for-byte erasure measurement.

## Per-board safeguards

Use only the expected USB serial and one physically selected board at a time.
After entering the bootloader with the physical buttons, inspect first using
`recover_ota.py` (see `--help` for arguments). Inspection makes no changes.

```powershell
python owntech/tools/recover_ota.py --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial <USB-serial> --identity <board-EUI> --inspect
```

After reviewing that exact slot inspection, replace `--inspect` with `--apply`
and add `--mcumgr owntech/third_party/mcumgr.exe` on Windows. Other platforms
must select their existing MCUmgr executable. If an erase or upload stops, do
not repeat blindly: the slot state has changed and the original preflight will
correctly refuse a fresh attempt.

### A board whose revert has already completed

The default inspection requires the staged secondary to be pending. If a
separately verified revert has completed, add `--after-revert` to both inspection
and application commands:

```powershell
python owntech/tools/recover_ota.py --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest .pio/ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial <USB-serial> --identity <board-EUI> --after-revert --inspect
```

After a successful inspection, use the same arguments with `--apply` instead of
`--inspect`, adding `--mcumgr owntech/third_party/mcumgr.exe` on Windows.
`--after-revert` does **not** initiate or resume a revert. It changes only the
initial secondary-state requirement from pending to nonpending. The exact
original primary must still be active, confirmed and nonpending; the exact
campaign secondary must be inactive and unconfirmed. All campaign, identity,
artifact and subsequent upload guards remain enforced. After uploading the
helper, the client always requires that new helper to be pending before reset.
The recovery-client suite covers these checks, including their combination
with the separate Lead-only mode.

An empty or partial image list fails the default and staged-Lead modes. Only
the separate prepared-follower policy accepts the original primary alone.
Do not select `--after-revert`
to bypass missing images or to infer that a move has completed. Diagnose the
interrupted state before any separate decision to resume it; the second-board
case above used complete backups and verified progress/content evidence.

The default and staged-Lead apply operations establish the following before erasure:

1. The application OTA command is explicitly unsupported and the standard
   image service responds on the selected USB interface.
2. Slot 0 is active and confirmed, with the original hash recorded for that
   board. Slot 1 contains exactly the staged campaign hash and is inactive and
   unconfirmed: pending by default, nonpending only with `--after-revert`.
3. The repair artifact matches its signed-image manifest and generated guards.

Only slot 1 is erased. The client rereads the slots, verifies that slot 0 is
unchanged and slot 1 is absent, uploads the repair image with bounded progress,
and verifies its exact hash before requesting reset. It never confirms an image
through the bootloader.

The repair program checks the board identity, retained original image and
matching local journal (`VALID`, commit ID zero) before modifying OTA metadata.
The explicit staged-Lead and prepared-follower modes use their own guards above.
It persists and verifies a recovery marker, rechecks local safety, then confirms
its own image before removing the failed campaign's keys. It removes the marker
last, allowing an interrupted repair to resume. Role, calibration and other
application metadata are preserved.

After an explicit repair-success report, install the corrected normal `OTA`
artifact through the bootloader, entered again with physical BOOT + RESET.
The repair utility deliberately disables software reset via 1200 baud.
Verify its active hash, local health, confirmation and available slot.
In the default multi-board repair, repeat for
each affected board; the two scoped modes authorize only their selected role.
A new fleet campaign should begin only once every board is ready and the
expected inventory has been freshly verified.

## Validation status

New campaign journals retain earlier successful device traces inside snapshots
without re-emitting their events under foreign campaign IDs. A previous SUCCESS
snapshot is accepted as history only if its identity, campaign, image, state and
trace exactly match the frozen inventory. Current activation, explicit reboot
phases and divergent traces still forbid recovery. Existing journals with mixed
top-level campaign IDs remain rejected and are not rewritten automatically.

The CAN transfer and failures above were observed on hardware. All 172
host/native tests pass in 49.444 seconds in
`.pio/swd-recovery-20260924/all-tests-final-usb-can.log`, including the NVS
replacement regression, corrected repeated writes, Lead staging handoff,
rejected-status evidence, fresh inventory per campaign, all three recovery
policies and interrupted repair transactions. Recovery tests include ARM fixtures with the installed image's
short-enum layout and the compact OTA2 fleet record.
The first board's scoped SWD repair passed on hardware, including normal helper
confirmation and a complete before/after flash comparison. The second board's
interrupted revert resumed through USB, with bootloader/NVS preservation proved
before installing the helper. Its subsequent guarded USB helper installation
and self-confirmed recovery succeeded with ST-Link disconnected; no complete
post-helper second-board dump is claimed. The later version-2 Lead repair and
version-3 prepared-follower repair both succeeded over USB, followed by normal
initialization of the same corrected image on both boards. Each further board
must pass its own actual bootloader slot checks.

Campaign `1cc90b2bd2929c1b` subsequently completed through reboot, reconciliation
and maintenance release on the two-board bench. PlatformIO `lead_update` returned
`SUCCESS` in 122.097 seconds, with one CAN pass and no reported RX drops.
The 250 ms image is build `ota-cfa4068bc1039caefdfd16fc`, MCUboot hash
`c1c3f9690de7dce4a83fb4616c2ad5edbb8af7335a13bd40c1b6a5e9e8fa9aac`,
219444 useful / 227328 transmitted bytes. A fresh read afterward confirmed
both frozen EUIs on that image in `SUCCESS`, healthy, confirmed, validated and
available, error zero, with the Lead slot available. This validates the normal
post-repair update path; it is not a new raw NVS-preservation comparison.

See [complete-cycle evidence and remaining qualification](ota-implementation.md#complete-two-board-can-update),
`ota-journals/campaign-1cc90b2bd2929c1b.jsonl` and
`.pio/swd-recovery-20260924/complete-cycle-250ms-status.json`.
The consecutive 1000 ms campaign, `0d8573b7b00837aa`, also passed with no manual
reset, BOOT entry, recovery or ST-Link intervention between the campaigns.
The normal PlatformIO command took 234.268 seconds including a full rebuild;
the campaign journal spans 117.490 seconds, with `ALL_VALIDATED` before COMMIT.
Both fresh target rows report `SUCCESS`, healthy, confirmed, validated and
available on build `ota-cb61d951ce57143aa874b049`, MCUboot hash
`6af4fc8cfdc63279e641e99f337b94acf53357fd63255e3264558392c7b0fa48`, error zero;
the Lead slot is available. This proves receiver, slot and campaign-storage
reuse through the normal workflow. Evidence:
`ota-journals/campaign-0d8573b7b00837aa.jsonl` and
`.pio/swd-recovery-20260924/complete-cycle-1000ms-status.json`.
