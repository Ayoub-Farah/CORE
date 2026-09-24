# Recovering an uncommitted OTA campaign

This procedure applies only to an explicitly identified campaign that reached
`ALL_VALIDATED` but failed before any participant committed or rebooted. It is
not an alternative way to accept an unverified trial image. Normal operation
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
board's separate USB recovery is recorded below; a complete subsequent fleet
update remains unverified.

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
The recovery-client suite covers these checks in 17 passing tests.

An empty or partial image list fails both modes. Do not select `--after-revert`
to bypass missing images or to infer that a move has completed. Diagnose the
interrupted state before any separate decision to resume it; the second-board
case above used complete backups and verified progress/content evidence.

The apply operation must establish all of the following before erasure:

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
It persists and verifies a recovery marker, rechecks local safety, then confirms
its own image before removing the failed campaign's keys. It removes the marker
last, allowing an interrupted repair to resume. Role, calibration and other
application metadata are preserved.

After an explicit repair-success report, install the corrected normal `OTA`
artifact through the bootloader, entered again with physical BOOT + RESET.
The repair utility deliberately disables software reset via 1200 baud.
Verify its active hash, local health,
confirmation and available slot, then repeat for the other board. A new fleet
campaign should begin only once every board is ready and the expected inventory
has been verified.

## Validation status

The CAN transfer and original failure above were observed on hardware. All 130
host/native tests pass, including the NVS replacement regression, corrected
repeated writes, guard refusals, interrupted repair transactions, and an ARM
fixture compiled with the installed image's short-enum layout.
The first board's scoped SWD repair passed on hardware, including normal helper
confirmation and a complete before/after flash comparison. The second board's
interrupted revert resumed through USB, with bootloader/NVS preservation proved
before installing the helper. Its subsequent guarded USB helper installation
and self-confirmed recovery succeeded with ST-Link disconnected; no complete
post-helper second-board dump is claimed. Each further board must pass its own
actual bootloader slot checks.
A complete corrected CAN campaign through reboot, reconciliation and maintenance
release has not yet been validated on hardware.
