# Minimal receiver and dedicated Lead — protocol v2

This implementation follows [the minimal OTA plan](../Idea/minimal_can_ota_implementation_report.md).
It is a software implementation, not a hardware qualification of MMC_ANA,
MCUboot power-loss behavior, RAM margins or real-time performance.
See the [software validation and measured MMC sizes](minimal-can-ota-validation.md),
the [MMC safety audit](ota-mmc-safety-audit.md), and the
[deferred-arm qualification requirements](ota-deferred-arm-qualification.md).

## Build and install separately

`OTA` builds the user's unchanged `src/main.cpp` with `ota_receiver.conf`.
`USB_LEAD` builds `owntech/lead/main.cpp` with `ota_lead.conf`; it does not build
the user's application entry point or apply its `src/app.conf`/`src/app.overlay`.
Build identities include the selected class, profile, effective settings and
application sources. Receiver and Lead build IDs are intentionally different.

Connect one board by USB with CAN disconnected. Select a stable USB serial with
`custom_ota_serial` (or `board_id`) if more than one board is connected. Close
the serial monitor. Initial installation from a legacy single-CDC application:

```sh
pio run -e OTA -t ota_init
# On the separate board which will remain the Lead:
pio run -e USB_LEAD -t ota_init
```

`ota_init` first checks for the safe 2400-baud v2 status without sending RX
bytes. A recognized different class/image is refused before reset. Only the
explicit legacy case enters the existing bootloader at 1200 baud before any
SMP probe. It never installs a bootloader or generates a signing key. A healthy
receiver confirms locally without requiring another node to acknowledge CAN.
An empty bootloader image list, unconfirmed active image or ambiguous device
stops the existing guarded uploader. BOOT + RESET may be necessary; the explicit
`provision_ota.py --bootloader` mode then checks that image service without
another software reset. Supply `--image-class receiver` or `--image-class lead`.

Ordinary `upload` verifies an already installed matching application without
resetting it. An unknown single-CDC device is not evidence authorizing upload:
select the explicit legacy or bootloader path. A different live class is refused.
`Upload and Monitor` concerns this USB installation only, never a fleet campaign.
The ordinary `USB` workflow does not promise to retain the OTA receiver.

The receiver has no application SMP server or second CDC. A change to 2400 baud
on its existing console requests one bounded `OTAR2 {JSON}` response, without
injecting console RX bytes. The PC restores 115200 baud and spaces requests by
at least 300 ms. Status includes identity, class, version/build ID, active hash,
confirmation, local/CAN health, maintenance and storage availability. Close
Scope/console consumers during this check; saturated/interleaved console output
does not count as a valid status response.

## Two artifacts, two contracts

Each OTA build checks the existing signed `firmware.mcuboot.bin` USB installation
artifact, then signs the raw application again with the same imgtool and key,
without `--pad`, to produce `firmware.can.bin`. It never truncates a padded file.
Both images must report the same MCUboot execution hash. The CAN file ends at
the final TLV and contains no activation trailer or alignment padding.

Both manifests declare schema/protocol 2 and class `receiver` or `lead`.
The class also resides in custom protected MCUboot TLV `0xA0` (ASCII): it is
covered by the execution hash and signature. The PC rejects a manifest relabeling
a Lead binary as a receiver, a missing/duplicate class, or an unprotected class.
USB format is `mcuboot-usb-padded`; CAN format is `mcuboot-compact` with
`activation_trailer: false`. Transfer SHA256 covers the exact respective file;
MCUboot SHA256 covers its header, program and protected TLVs. Structural checks
do not claim cryptographic signature acceptance; the installed bootloader
remains responsible for signature verification.

Snapshots are written under `ota-artifacts/OTA/` and `ota-artifacts/USB_LEAD/`,
outside PlatformIO's build-clean directories. Campaign logs go to
`ota-journals/`. These generated directories are Git-ignored: archive the exact
artifacts, manifests, configuration, map and journal before another build.

## Distribute only to receiver boards

Install the dedicated Lead first. Configure `custom_ota_expected_ids` as the
receiver EUI-64 list, or `custom_ota_expected_count` as the receiver count.
**Neither includes the Lead.** All expected boards must be discovered, compatible
and available; an absent or unexpected identity cannot be silently removed.
The local `src/app.ini` list inherited from v1 is preserved for operator review;
remove the dedicated Lead's EUI explicitly before starting a v2 campaign.

```sh
pio run -e USB_LEAD -t lead_update
# Or distribute a previously archived exact artifact:
python owntech/tools/lead_update.py --image ota-artifacts/OTA/firmware.can.bin \
  --serial LEAD_USB_SERIAL --expected-count 2
```

