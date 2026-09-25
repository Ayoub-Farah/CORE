# OwnTech: moving between USB, OTA v1 and OTA v2

This guide explains how to use the current Core project when boards already
run a USB application, the legacy OTA v1 prototype, or OTA v2. Commands below
use Windows PowerShell and the existing OwnTech bootloader. They do not install
a bootloader or replace a signing key.

**Current implementation status:** OTA v2 is experimental. The supplied profiles
leave deferred CAN activation unqualified, and a power-control application
needs real maintenance and health callbacks before installation. A successful
build or a memory-size report does not establish either condition. See
[Readiness before flashing](#readiness-before-flashing) before using any upload
command in this document.

## 1. Choose the right path

“USB” and “OTA” here describe the installed **application**. Both use the
existing USB bootloader for individual installation.

| Starting point | Intended result | Procedure | Main condition |
|---|---|---|---|
| Ordinary USB application | OTA v2 receiver or dedicated Lead | [Case A](#case-a--ordinary-usb-application-to-ota-v2) | Correct application integration and known board history |
| OTA v1 application | OTA v2 | [Case B](#case-b--ota-v1-to-ota-v2) | Resolve legacy campaign metadata first; no automatic v1 migration |
| OTA v2 receiver or Lead | Ordinary USB application, then OTA v2 again | [Case C](#case-c--ota-v2-back-to-usb-then-back-to-ota-v2) | Complete/reconcile the campaign and retire its metadata with the signed transition helper |

The current build environments have different purposes:

| Environment | Application built | Intended operation |
|---|---|---|
| `USB` | This checkout's `src/main.cpp`, ordinary USB profile | Install a non-OTA application over USB |
| `OTA` | This checkout's `src/main.cpp`, minimal v2 receiver profile | Install a receiver over USB; produce future CAN update images |
| `USB_LEAD` | `owntech/lead/main.cpp`, dedicated v2 Lead profile | Install the coordinator over USB; distribute receiver images from the PC |
| `OTA_RECOVERY` | A campaign-bound repair application | Repair only an explicitly supported, diagnosed interrupted campaign |
| `OTA_TRANSITION` | A board-bound terminal-v2 cleanup application | Retire completed OTA metadata for a normal USB round trip |

In v2, the Lead does **not** run the receiver's application and is **not** a CAN
update target. Reserve a separate board for it. Reusing an application board as
the Lead replaces that board's application with the coordinator.

## 2. Prepare the project and workstation

### 2.1 Verify the actual source checkout

Open the **Core checkout containing this OTA v2 implementation**, then run:

```powershell
Get-Location
git branch --show-current
git rev-parse --short HEAD
Get-FileHash ./src/main.cpp -Algorithm SHA256
```

`OTA` and `USB` compile `src/main.cpp` in **this directory**. Changing the branch
of another clone does not change this file. In particular,
`CoreV2/Core_Ana/src/main.cpp` and `MMC/MMC_ANA/src/main.cpp` are different source
locations. If you want the MMC application, bring the intended application and
its required configuration into this checkout, preserving your existing work,
and record its source path, branch, commit and SHA-256 before building.

Check `platformio.ini`, `src/app.ini`, and any `src/app.conf` or
`src/app.overlay`. Match the actual Spin and shield revisions. The supplied
OTA compatibility profile describes Spin 1.2.0 / Twist 1.4.2; a different
hardware/layout/signing setup needs a matching reviewed profile, not just a
different label in a manifest. The dedicated Lead does not apply the receiver's
`src/app.conf` or `src/app.overlay`.

The isolated `.pio/mm*` workspaces used for memory analysis compile the complete
arming path for sizing only. **Do not use their images for installation.**

### 2.2 Select the tools

With PlatformIO installed at its usual Windows location:

```powershell
$Python = Join-Path $env:USERPROFILE ".platformio/penv/Scripts/python.exe"
$Pio = Join-Path $env:USERPROFILE ".platformio/penv/Scripts/pio.exe"
$Mcumgr = Join-Path (Get-Location) "owntech/third_party/mcumgr.exe"
& $Pio --version
& $Pio device list
Test-Path $Mcumgr
```

Adapt these paths if your installation differs. `Test-Path` must return `True`
before the explicit provisioning/recovery commands below. Use the compatible
OwnTech MCUmgr executable; the existing USB build scripts can retrieve it when
missing. Keep the bootloader's existing compatible signing key.

Run commands from this Core directory, keep the same PowerShell session for
the variables above, and stop if a command fails. Replace all uppercase
placeholders such as `RECEIVER_USB_SERIAL` with your own recorded values.

### 2.3 Identify and label the boards

1. Put the power stage in its established safe, stopped state. Prevent automatic
   restarts from external controllers while changing firmware.
2. Close serial monitors, Scope acquisition and other programs using the ports.
3. Connect only the board being installed by USB. Disconnect CAN during initial
   installation or individual migration, after resolving any campaign in progress.
4. Use `device list` to record the board's stable **USB serial** and current COM
   port. Ports can change after reboot; a USB serial identifies the physical board.
5. Record its **CAN EUI-64** separately when the compatible application reports it.
   It is not the USB serial and not the temporary CAN address.

Merge the following settings into `src/app.ini`; do not discard existing
application settings or create duplicate INI sections:

```ini
[env:OTA]
custom_ota_serial = RECEIVER_USB_SERIAL

[env:USB_LEAD]
custom_ota_serial = LEAD_USB_SERIAL
custom_ota_expected_ids = RECEIVER_1_EUI, RECEIVER_2_EUI
custom_ota_timeout = 180
```

Change `custom_ota_serial` under `OTA` for each receiver you install. Each EUI
is exactly 16 hexadecimal characters. The v2 expected list contains **receivers
only**, never the Lead. An explicit identity list is preferable when you know
the boards; `custom_ota_expected_count` is an alternative receiver count.

### Readiness before flashing

Verify these application and bootloader requirements before Case A or B:

- The application provides meaningful `owntech_ota_enter_maintenance()` and
  `owntech_ota_check_health()` implementations. Weak defaults reject operation.
  The supplied LED demo has demo-specific callbacks; copying its unconditional
  success into MMC is not a valid integration. The MMC sources measured in this
  session do not yet provide the qualified integration needed for deployment.
- The installed bootloader, signing key, partition layout and actual board are
  compatible with the built image. An unconfirmed trial, a pending swap or an
  unexplained slot state must be resolved before ordinary replacement.
- Before a **CAN campaign**, deferred arming must have been qualified on the
  installed bootloader. `CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED` defaults to
  `n`, so the supplied receiver refuses `PREPARE` before erasing its slot.
  Enabling this option is a consequence of qualification, not a way to bypass it.

These are separate gates: a receiver can complete healthy USB initialization
without a CAN peer, while a later CAN campaign is still disabled. Follow the
[application integration notes](../docs/ota-implementation.md#application-integration),
[MMC integration audit](../docs/ota-mmc-safety-audit.md) and
[deferred-arm qualification requirements](../docs/ota-deferred-arm-qualification.md).
The implementation document contains historical v1 details; use this guide and
the [v2 operator guide](../docs/minimal-can-ota.md) for current commands and roles.

### 2.4 Know which image belongs to which transfer

| File | Use |
|---|---|
| `ota-artifacts/OTA/firmware.mcuboot.bin` | Padded, signed receiver image for individual USB installation |
| `ota-artifacts/OTA/firmware.can.bin` | Compact, signed receiver image for a v2 CAN campaign |
| `ota-artifacts/USB_LEAD/firmware.mcuboot.bin` | Padded, signed dedicated Lead image for USB installation |
| `ota-artifacts/USB/firmware.mcuboot.bin` + `firmware.usb.json` | Ordinary USB image with local build evidence |
| `ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin` | Generated repair image for one guarded recovery configuration |
| `ota-artifacts/OTA_TRANSITION/firmware.mcuboot.bin` | Generated normal-transition image for one board and terminal history |

Keep the associated JSON manifests for OTA artifacts. A `.can.bin` file is not
the ordinary USB upload file; a `.mcuboot.bin` file is not a v2 CAN campaign
file. Do not trim padding or relabel image classes manually.

The environment snapshots in `ota-artifacts/` survive PlatformIO build cleanup,
but a later build can overwrite them. Before deployment, copy the exact images,
manifests, configuration, source identity and campaign logs to a unique archive.
`ota-artifacts/` and `ota-journals/` are Git-ignored.

### 2.5 Read an installed v2 board's local status

Define this helper in the PowerShell session. It selects the exact USB serial
and requests only `info`; it does not upload, reset or start a campaign:

```powershell
function Get-OtaInfo([string]$BoardSerial) {
@'
import json, sys
sys.path.insert(0, "owntech/tools")
from lead_update import USBConnection, json_value
connection = USBConnection(sys.argv[1])
try:
    transport = connection.connect()
    print(json.dumps(transport.request("info"), default=json_value, indent=2))
finally:
    if connection.transport:
        connection.transport.close()
'@ | & $Python - $BoardSerial
}
Get-OtaInfo "BOARD_USB_SERIAL"
```

For a minimal receiver, this uses the existing console's read-only 2400-baud
`OTAR2` response and restores 115200 baud. No SMP request bytes are sent to that
console. Close other console consumers and retry if output is incomplete.

Expect `protocol: 2`, the intended `image_class`, EUI, version/build ID and image
hash. A completed standalone installation must report `active_confirmed: true`,
`local_healthy: true`, `slot_available: true`, and no error. A minimal receiver
must also report `maintenance: false`. The Lead's SMP `info` does not expose a
`maintenance` field: check that no campaign is unresolved and that its phase is
`IDLE`, initial `WAITING_CAN`, or reconciled `SUCCESS` as appropriate. Never
interpret a missing field as `false`.
`WAITING_CAN` is allowed before an ACK-capable CAN peer is connected; it is not
permission to start a campaign. Once the bus is ready, also require
`healthy: true`, `can_ready: true` and `available: true`.

For the **Lead's fleet status**, use:

```powershell
& $Python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --status
```

The current `--status` command also asks for the fleet `status` operation, which
the minimal receiver does not implement. Use `Get-OtaInfo` for receiver boards.
Neither current v2 status path is a general diagnostic client for OTA v1.

## Case A — ordinary USB application to OTA v2

### A1. Confirm this is an initial installation

Use this path for a known ordinary single-CDC USB application, with no unresolved
OTA history. A board which was returned to USB after OTA use may still contain
OTA metadata; changing its application did not erase that history. Treat an
unknown or previously interrupted history as a diagnosis/recovery case first.

### A2. Build the receiver and dedicated Lead

After completing the readiness checks, build both images:

```powershell
& $Pio run -e OTA -t mcuboot-image
& $Pio run -e USB_LEAD -t mcuboot-image
```

Check that both builds succeed and their manifests identify `receiver` and
`lead` respectively. Their build IDs are intentionally different. Archive the
artifacts before installing them.

### A3. Install each receiver individually

Connect the selected receiver alone, verify its configured USB serial, and run:

```powershell
& $Pio run -e OTA -t ota_init
```

This is the PlatformIO task **Initialize board over USB**. For a declared legacy
single-CDC application, it checks for a v2 status response first, then uses
1200-baud bootloader entry before any SMP probe. It refuses a recognized
different v2 class or image instead of silently overwriting it.

Wait for `PROVISIONED` or `ALREADY_INITIALIZED` and inspect the returned status.
Check the expected image hash, local health and active confirmation. An isolated
board can legitimately finish in `WAITING_CAN`. Record its EUI with its USB serial.

Repeat for every receiver, changing `custom_ota_serial` each time. Do not proceed
past `FAILED`, `RECOVERY_REQUIRED`, an unconfirmed image or a provisioning error.

If software entry is unavailable, use the
[explicit physical-entry procedure](#physical-entry-into-the-existing-bootloader),
then the receiver `--bootloader` command in [B4](#b4-install-v2-over-usb-after-the-state-is-admissible).
Do not repeatedly reset a board with an uncertain trial or pending image.

### A4. Install the dedicated Lead

Connect the board reserved for the Lead, verify `custom_ota_serial` under
`USB_LEAD`, and run:

```powershell
& $Pio run -e USB_LEAD -t ota_init
```

Require a locally healthy, confirmed **`lead`** image. This installs
`owntech/lead/main.cpp`, not the MMC receiver application. Installing an existing
v2 receiver as a Lead is a deliberate class change and is refused by normal
initialization; resolve its history and use explicit physical bootloader entry.

### A5. Connect the CAN network and verify the inventory

1. Connect and power the intended Lead and receivers with the appropriate CAN
   transceivers, common reference and termination.
2. The supplied profile uses classic CAN at **500 kbit/s**, FDCAN2 PB5 RX / PB6 TX.
   Keep the existing HRTIM wiring unchanged.
3. Check that boards leave `WAITING_CAN` and become healthy and available.
4. Set the complete expected receiver EUI list. Remove the old v1 Lead EUI from
   that list explicitly; v2 does not update the Lead with the receiver image.
5. Confirm the deferred-arm qualification gate has been satisfied before starting
   a campaign. With the shipped default `n`, stop at initialization/status tests.

### A6. Distribute a receiver update

Make the intended receiver application change in this checkout, then run:

```powershell
& $Pio run -e USB_LEAD -t lead_update
```

This task builds `OTA` and streams its compact receiver image from the PC through
the Lead to the expected receivers. It does not install or reset the Lead.
Keep the PC process, USB connection and CAN network available throughout.

To distribute an already archived exact image instead of rebuilding:

```powershell
& $Python owntech/tools/lead_update.py --image PATH_TO_ARCHIVE/firmware.can.bin --manifest PATH_TO_ARCHIVE/firmware.can.json --serial LEAD_USB_SERIAL --expected-id RECEIVER_1_EUI --expected-id RECEIVER_2_EUI
```

Success requires all frozen receivers to return with the expected healthy,
confirmed image and collective maintenance release. Keep the reported
`ota-journals/campaign-*.jsonl` file. `100%` transfer alone is not success.

If the PC loses contact after commit, inspect and reconcile the original journal:

```powershell
& $Python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --status
& $Python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --reconcile-journal ota-journals/campaign-CAMPAIGN_ID.jsonl
```

Reconciliation uses the frozen campaign; do not substitute a new receiver list.
`ABORT` is not proof that an already armed image was disarmed. Preserve failed or
partial journals and use the scoped recovery guidance rather than resetting or
starting another campaign blindly.

## Case B — OTA v1 to OTA v2

**There is no automatic v1-to-v2 migration in this implementation.** The wire
protocol, image format, Lead role and persistent journals changed. Do not send
a v2 compact image through a v1 CAN campaign or use the v2 client to operate a
v1 fleet.

### B1. Record the v1 state before changing anything

1. Preserve the exact v1 images, configuration, client checkout and campaign logs.
2. Use the matching archived **v1** client's read-only status facility to identify
   the old Lead, receivers, active hashes, confirmation and campaign state. The
   current `lead_update.py --status` is v2-only.
3. Finish/reconcile an active v1 campaign with its matching tools if possible.
   If it failed or its state is uncertain, diagnose that state before resetting
   or installing another application.
4. Plan the separate v2 Lead and the new receiver-only inventory. Do not assume
   the old Lead can keep running the MMC application after conversion.

### B2. Determine whether legacy metadata permits migration

| v1 board history | Required action |
|---|---|
| Known initial provisioning with no campaign, or independently verified clean OTA metadata | Verify confirmed application/boot state, then use B4 |
| Interrupted campaign matching a supported recovery mode, with its exact journal and artifacts | Complete B3 first |
| A successful previous v1 campaign | Stop for an explicit metadata-migration procedure; it is not implemented here |
| Missing journal, corrupt/unknown metadata, unexplained pending image or interrupted swap | Stop for diagnosis and preserve evidence |

V2 refuses old local/fleet records and leaves their bytes intact. A successful
v1 campaign can retain a `SUCCEEDED` journal, so “the last campaign succeeded”
does not mean “NVS is ready for v2.” An `IDLE` status or a normal bootloader
image list cannot prove that legacy NVS records are absent.

There is no general-purpose “clear OTA history” command. A full NVS erase would
also risk calibration and other board metadata; it is not a migration step in
this guide. Ordinary USB uploading does not perform this cleanup either.

### B3. Recover only a supported interrupted v1 campaign

The existing repair utility recognizes narrowly defined cases:

| Mode | Required type of evidence |
|---|---|
| Default, no scope flag | Complete validation and a failed COMMIT request, with no actual commit/reboot/success; device journal still `VALID`, commit ID zero |
| `--staged-lead-only` | Lead completely staged, failed START, no CAN transfer or COMMIT; Lead only |
| `--prepared-follower-only` | Follower left `PREPARING`/`READY` after that early failure, commit ID zero and absent secondary image |

Use the [guarded legacy recovery procedure](../docs/ota-recovery.md) to establish
the evidence and select the mode. The appendix below gives current artifact
paths. `--compact-receiver-only` is a **v2** mode, not a way to clear v1 records.
The generators deliberately reject successful campaigns and unsupported states.

Proceed to B4 only after the repair application reports `RECOVERED` or
`ALREADY_RECOVERED` with the expected EUI and `confirmed=1`. It confirms itself
and removes only its guarded OTA maintenance/journal/recovery keys; calibration,
the legacy role key and other non-OTA metadata are preserved. The repair image
is not the final receiver application.

### B4. Install v2 over USB after the state is admissible

Build/archive the v2 receiver and Lead images as in A2. Connect one board at a
time and enter its existing bootloader **physically**, as described below.
`ota_init` is not the v1 migration command: its legacy-console path requires
exactly one CDC interface and rejects the old dual-CDC v1 application.

For a receiver, after checking the physical identity and bootloader image state:

```powershell
& $Python owntech/tools/provision_ota.py --bootloader --image ota-artifacts/OTA/firmware.mcuboot.bin --image-class receiver --serial RECEIVER_USB_SERIAL --mcumgr $Mcumgr
```

For the separate Lead, after the same checks on that board:

```powershell
& $Python owntech/tools/provision_ota.py --bootloader --image ota-artifacts/USB_LEAD/firmware.mcuboot.bin --image-class lead --serial LEAD_USB_SERIAL --mcumgr $Mcumgr
```

`--bootloader` declares that physical entry has already happened; it avoids
another 1200-baud reset. It does not migrate NVS or bypass the bootloader's
image-state requirements. Keep the original JSON manifest beside each image.

Verify protocol 2, intended class/hash and healthy local confirmation as in
Section 2.5. If a legacy journal causes `RECOVERY_REQUIRED` or another startup
failure, stop; repeated uploads do not fix its persistence. Once every board
is correctly initialized, continue with A5 and A6.

## Case C — OTA v2 back to USB, then back to OTA v2

**A dedicated signed transition helper now supports this normal round trip
without ST-Link.** Physical BOOT + RESET and the existing USB bootloader are
used for each installation. The complete procedure is in
[OTA v2 to ordinary USB, and back](../docs/ota-usb-transition.md), including
PowerShell commands, per-board archives, interruption handling and the return
to a different OTA v2 image.

This complete workflow has not yet been qualified on hardware. The software
includes host tests for the cleanup transaction and its interrupted mutations;
those do not establish physical swap/NVS behavior or application safety.

### C1. Finish the current campaign

Before disconnecting a receiver, finish/reconcile the original frozen campaign.
Require all expected images healthy and confirmed, collective release, and the
Lead's recorded `SUCCESS`. Retain the original full campaign journal and exact
image artifacts. Prevent a new campaign while converting boards, and convert
the Lead last when retiring the fleet.

The normal transition accepts a successful v2 campaign or a board that has never
participated in a campaign. `--no-campaign` does not discard history: the helper
requires the local and fleet records to be absent. An unknown, corrupt, legacy,
failed or unresolved committed state remains a recovery problem.

### C2. Capture and prepare the transition

Use `transition_ota_usb.py capture` while the old v2 application is still running.
Archive the live board info, the exact currently installed source image and
manifest, and the successful campaign journal when applicable. Then run
`prepare_ota_transition.py` with that evidence and build `OTA_TRANSITION`.

The generated helper is bound to the board EUI, current image and exact expected
terminal records. It works with the existing v2 info interface; no preliminary
upgrade of the installed receiver is required. Archive the generated JSON/header
pair and helper image/manifest before another operation overwrites build outputs.

### C3. Install the helper and wait for cleanup proof

Enter the existing bootloader with BOOT + RESET. Use the transition tool's
`install-helper --inspect`, followed by `install-helper --apply` with the same
archived configuration and board identity. Keep its operation JSONL log.

Once the helper runs, use `receipt` to record its EUI, operation token, successful
result and confirmation. It inhibits outputs, verifies the terminal metadata,
persists a resumable intent, confirms itself, then removes only the obsolete
OTA local/fleet/maintenance records. It removes its operation marker last.
Calibration, role and other NVS records are retained.

**Do not install USB until the tool reports `CLEANUP_CONFIRMED`.** An upload
percentage or `RESET_REQUESTED` result is not proof that cleanup completed.
This policy is separate from `OTA_RECOVERY` and `--compact-receiver-only`.

### C4. Install and use the ordinary USB application

Build `USB`, checking this checkout's actual `src/main.cpp`, and retain its
`firmware.mcuboot.bin` and generated `firmware.usb.json`. The manifest records
that the local build disables OTA/recovery and contains ordinary Core's image
confirmation function.

Enter the bootloader from the helper. Use `install-usb --inspect`, then
`install-usb --apply`, passing the cleanup receipt and exact USB artifacts.
Allow the ordinary application to start and confirm itself. Re-enter the
bootloader and use `verify-usb --apply` to record the confirmed image and return
with one reset to normal USB execution.

Ordinary USB development can continue without another helper. After installing
a different USB build, archive its exact image/manifest, and run `verify-usb`
again with those files and the same operation log before returning to OTA.
Do not substitute a previous USB image's hash or manifest.

### C5. Return to OTA v2

Build/archive the intended new `OTA` receiver or `USB_LEAD` image and manifest.
Use the **padded USB artifact**, not `firmware.can.bin`. Enter the bootloader
physically and use `return-ota --inspect`, then `return-ota --apply`, with the
same operation archive, cleanup receipt and log plus the currently verified USB
and new OTA artifacts. After boot, run `verify-ota` against the live application.

The returned image may have a different hash/version/build ID from the previous
OTA image: the obsolete journal has already been retired. Require the intended
EUI/class/hash, local health, confirmation and no maintenance, then reconnect
CAN and verify readiness. The deferred-arm qualification and application health
requirements still apply before another CAN campaign.

For a later OTA-to-USB conversion, start a new operation from the then-current
image and campaign evidence. An old cleanup receipt is not authorization to
forget a subsequent campaign.

Follow the [full command walkthrough](../docs/ota-usb-transition.md) rather than
using raw MCUmgr uploads or the legacy `pio run -e USB -t upload` task for the
initial mode change. Those upload paths do not retire OTA metadata. Never erase
all NVS, force image confirmation or use repeated resets to bypass a refusal.

## Physical entry into the existing bootloader

Use this only after the board/campaign state has been checked. For the audited
OwnTech v1.1.0 bootloader, hold the board's **BOOT** button, press and release
**RESET**, keep BOOT held for about one second, then release it. This enters its
USB image-management service before the normal swap decision. Verify the actual
installed bootloader and board's procedure; do not assume every revision behaves
identically. See the [bootloader evidence and entry explanation](../docs/ota-recovery.md#why-physical-bootloader-entry-is-required).

This is not the STM32 ROM DFU procedure using PB8/BOOT0. The task
`recovery-bootloader` reinstalls the bootloader and is **not** a USB/OTA mode-switch
command. None of the three normal paths above requires it.

After every entry, re-enumerate the selected USB serial and verify its image
service on the current COM port. A software 1200-baud reset is not a substitute
for physical entry when an unresolved pending swap must be avoided.

## Appendix: current commands for a scoped repair

This is a command template for an already diagnosed case, not a fourth general
migration path. First choose exactly the mode supported by the evidence:

```powershell
# Example only: a diagnosed v1 prepared-follower failure.
$RecoveryMode = "--prepared-follower-only"
& $Python owntech/tools/prepare_ota_recovery.py --journal PATH_TO_ORIGINAL_CAMPAIGN.jsonl $RecoveryMode
```

For staged v1 Lead repair use `--staged-lead-only`; for the documented default
v1 fully validated case omit the scope flag from **all** commands. For the
supported v2 precommit receiver case use `--compact-receiver-only` throughout.
Do not change modes merely to make the generator accept a journal.

After successful configuration generation:

```powershell
& $Pio run -e OTA_RECOVERY -t mcuboot-image
```

Archive the generated configuration/header together with the exact repair
image/manifest. Enter the selected board's bootloader physically, then inspect:

```powershell
& $Python owntech/tools/recover_ota.py $RecoveryMode --inspect --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial BOARD_USB_SERIAL --identity BOARD_EUI
```

Only after a successful inspection of this exact board/configuration, apply:

```powershell
& $Python owntech/tools/recover_ota.py $RecoveryMode --apply --config .pio/ota-recovery-config/owntech_ota_recovery_config.json --image ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin --manifest ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json --serial BOARD_USB_SERIAL --identity BOARD_EUI --mcumgr $Mcumgr
```

The PC's `RESET_REQUESTED` result only means it requested the reboot. Re-enumerate
the same USB serial, then open the repair application's current console port:

```powershell
& $Pio device list
& $Pio device monitor --port CURRENT_RECOVERY_COM_PORT --baud 115200
```

Wait for its periodically printed result, for example:

```text
OTA_RECOVERY RECOVERED rc=0 EUI=EXPECTED_EUI confirmed=1; outputs inhibited
```

`ALREADY_RECOVERED rc=1` with the expected EUI and `confirmed=1` is also a
successful result. Close the monitor before the next bootloader entry or upload.
Recovery is resumable only within its guards. `--after-revert`
does not initiate a revert; it belongs only to the separately proven v1 case
described in the recovery guide. Do not add it to a precommit receiver repair.

The recovery application intentionally requires physical BOOT + RESET for its
next replacement. Install the intended final receiver/Lead via B4. Case C starts
from a confirmed live v2 application with captured identity and known history;
its transition tool does not treat a legacy repair image as that source.

## Quick troubleshooting

| Symptom | Next action |
|---|---|
| Wrong program appears in the build | Check current directory, `src/main.cpp` hash and the source clone/branch; a different checkout is not synchronized automatically |
| `WAITING_CAN` after individual initialization | Check local confirmation first, then CAN wiring/termination and an active compatible peer |
| `PREPARE` refused on the supplied profile | Check deferred-arm qualification and application maintenance; do not bypass either gate |
| `ota_init` rejects multiple CDC interfaces | Check whether this is legacy v1; use its migration procedure rather than retrying legacy-console initialization |
| Different class or image already installed | Verify the board's intended role and history; normal initialization intentionally refuses replacement |
| Receiver `--status` fails after reading `info` | Use the `Get-OtaInfo` helper; fleet `--status` is for the dedicated Lead |
| Empty image list, `EBADSTATE`, repeated `0%`, unknown pending state | Preserve logs and diagnose the boot state; do not force confirmation, erase NVS or reset blindly |
| `RECOVERY_REQUIRED` after v1 replacement or an unprepared USB round trip | Existing metadata may conflict with the new image; repeated flashing is not a metadata migration; normal v2 round trips use Case C before USB installation |
| Lead absent from the expected v2 targets | Correct: it is the coordinator, not a receiver target |

## References in this checkout

- [Current v2 operator guide](../docs/minimal-can-ota.md)
- [Normal USB round-trip commands](../docs/ota-usb-transition.md)
- [Software validation and qualification limits](../docs/minimal-can-ota-validation.md)
- [Legacy campaign recovery evidence and procedures](../docs/ota-recovery.md)
- [USB provisioning implementation](../owntech/tools/provision_ota.py)
- [Current client and USB status transport](../owntech/tools/lead_update.py)
- [Persistent OTA state and compatibility checks](../zephyr/modules/owntech_ota/zephyr/src/ota_storage.cpp)
- [Ordinary USB uploader](../owntech/scripts/pre_bootloader_serial.py)
