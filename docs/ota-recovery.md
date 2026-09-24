# Recovering an uncommitted campaign through USB

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
could exhaust NVS: garbage collection preserves the previous value until the
new value is durable. A host regression reproduces this failure with other
metadata present. The corrected record is 304 bytes; admission also reserves
replacement space, GC overhead, and the implicit storage-version entry before
erasing an image slot. Existing OTA1 fleet records remain readable. These
semantics follow [Zephyr NVS](https://docs.zephyrproject.org/4.0.0/services/storage/nvs/nvs.html).

The existing application API cannot resume this failed COMMIT. Its abort
operation neither removes the padded activation trailer nor clears maintenance.
Do not reset normally while that staged image remains pending.

## Why physical bootloader entry is required

In the inspected OwnTech bootloader v1.1.0 source, the physical user button is
checked **before** `boot_go()`. Hold BOOT, press and release RESET, keep BOOT
held for about a second, then release it. This stops in the USB image-management
server without swapping the pending image. Software boot-mode entry is checked
after `boot_go()`, so a 1200-baud reset is unsuitable for this procedure.

The bootloader configuration enables `CONFIG_MCUMGR_GRP_IMG_ALLOW_ERASE_PENDING`.
The repair client still checks the actual USB receiver and image slots before
performing any erase. If the installed bootloader differs or any check fails,
stop and preserve the diagnostic log; do not force a confirmation or erase the
primary slot.

References: [OwnTech v1.1.0 bootloader](https://github.com/owntech-foundation/bootloader/tree/v1.1.0)
and the [OwnTech bootloader guide](https://docs.owntech.org/latest/bootloader/docs/getting_started/).

## Current hardware stop: no recognized images

The first board (`3232500B002B002D`, EUI `1ccd6d8a80f3af97`) entered its USB
bootloader on 2026-09-24. At 16:35 UTC, the read-only recovery inspection returned
`{"images": [], "splitStatus": 0}`. A second fresh read on the same USB serial
returned the same state. Inspection stopped before any erase, upload or reset;
the recovery utility has not been installed. Additional read-only OS parameters,
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

The required original and staged hashes therefore cannot be checked. Do not
bypass the recovery guards, upload blindly, force confirmation or request a
normal reset. Keep the first board in this state and avoid resetting the second
board, which still has the uncommitted staged image. The audited USB service has
no raw flash-read operation with which to resolve the missing image identities.
The next diagnostic path is a read-only flash/option-byte inspection through
SWD, or assistance from OwnTech with the installed bootloader. No SWD programmer
was available at the bench, so hardware recovery remains blocked.

For a future SWD inspection, save flash before any erase or programming. Inspect
the headers at `0x08010000` and `0x08047800`, both complete slots and their
trailers, and the 4096-byte NVS area at `0x0807f000`. Check flash size and bank
configuration against the installed bootloader's partition map. The application's
generated map and STM32G474 driver place NVS after slot 1; their source audit
found no overlapping erase range. This does not establish the actual flash
contents or the partition map of the bootloader installed on this board.

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

The apply operation must establish all of the following before erasure:

1. The application OTA command is explicitly unsupported and the standard
   image service responds on the selected USB interface.
2. Slot 0 is active and confirmed, with the original hash recorded for that
   board. Slot 1 contains exactly the staged campaign hash and is not active.
3. The repair artifact matches its signed-image manifest and generated guards.

Only slot 1 is erased. The client rereads the slots, verifies that slot 0 is
unchanged and slot 1 is absent, uploads the repair image with bounded progress,
and verifies its exact hash before requesting reset. It never confirms an image
through the bootloader.

The repair program checks the board identity, retained original image and
matching local journal (`VALID`, commit ID zero) before modifying OTA metadata.
It retains a verified recovery marker while removing the failed campaign's
keys, allowing an interrupted repair to resume. Role, calibration and other
application metadata are preserved. It explicitly confirms its own image only
after the repair preconditions and local safety checks pass.

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
The USB repair procedure must additionally pass the actual bootloader slot
checks and report repair success on each board; building the utility alone does
not establish hardware recovery.