The task is named **Update CAN receiver boards**. It explicitly builds the
`OTA` environment and distributes its compact receiver artifact. It does not
upload to, reset, or replace the Lead. The previous `--receiver-absent` campaign
bootstrap is refused; install the Lead in its separate USB workflow.

`stage_begin` binds the v2 receiver manifest and campaign to a PC source;
`stage_end` changes `SOURCE_OPEN` to `SOURCE_READY` without writing a Lead slot.
After `start`, status reports one `source_campaign/source_offset/source_length`
credit. The PC supplies exactly that block with `stage_data`, at most 256 bytes.
Older offsets are permitted for retransmission. The Lead retains only a bounded
RAM window. The PC keeps the immutable source bytes for the whole campaign.
An absent PC times out the source and cannot cause a commit.

Boards prepare their secondary slot under persisted maintenance, receive and
verify compact bytes, then report `VALID`. Only the collective validation
barrier permits `COMMIT`; activation is armed separately. Reconciliation checks
every frozen receiver's expected image, health and confirmation before releasing
maintenance. A common broadcast does not imply an atomic simultaneous reboot.

```sh
python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL --status
python owntech/tools/lead_update.py --serial LEAD_USB_SERIAL \
  --reconcile-journal ota-journals/campaign-CAMPAIGN_ID.jsonl
```

## Recovery and acceptance still requiring the bench

Keep the journal after any interruption. A failed transfer retains maintenance;
do not interpret `ABORT` as proof that an already armed image was disarmed.
After commit, read the real boot state and reconcile the original frozen roster.
The guarded `OTA_RECOVERY` utility preserves its legacy v1 modes and adds an
explicit `--compact-receiver-only` mode. This mode requires a frozen receiver
roster excluding the Lead, a source/START followed by FAILED or ABORTED, and no
commit intent/request, reboot or postboot proof. It binds campaign, Lead EUI,
receiver EUI, original active hash, both candidate hashes and image size.

```sh
python owntech/tools/prepare_ota_recovery.py --journal ota-journals/campaign-ID.jsonl \
  --compact-receiver-only
pio run -e OTA_RECOVERY -t mcuboot-image
# Enter the selected receiver's existing bootloader physically (BOOT + RESET).
python owntech/tools/recover_ota.py --compact-receiver-only --inspect \
  --config .pio/ota-recovery-config/owntech_ota_recovery_config.json \
  --image ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin \
  --manifest ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json \
  --serial RECEIVER_USB_SERIAL --identity RECEIVER_EUI
```

After a successful inspection, the same command with `--apply --mcumgr PATH`
authorizes the explicit repair. The host requires the exact confirmed original
primary and either no secondary or the exact nonpending candidate; pending,
wrong or unconfirmed images stop before erase. It erases only that proven
secondary, uploads the scoped repair application and requests one verified
reset. On-device checks require the v2 local journal's matching hashes and
identity, zero commit ID, durable maintenance, a prearming state, and no fleet
journal. Commit intent/armed/rebooted states are refused. No automatic NVS-wide
erase or ambiguous upload is provided. This software path still needs the
power-loss/bootloader bench qualification below; it is not a postcommit repair.
A bootloader cannot report application image class: explicit physical entry,
selected identity, generated manifest and these slot/journal guards are required.


The unchanged MMC_ANA application still needs a qualified Core maintenance and
health adapter. Default weak callbacks fail closed. A successful link is not
proof that direct hardware power commands, RS485 restart requests, initialization
health or protection supervision are safe. No MMC_ANA source changes are made.

Before operational use, complete and archive the plan's acceptance matrix:

- USB-only installation of receiver and Lead, then two complete CAN campaigns.
- Exact signed-image size, receiver map without coordinator/fleet tables, RAM
  including Scope and all allocations, and measured worker stack high-water marks.
- At least 8192 bytes real remaining RAM under representative MMC load, after
  allocation overhead; the provisional OTA increment is 25000–28000 bytes.
- Flash/ECC geometry and power cuts during erase, data, validation, commit
  intent, arming and swap; the activation trailer must remain physically erased
  before commit, not merely read back as `FF`.
- Lost/duplicated/out-of-order blocks, queue overflow, unavailable NVS/rollback
  slot, wrong hashes/classes/signature, disappeared PC/Lead and corrupt journals.
- FDCAN2 PB5/PB6 at classic 500 kbit/s, HRTIM PB2/PB1 unchanged, measured control
  jitter and RS485 loss with CAN silent and under discovery/hostile traffic.

The design's Flash/RAM targets and historical prototype trials are not final
measurements or qualification of this implementation.
