# OTA v2 to ordinary USB, and back to OTA v2

This workflow uses a temporary signed transition application to retire completed
OTA v2 metadata before installing an ordinary USB application. The transition
application is then replaced: it adds no permanent RAM or Flash allocation to
the minimal receiver or the ordinary USB application. The existing bootloader,
signing key, calibration, board role record and other application NVS keys are
preserved.

**Software support is implemented; this complete round trip has not been
validated on physical boards.** Host fault-injection tests exercise interruption
of the cleanup transaction. They do not qualify MCUboot swap interruptions,
physical NVS behavior or power-stage safety. The application and bootloader
requirements in the [step-by-step guide](../Idea/usb_ota_v1_v2_step_by_step_guide.md#readiness-before-flashing)
still apply. No board is flashed by building or preparing a configuration.

The normal path needs **USB and physical BOOT + RESET**, without ST-Link. It
starts from the live OTA v2 application with known artifacts and terminal
history. It is not a general recovery utility for a failed campaign, a v1
migration, or cleanup of an unknown USB application's previous OTA history.

The supported bootloader interface is the OwnTech **Zephyr MCUmgr USB image
service** reached by physical BOOT entry. Generic MCUboot serial recovery is
different: its default upload can target the primary slot. Before every new
upload, the tool therefore requires a successful explicit secondary-slot erase
and verifies that the confirmed primary is unchanged. An unsupported erase
stops the operation before upload. The helper version is `0.0.1+0`; a different
bootloader enforcing version downgrade prevention requires separate support.

## 1. Establish a completed campaign or genuinely unused OTA state

Keep the power stage safely stopped and prevent automatic restarts. Close Scope
and serial monitors. Before removing a receiver, finish/reconcile the whole
frozen campaign through the Lead and retain its original JSONL journal:

```powershell
& $Python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --status
& $Python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --reconcile-journal PATH_TO_CAMPAIGN.jsonl
```

Only run reconciliation when a campaign exists. Require confirmed, healthy
receivers at the expected image, collective maintenance release, and recorded
`SUCCESS` for the complete roster. Transfer completion, a local confirmation,
or `ABORT` is insufficient. Prevent any new campaign from starting during the
conversion. If retiring the fleet, convert the Lead last.

The configuration generator supports two separate cases:

| Evidence | Selected policy |
|---|---|
| Original successful v2 campaign journal, including all frozen receivers | `--journal` derives the exact terminal receiver or Lead record |
| Board initialized in v2 but never used in a campaign | `--no-campaign` requires absence of local and fleet records, checked on the board by the helper |

`--no-campaign` is not an override for missing evidence. Even if a rebooted Lead
reports `IDLE`, an existing fleet record causes this policy to refuse. Legacy,
corrupt, active, aborted, failed and unresolved committed records are not
accepted. Diagnose those separately rather than forcing a normal mode switch.

## 2. Create a permanent operation archive and capture the board

Run the following from the Core checkout containing this implementation. Replace
the placeholders, including the current source artifact paths. The source must
be the **exact already-running v2 image**, not a rebuild made today. A receiver
source can be its archived compact CAN image/manifest or padded USB pair; a Lead
uses its dedicated Lead image/manifest. Keep the archive at its original path:
the configuration records absolute input locations and verifies their bytes.

```powershell
$Python = Join-Path $env:USERPROFILE ".platformio/penv/Scripts/python.exe"
$Pio = Join-Path $env:USERPROFILE ".platformio/penv/Scripts/pio.exe"
$Mcumgr = Join-Path (Get-Location) "owntech/third_party/mcumgr.exe"
$Tool = "owntech/tools/transition_ota_usb.py"
$Serial = "BOARD_USB_SERIAL"
$Eui = "BOARD_EUI_16_LOWERCASE_HEX"
$Operation = Join-Path (Get-Location) "ota-artifacts/transitions/UNIQUE_OPERATION_NAME"
New-Item -ItemType Directory -Path $Operation -ErrorAction Stop | Out-Null
foreach ($Name in @("source", "config", "helper", "usb", "ota")) {
    New-Item -ItemType Directory -Path (Join-Path $Operation $Name) -ErrorAction Stop | Out-Null
}
Copy-Item "PATH_TO_EXACT_RUNNING_IMAGE.bin" (Join-Path $Operation "source/image.bin")
Copy-Item "PATH_TO_EXACT_RUNNING_MANIFEST.json" (Join-Path $Operation "source/manifest.json")
$Info = Join-Path $Operation "board-info.json"
& $Python $Tool capture --serial $Serial --info $Info
if ($LASTEXITCODE -ne 0) { throw "Board capture failed" }
```

The board must still run its OTA application during capture. The tool reads its
existing v2 info endpoint; no new command needs to be present in the installed
v2 firmware. Check the captured EUI, serial, class and image against your board
labels and archives. A source mismatch is a refusal, not permission to replace
the expected hash in JSON.

Copy the successful campaign journal into this archive before preparation. Use
one of these argument arrays:

```powershell
# A completed campaign: retain its original complete journal.
Copy-Item "PATH_TO_SUCCESSFUL_CAMPAIGN.jsonl" (Join-Path $Operation "campaign.jsonl")
$HistoryArgs = @("--journal", (Join-Path $Operation "campaign.jsonl"))

# Alternative ONLY for a board never used in a campaign:
# $HistoryArgs = @("--no-campaign")
```

## 3. Prepare and build the board-specific helper

```powershell
$PrepareArgs = @(
    "--image", (Join-Path $Operation "source/image.bin"),
    "--manifest", (Join-Path $Operation "source/manifest.json"),
    "--board-info", $Info
) + $HistoryArgs
& $Python owntech/tools/prepare_ota_transition.py @PrepareArgs
if ($LASTEXITCODE -ne 0) { throw "Transition preparation refused" }
& $Pio run -e OTA_TRANSITION -t mcuboot-image
if ($LASTEXITCODE -ne 0) { throw "Transition build failed" }
Copy-Item .pio/ota-transition-config/owntech_ota_recovery_config.h (Join-Path $Operation "config/")
Copy-Item .pio/ota-transition-config/owntech_ota_recovery_config.json (Join-Path $Operation "config/")
Copy-Item ota-artifacts/OTA_TRANSITION/firmware.mcuboot.bin (Join-Path $Operation "helper/")
Copy-Item ota-artifacts/OTA_TRANSITION/firmware.mcuboot.json (Join-Path $Operation "helper/")
```

The helper is signed with the existing compatible key and bound to one board,
source image, operation token and exact expected terminal records. Its generated
header retains the recovery configuration filename, but `OTA_TRANSITION` has a
separate configuration directory and policy from `OTA_RECOVERY`. Do not edit
the generated files or build another operation over them before archiving this
pair and its helper. Generated environment snapshots are overwritten by later
builds; the operation archive must survive them.

Define the reusable arguments against that archive:

```powershell
$Config = Join-Path $Operation "config/owntech_ota_recovery_config.json"
$Helper = Join-Path $Operation "helper/firmware.mcuboot.bin"
$HelperManifest = Join-Path $Operation "helper/firmware.mcuboot.json"
$Receipt = Join-Path $Operation "cleanup-receipt.json"
$Log = Join-Path $Operation "transition.jsonl"
$Common = @(
    "--config", $Config, "--helper", $Helper, "--helper-manifest", $HelperManifest,
    "--serial", $Serial, "--identity", $Eui, "--log", $Log
)
```

Use this same operation log for every later step, including the return to OTA.

## 4. Build and archive the ordinary USB application

Check the actual `src/main.cpp`, hardware configuration and branch before the
build. This is also necessary for a former Lead: `USB` builds the application in
this checkout, not the dedicated Lead program.

```powershell
& $Pio run -e USB -t mcuboot-image
if ($LASTEXITCODE -ne 0) { throw "USB build failed" }
Copy-Item ota-artifacts/USB/firmware.mcuboot.bin (Join-Path $Operation "usb/")
Copy-Item ota-artifacts/USB/firmware.usb.json (Join-Path $Operation "usb/")
$UsbImage = Join-Path $Operation "usb/firmware.mcuboot.bin"
$UsbManifest = Join-Path $Operation "usb/firmware.usb.json"
$UsbArgs = @("--usb-image", $UsbImage, "--usb-manifest", $UsbManifest)
```

`firmware.usb.json` records the exact image and local build evidence: no OTA or
recovery profile, and the ordinary Core automatic confirmation function present
in the ELF. The tool rejects a receiver image relabeled as USB. This evidence
does not replace MCUboot signature verification or application safety testing.

## 5. Install the helper and record completed cleanup

Connect only the selected board by USB. For the audited OwnTech bootloader,
hold **BOOT**, press and release **RESET**, keep BOOT held for about one second,
then release it. Use the actual board's qualified procedure. This enters the
existing MCUboot USB service, not the STM32 ROM DFU mode. The tool selects the
current port by the explicit serial number and refuses a live OTA response.

```powershell
& $Pio device list
& $Python $Tool install-helper @Common --inspect
if ($LASTEXITCODE -ne 0) { throw "Helper installation preflight refused" }
& $Python $Tool install-helper @Common --apply --mcumgr $Mcumgr
if ($LASTEXITCODE -ne 0) { throw "Helper installation stopped; preserve the log" }
```

Inspection is the default and does not upload, erase or reset. `--apply`
rechecks the confirmed original primary and refuses an unexpected pending
image. It always requests erasure of **slot 1**, even when no secondary image
is listed, then requires a successful acknowledgement, the unchanged primary
and no remaining secondary image before upload. This also distinguishes the
required secondary-slot management service from unsupported generic serial
recovery. It uploads the exact helper, verifies the resulting candidate and
requests one reset. It does not erase NVS or force application confirmation
through MCUmgr. The same erase-and-readback guard protects each new USB and
returning OTA upload.

Wait for the helper to boot, then record its actual completion:

```powershell
& $Python $Tool receipt @Common --receipt $Receipt --timeout 30
if ($LASTEXITCODE -ne 0) { throw "No cleanup receipt: do not install USB" }
$AfterCleanup = $Common + @("--receipt", $Receipt)
```

The helper inhibits outputs and repeatedly prints a record containing the exact
EUI and operation token. Only `rc=0` or `rc=1` with `confirmed=1` produces
`CLEANUP_CONFIRMED`. A PC `RESET_REQUESTED` result is not proof of cleanup. Receipt
collection only reads the helper console at 115200 baud; close other monitors.

On the device, the helper validates the identity, original backup image hash,
record CRCs, terminal state and exact prepared bytes. It persists and verifies
an operation marker, confirms itself, then removes only the OTA fleet, local
journal and maintenance keys with readback. It removes the operation marker
last. Calibration, role and other NVS records are not cleared.

## 6. Install USB and verify its normal confirmation

Enter the bootloader physically again, now from the helper, and run:

```powershell
& $Python $Tool install-usb @AfterCleanup @UsbArgs --inspect
if ($LASTEXITCODE -ne 0) { throw "USB installation preflight refused" }
& $Python $Tool install-usb @AfterCleanup @UsbArgs --apply --mcumgr $Mcumgr
if ($LASTEXITCODE -ne 0) { throw "USB installation stopped; preserve the log" }
```

Let the ordinary USB application finish startup and perform its existing local
confirmation. Verify expected application behavior with the power stage still
stopped. Then enter the bootloader physically once more:

```powershell
& $Python $Tool verify-usb @AfterCleanup @UsbArgs --apply
if ($LASTEXITCODE -ne 0) { throw "USB confirmation not established" }
```

This requires the exact USB image as a confirmed primary with no unexplained
pending state. It records `USB_VERIFIED` in the operation log and, with
`--apply`, requests one reset back to normal USB operation. Without `--apply`,
it records the verification and leaves the board in its bootloader.

The board now runs the ordinary USB application with clean OTA metadata.
`OTAR2` status and CAN OTA updates are no longer available in this application.

### Further ordinary USB development

You can continue installing ordinary USB builds during this USB period without
running the cleanup helper again. After such uploads, archive the **exact last
installed** USB image and its generated `firmware.usb.json` in a new directory;
retain the previous artifacts for the operation history. Update the arguments:

```powershell
$UsbRevision = Join-Path $Operation "usb/revision-2"
New-Item -ItemType Directory -Path $UsbRevision -ErrorAction Stop | Out-Null
Copy-Item ota-artifacts/USB/firmware.mcuboot.bin $UsbRevision
Copy-Item ota-artifacts/USB/firmware.usb.json $UsbRevision
$UsbImage = Join-Path $UsbRevision "firmware.mcuboot.bin"
$UsbManifest = Join-Path $UsbRevision "firmware.usb.json"
$UsbArgs = @("--usb-image", $UsbImage, "--usb-manifest", $UsbManifest)
```

Enter the bootloader physically and repeat the `verify-usb` command from
Section 6 with the updated arguments. This checks the current exact confirmed
USB image, allows an inactive nonpending previous application in the backup slot, and records
the new USB hash. Use these updated arguments when returning to OTA. Do not
reuse the old USB manifest or its verification event for a changed application.

## 7. Return to a newly built OTA v2 application

Keep the operation archive, receipt and log throughout the USB period. Build a
compatible receiver image, including the real application maintenance/health
integration, and archive it before another build:

```powershell
& $Pio run -e OTA -t mcuboot-image
if ($LASTEXITCODE -ne 0) { throw "OTA build failed" }
Copy-Item ota-artifacts/OTA/firmware.mcuboot.bin (Join-Path $Operation "ota/")
Copy-Item ota-artifacts/OTA/firmware.mcuboot.json (Join-Path $Operation "ota/")
$OtaImage = Join-Path $Operation "ota/firmware.mcuboot.bin"
$OtaManifest = Join-Path $Operation "ota/firmware.mcuboot.json"
$OtaArgs = @("--ota-image", $OtaImage, "--ota-manifest", $OtaManifest)
```

For a dedicated Lead, build `USB_LEAD` and copy that environment's padded
image/manifest instead. Select the intended class deliberately; keep the Lead
out of receiver campaign lists. The returning image can have a different hash,
version and build ID from the previous OTA application. Hardware layout and
signing-key compatibility still apply.

Enter the bootloader physically from the ordinary USB application:

```powershell
& $Python $Tool return-ota @AfterCleanup @UsbArgs @OtaArgs --inspect
if ($LASTEXITCODE -ne 0) { throw "OTA return preflight refused" }
& $Python $Tool return-ota @AfterCleanup @UsbArgs @OtaArgs --apply --mcumgr $Mcumgr
if ($LASTEXITCODE -ne 0) { throw "OTA return stopped; preserve the log" }
```

The return requires the exact confirmed USB image and its earlier `USB_VERIFIED`
event in this operation log. After the new OTA application boots:

```powershell
& $Python $Tool verify-ota @Common @OtaArgs
if ($LASTEXITCODE -ne 0) { throw "Returned OTA image is not locally ready" }
```

Require `OTA_VERIFIED`, the intended EUI/class/hash, local health, confirmation
and no maintenance. `WAITING_FOR_PEER` can be an admissible local initialization
result: reconnect the CAN network and verify full readiness before a campaign.
No old campaign hash needs to be reused because its obsolete journals were
retired before USB operation. Update the expected receiver list explicitly and
respect the existing deferred-arm qualification gate before CAN updates.

For another OTA-to-USB conversion later, start a **new capture and operation**.
In particular, after another OTA campaign, the previous receipt no longer
proves that the current OTA metadata can be retired. Complete the new campaign
and prepare a helper against its current source artifacts and terminal history.

## Interrupted operations

Preserve the complete archive and original log. A refusal is a diagnosis point,
not a reason to erase NVS, change generated guards, force-confirm an image or
repeat resets.

| Point of interruption | Safe continuation |
|---|---|
| Helper not yet confirmed | No OTA key has been removed. A reset may revert to the original image; inspect the actual slot state before repeating the same guarded installation. |
| Helper confirmed; cleanup interrupted | The confirmed helper resumes its matching cleanup transaction on boot. Collect its completion receipt before proceeding. Every remaining record must still match; foreign/corrupt state is refused. |
| Helper reports a negative result | Keep the helper and logs; resolve the reported identity, metadata, storage or health failure. Do not install USB without a receipt. |
| Upload completed but the PC lost the reply before reset | Re-enter/keep the bootloader and inspect. `--resume` accepts only the exact pending candidate with recorded successful secondary preparation and an upload request in the same operation log. |
| Explicit secondary erase is unsupported or primary changes | Stop before upload. Do not bypass the guard or switch to generic MCUboot serial recovery; the required bootloader interface/state has not been established. |
| Partial upload or unexpected pending image | `--resume` is not a partial-transfer repair. Diagnose the state; do not reset an unverified candidate. |
| USB or returned OTA image is unconfirmed | Stop and diagnose startup/boot state. These tools do not force confirmation to make the sequence pass. |

For the narrowly supported completed-upload case, repeat the relevant step with
`--resume --inspect`, then `--resume --apply` after it passes, preserving all
arguments and the original log. This requests a reset of that already-verified
candidate; it does not upload again or authorize an unrelated pending image.

## Qualification before deployment

Bench validation still needs a complete receiver and Lead round trip, including
a different returning OTA image, confirmation checks, preserved calibration,
and a successful subsequent CAN campaign. Test power cuts around helper upload,
MCUboot swaps, marker persistence, confirmation and every NVS deletion. Confirm
that unresolved commit/trial states are refused and outputs remain inhibited.

The portable transaction tests cover both sides of every modeled mutation and
refuse stale/corrupt records. They support this design; they do not constitute
the missing hardware qualification.
