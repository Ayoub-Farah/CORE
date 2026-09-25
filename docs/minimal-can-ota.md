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

The host retries a silent status request up to three times within the response
timeout, without sending console bytes or resetting the board. A missing
`OTAR2` response does not identify a legacy application or bootloader, and is
not a reason to initialize a board with an existing campaign. Close other
serial tools and retry **Check connected board**. A returned `FAILED` state is
the campaign result, not a USB detection failure.

Receiver status also reports `deferred_arm_qualified`, the compiled activation
gate. An unqualified receiver now advertises unavailable and rejects PREPARE
before reserving a campaign or writing a journal. Its confirmed USB application
can still initialize, reporting `DISABLED_IN_RECEIVER_BUILD`. Generated artifact
profiles record the effective `receiver_can_update_enabled` setting; the host
rejects a candidate with that setting false before starting a campaign.

For a dedicated LED/OTA test bench, the local `src/app.ini` can explicitly enable
the path under `[env:OTA]` while preserving its profile arguments:

```ini
[env:OTA]
board_build.zephyr.cmake_extra_args =
    -DBUILD_ENV_NAME=OTA
    -DOWNTECH_BUILD_PROFILE=ota_receiver
    -DCONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED=y
```

This enables a bench test; it does not provide the bootloader qualification
evidence described in `ota-deferred-arm-qualification.md`. The shipped profile
remains disabled. Changing only the future CAN artifact cannot enable the
receiver already running: the first enabled receiver must be installed by USB.

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
outside PlatformIO's build-clean directories. The assistant archives campaigns
under `ota-artifacts/operations/`; direct CLI logs default to `ota-journals/`.
These generated directories are Git-ignored: archive the exact
artifacts, manifests, configuration, map and journal before another build.

## Distribute only to receiver boards

Install the dedicated Lead first, then connect it by USB. The **Update CAN
receiver boards** task opens the assistant, including when launched from the
terminal. If several boards are connected by USB, select the Lead in the board
dialog. Enter the number of receivers to update, **excluding the Lead**.
The assistant selects the USB board and receiver count interactively; it does
not read `custom_ota_serial`, `custom_ota_expected_ids` or
`custom_ota_expected_count` from `src/app.ini` for this task.

```sh
pio run -e USB_LEAD -t lead_update
```

Discovery must find exactly that count, and every receiver must be compatible
and available. The assistant freezes the discovered identities for the campaign.
To require specific receiver EUIs before starting, use the direct CLI with
`--expected-id` once per receiver. The CLI also takes its selection from explicit
arguments, rather than `src/app.ini`:

```sh
# Distribute a previously built or archived exact artifact:
python owntech/tools/lead_update.py --image ota-artifacts/OTA/firmware.can.bin \
  --manifest ota-artifacts/OTA/firmware.can.json --serial LEAD_USB_SERIAL \
  --expected-id RECEIVER_EUI_1 --expected-id RECEIVER_EUI_2
```

Alternatively, use `--expected-count 2` when a receiver count is sufficient.
The expected IDs and count always exclude the Lead. With exact IDs, an absent
or unexpected receiver causes discovery to fail before the campaign starts.

The assistant explicitly builds the
`OTA` environment and distributes its compact receiver artifact. It does not
upload to, reset, or replace the Lead. The previous `--receiver-absent` campaign
bootstrap is refused; install the Lead in its separate USB workflow.
The assistant saves its campaign log and firmware under
`ota-artifacts/operations/`; direct CLI logs default to `ota-journals/` unless
`--journal` selects another path.

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

Older receivers could turn a refusal before preparation into `FAILED/-7` and
persist a v2 journal without entering maintenance. The explicit
`--preprepare-receiver-only` repair covers that case separately from compact
transfer recovery. It requires exactly one receiver, frozen original/candidate
hashes and identity, zero accepted bytes/pass, the preparation-entry event only,
and no validation/commit proof. The secondary must be absent at USB inspection,
or match an explicitly supplied `--expected-secondary-hash` from a previous
read-only inspection. That option accepts only an inactive, unconfirmed,
nonpending old backup; the exact slots are reread immediately before erasure.
The helper
requires a matching FAILED or ABORTED journal with zero commit ID, no fleet journal, and
absent or valid-false maintenance. Corrupt records, true maintenance, an unexpected
secondary, and postcommit states remain refused. Its distinct durable marker
permits resuming interrupted cleanup of only the OTA keys.
The old client sends ABORT after a transfer error; its receiver can retain the
original error in the USB display even after persisting ABORTED. Both terminal
states are therefore covered by the same no-transfer proof.

```sh
python owntech/tools/prepare_ota_recovery.py --journal PATH_TO_FAILED_CAMPAIGN \
  --preprepare-receiver-only --output-dir .pio/ota-preprepare-recovery-config
# Set custom_ota_recovery_config = .pio/ota-preprepare-recovery-config
# under [env:OTA_RECOVERY] in the local src/app.ini, then:
pio run -e OTA_RECOVERY -t mcuboot-image
# Enter the receiver's existing bootloader using BOOT + RESET.
python owntech/tools/recover_ota.py --preprepare-receiver-only --inspect \
  --config .pio/ota-preprepare-recovery-config/owntech_ota_recovery_config.json \
  --image ota-artifacts/OTA_RECOVERY/firmware.mcuboot.bin \
  --manifest ota-artifacts/OTA_RECOVERY/firmware.mcuboot.json \
  --serial RECEIVER_USB_SERIAL --identity RECEIVER_EUI
```

After inspection succeeds, `--apply --mcumgr PATH` installs the scoped helper.
Wait for `RECOVERED rc=0` with the expected EUI and confirmation. The helper keeps
outputs inhibited; it is not the receiver application. Enter the bootloader
again, verify the exact confirmed helper in primary and the original receiver
nonpending in secondary, then install the intended receiver using the explicit
USB bootloader provisioning path. Verify its hash, local health, confirmation
and activation gate before returning to `lead_update`.

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


CAN/OTA starts in Core threads without any application health or maintenance
callback. The OTA profiles run `main` at priority 12, below OTA (7) and CAN/SDK
(10); configuration rejects a main priority that could starve these services.
A returning, sleeping or CPU-busy `main` therefore needs no OTA servicing call,
provided it leaves interrupts and scheduling operational. Core inhibits all
HRTIM outputs and shield power GPIOs directly, including before PWM setup.
Service health validates CAN startup, storage, image identity and safe outputs;
it does not certify that MMC control or synchronization works. MMC keeps its
own power permission and rearm logic. NVS application writes return `-EBUSY`
while inhibited or busy, and whole-NVS erase returns `-EPERM` in OTA builds.

This is independence from the application's lifecycle, not memory or CPU
isolation: a HardFault, blocked interrupts/scheduler, higher-priority busy task,
or direct reconfiguration of reserved peripherals can still break the service.
Recovery from those cases needs an independently reachable CAN bootloader and
a qualified reset/watchdog path. These are not supplied by this change. See the
[incident and current recommendations](../Idea/recommandations_ota_mmc.md).

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
