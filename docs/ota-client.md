# Update your application over USB and CAN

Both OTA environments build the application's current `src/main.cpp`. `OTA`
installs that application on one board; `USB_LEAD` sends the complete firmware
to the selected CAN fleet, including the USB-connected Lead. There is no special
Lead `main.cpp`. The same compatible firmware contains the participant,
coordinator and USB service, and keeps them available for later updates.

The ordinary `USB` and `STLink` environments remain available for their existing
workflows. Use an OTA environment for images that must receive future campaigns.
Neither OTA workflow installs or replaces the existing bootloader or generates
a new signing key.

## Install OTA support on each board

Build and upload the current application individually to every board that does
not yet run a compatible OTA service. With exactly one physical OwnTech USB
board connected, its stable USB serial is detected automatically; several CDC
interfaces belonging to that board still count as one board. To select a board
explicitly, set its USB serial in `src/app.ini`. This serial is distinct from
its CAN EUI-64:

```ini
[env:OTA]
; Optional with one board; select explicitly when several boards are connected:
; custom_ota_serial = EXACT_BOARD_USB_SERIAL
; Optional, when the old application has several CDC interfaces:
; custom_ota_port = COM9
```

```sh
pio run -e OTA -t upload
```

An explicit `custom_ota_serial` (or existing `board_id`) never falls back to
another board. An ambiguous selection is rejected without a reset or upload.
`custom_ota_port` can select the known console interface for initial bootloader
entry (`upload_port` is also accepted). Provide the existing MCUmgr executable
with `custom_ota_mcumgr` if it is not in `owntech/third_party/`.

When the receiver is absent, the task enters the board's existing USB bootloader,
waits for its standard image service to answer on the same USB serial, uploads
the application and requests one initial reset. A board already answering
`info` with SMP `rc: 8` (unsupported command) is checked for that image service
without another 1200-baud reset. The rejection alone never authorizes an upload:
a valid read-only image list is required, and a live OTA application still blocks
this installation path. When the same application
is already healthy, confirmed and idle, it verifies that state and selects the
follower role without reinstalling. An existing different application with a live
OTA service must be updated through the campaign workflow; an occupied port,
malformed response or ambiguous device does not trigger an upload. This task does
not discover a fleet or select the board as Lead. Perform initial installation
before staging a fleet update. The application's maintenance and health callbacks must be
appropriate for its power and control behavior; see [application integration](ota-implementation.md#application-integration).

The task prints the selected USB serial and the port that answers the image
probe. If the image service does not answer before `custom_ota_timeout` (30 seconds
by default), it stops before starting the upload. During upload, repeated `0 %`
lines are not progress: 20 seconds without an advance stops MCUmgr and reports
its last output. No post-upload reset is sent after an error, stall or interrupted
transfer. Close the serial monitor before retrying **OTA -> Upload**; this command
also handles a board left in the standard image service by an earlier attempt.

If a legacy application remains enumerated but produces USB write timeouts and
does not enter the bootloader at 1200 baud, use the board's BOOT and RESET buttons
to enter recovery, then retry OTA upload. See the
[OwnTech recovery procedure](https://docs.owntech.org/latest/bootloader/docs/getting_started/#recovery-mode).
On older firmware, probing an unconsumed console can block its shared USB
workqueue. The OTA profile avoids that console overflow path; the PC client
also checks the other CDC interfaces when one console is inaccessible.

Prepare the shared CAN bus and its wiring, termination and power arrangement
before the receiver's first boot. Startup health requires CAN to become ready
within its configured deadline (15 seconds in the supplied profile). An active
ACK-capable CAN peer, such as another CAN application or a bench adapter, may be
needed for address setup. Boards still running USB-only firmware are not CAN
peers. If startup health fails or the image remains unconfirmed, inspect the
status and correct the setup before starting a campaign.

After installation, check each board and retain its EUI-64:

```sh
python owntech/tools/lead_update.py --status --serial EXACT_BOARD_USB_SERIAL
```

A healthy, confirmed active image and an available secondary slot are required
for a new campaign. A board with a previous pending/test/revert image requires
reconciliation before another erase.

## Distribute the current application

Edit your application in `src/main.cpp` and its normal project configuration.
Select the Lead USB serial and exact expected inventory, including the Lead, in
`src/app.ini` or the environment's project options:

```ini
[env:USB_LEAD]
custom_ota_serial = YOUR_USB_SERIAL_NUMBER
custom_ota_expected_ids = 0102030405060708, 1112131415161718, 2122232425262728
; Alternatively on a controlled bench: custom_ota_expected_count = 3
custom_ota_timeout = 180
```

Pause power conversion/control on the whole fleet before starting the command;
real-time operation is not guaranteed during flash writes and reboot. The
application's maintenance callback must keep it paused, including when RS485
commands arrive. Outside campaigns, the supplied OTA profile disables periodic
CAN telemetry and gives CAN interrupts lower priority than HRTIM control; see
[real-time coexistence](ota-implementation.md#coexistence-with-real-time-control-outside-a-campaign).

Then run the **Update Lead and CAN fleet** Project Task:

```sh
pio run -e USB_LEAD -t lead_update
```

The task builds and signs the current application, identifies the USB board,
persists its Lead role, checks the expected inventory, stages every byte of
`firmware.mcuboot.bin`, and distributes it over CAN. It validates every selected
board before requesting the collective reset, then verifies each board's actual
image, build identity and health. The next update uses the same command after
editing the application again. A compatible receiver is reused; it is not
reinstalled on each campaign.

Build identity is derived automatically from the application sources and build
configuration. Editing `src/main.cpp` changes the identity without requiring a
manual build-ID increment. Equivalent `OTA` and `USB_LEAD` source/configuration
builds share that identity; USB serial and fleet selection do not change it. The
common default firmware version is `1.0.0`. Set
`board_build.zephyr.bootloader.app_version` consistently in both OTA environments
when a project needs semantic release versions. Version alone is not the proof of an update: the
manifest and postboot checks also use the generated build ID and image hash.

Close the serial monitor first: the campaign client is the only reader of its
SMP CDC port. An explicit `custom_ota_serial` (or existing `board_id`) never falls
back to another board. Without one, exactly one OwnTech USB board with a stable
serial must be present. Its interfaces are probed read-only; exactly one
compatible SMP service must reply. Reconnection retains the USB serial and
checks the firmware's EUI-64. An occupied port, ambiguous selection or incompatible
reply causes a bounded failure.

The task can also initialize a Lead whose receiver absence has been established
explicitly: set `custom_ota_receiver_absent = true` and provide the existing
MCUmgr executable with `custom_ota_mcumgr` if it is not in
`owntech/third_party/`. Only an unanswered probe together with that assertion
authorizes the initial 1200-baud bootloader entry, application upload and reset.
A compatible service skips this path even when its version differs. Remove the
assertion afterward. For an old application with several CDC ports, specify
`custom_ota_port` as its known console port; the task does not guess which port
to touch. `custom_ota_bootstrap_image` may select a separately built compatible
application for this initial installation; the campaign still stages and sends
the current target artifact in full afterward.

The prototype assumes no unexpected reset or power interruption between staging
and the final campaign reboot. The padded artifact already contains activation
magic. Aborting retains maintenance and stops automatic progression, but does
not disarm that trailer or cancel a reset already scheduled. Do not reset a board
once staging has started. Installed bootloader signature acceptance, rollback
behavior and useful capacity still require hardware qualification.

## Artifact and limits

The inspector uses the Python standard library. `OTA` and `USB_LEAD` builds
run this check immediately after the existing signer and
write the adjacent `.json` manifest. Invalid padding/structure or useful content
over the separate capacity limit fails the build before upload.
The same verified binary and manifest are also saved in
`.pio/ota-artifacts/<environment>/`. Use this stable snapshot when retaining a
firmware build: a first build of another environment can cause PlatformIO to remove
`.pio/build`, while the snapshot directory remains intact.

For standalone structural inspection:

```sh
python owntech/scripts/ota_artifact.py inspect .pio/ota-artifacts/USB_LEAD/firmware.mcuboot.bin
```

Retain the build-generated adjacent manifest for deployment. Structural inspection
alone cannot reconstruct the application's compiled source fingerprint.

It validates MCUboot header, TLV bounds, SHA256 TLV, signature/key TLV structure,
the erased padding and the exact terminal activation magic produced by the
existing `--pad` pipeline. It rejects compact images, altered trailers,
`--confirm` artifacts, unsupported encrypted images and over-capacity useful
content. It never truncates or signs a file.

The manifest distinguishes:

| Field | Meaning |
|---|---|
| `artifact_size` | Exact transmitted file length, currently 227328 bytes |
| `useful_size` | Header + program + protected/unprotected TLVs |
| `artifact_sha256` | SHA256 of every transmitted byte, including padding/trailer |
| `mcuboot_image_hash` | MCUboot SHA256 of header/program/protected TLVs, used after boot |
| `profile.useful_capacity` | Provisional 221184-byte useful-content bound |
| `signature.key_sha256` | Public signing-key hash in the existing artifact |
| `signature.signing_key` | Existing signing-key path resolved by the PlatformIO hook |

The default bound assumes the reported 2 KiB sectors, 8-byte alignment and
swap-move reserve. It is not proof of the configuration installed on a board.
`bootloader_id = 0x00010100` is a compatibility-profile identifier, not measured
attestation of the bootloader binary. `hardware_id = 0x01020142` identifies the
SPIN 1.2.0 / TWIST 1.4.2 profile and `layout_id = 0x00010001` its partition layout.
Use `custom_ota_profile` / `--profile` to supply a JSON override with measured
metadata. A `public_key_sha256` override additionally pins the expected key hash.
The inspector checks signature structure, not cryptographic authenticity;
`signature.verified` remains false. The bootloader performs signature acceptance.

## Direct client and recovery

The standalone client requires Python 3 and `pyserial`; PlatformIO's Python
already supplies it. CBOR and SMP framing need no extra packages.

```sh
python owntech/tools/lead_update.py --image .pio/ota-artifacts/USB_LEAD/firmware.mcuboot.bin --serial YOUR_USB_SERIAL_NUMBER --expected-count 3
python owntech/tools/lead_update.py --status --serial YOUR_USB_SERIAL_NUMBER
python owntech/tools/lead_update.py --reconcile-journal ota-journals/campaign-ID.jsonl
python owntech/tools/lead_update.py --abort-journal ota-journals/campaign-ID.jsonl
```

`--status` is a read-only snapshot: it calls only `info` and paginated `status`.
It requires neither an image nor an expected fleet and performs no discovery,
role selection, bootstrap, upload or reset. Run it while the campaign client is
closed because each USB port has one reader.

Each campaign creates or verifies the image's adjacent `firmware.mcuboot.json`
and writes a timestamped JSONL journal under `ota-journals/` in the current
project/working directory. This location survives normal PlatformIO build
cleanup; `--journal PATH` still selects an explicit journal file. The journal
contains USB serial, Lead EUI-64, immutable
target set, version/hash domains, transitions and errors. Recovery reloads this
set from the journal; it does not substitute newly discovered devices. Partial
image writes are never resumed after a reset. An aborted or failed campaign
keeps maintenance/diagnostics; follow the board's recovery procedure before
attempting a new erase.

For direct invocation, retain the adjacent manifest generated by the build.
The client verifies it against the image and reuses its automatic build identity;
no manual `--build-id` is needed in this normal workflow. A mismatched
existing manifest is rejected without overwriting it; rebuild or regenerate it
explicitly. Status and recovery do not read or rewrite the image/manifest:
recovery uses the frozen metadata embedded in the journal and appends results.
Successful reconciliation can persist release from maintenance on the devices;
use `--status` for observation alone.

During CAN work the table reports accepted offsets, queue depth, writer state,
validation, and postboot outcome separately, at most 2.5 times per second.
Lead USB progress is journaled separately from followers' accepted flash offsets.
100% accepted bytes alone cannot authorize commit: every target must report
`flash_complete`, `validated` and the exact file length, and the coordinator must
reach `ALL_VALIDATED`. After commit, every
expected identity must return with the expected active MCUboot hash and version,
build ID, local health and confirmation. Success also requires the coordinator's
`SUCCESS` phase after persistent maintenance release. A missing/wrong/unhealthy target remains visible
and produces `PARTIAL` with nonzero task exit status. No synthetic bootloader
swap percentage is displayed.

Device event history is bounded to twelve named transitions per campaign.
Each row carries `event_mask`, `event_ms[12]` (device uptime), `event_order[12]`
and its campaign ID. The PC journals newly observed events by the recorded order,
including transitions missed between polls. It keeps the device timestamps and
the PC reception timestamp separate, preserves ordering across a reboot clock
reset, and deduplicates by campaign/identity/event even after reopening a journal.
Flash/validation transitions are never invented from a snapshot or percentage.

## Application SMP contract, version 1

Group **64** is reserved for OwnTech OTA. All requests use SMP WRITE opcode 2;
responses use opcode 3. Payloads are CBOR maps with text keys, byte-string hashes
and lower-case 16-character hexadecimal EUI-64 strings. UART uses MCUmgr's
`06 09` / `04 14` base64 lines, network-order length, and CRC16-CCITT (initial 0).
The client handles definite and Zephyr indefinite CBOR maps, bounded packet
lengths, delayed sequence numbers, and rejects corrupt/mismatched replies.
Every successful reply has `rc = 0` (or no `rc`); nonzero `rc` / `err` is failure.

| ID | Command | Request / required result |
|---|---|---|
| 0 | `info` | `{}` → service `owntech-ota`, protocol 1, identity, role, capacities, available/active_confirmed/slot_available |
| 1 | `set_role` | `{role:"lead"}` or `{role:"follower"}` → persisted runtime role, rejected while busy |
| 2 | `discover` | `{}` → `phase:DISCOVERING` until complete, then inventory; repeated calls do not restart scan |
| 3 | `stage_begin` | campaign uint64, protocol, artifact_size, useful_size, both 32-byte hashes, version, build_id and compatibility IDs → `STAGING` or asynchronous status |
| 4 | `stage_data` | offset and data (256 bytes maximum from this client) → exact next accepted offset |
| 5 | `stage_end` | `{}` → `STAGED` only after writer flush/close, full reread/hash and structural validation |
| 6 | `start` | campaign and explicit target EUI-64 list including Lead |
| 7 | `status` | `{}` or `{index:n}` → phase/pass, target_count, targets |
| 8 | `commit` | campaign → reset authorized only after all frozen targets validate |
| 9 | `abort` | campaign → stop transfer/automatic reset; no implicit trailer disarm |
| 10 | `reconcile` | campaign, frozen targets and expected MCUboot hash → actual postboot target records |

Inventory/status/reconcile may return pages: `target_count` is the total and
`{index:n}` returns the corresponding row in `targets`. The client requests all
pages. Inventory rows require identity, address, available, compatible and role.
Status rows include offset, queue_depth, state, error, flash_complete and validated.
Postboot rows include actual version, build_id, mcuboot_image_hash, healthy and
confirmed. Response phases are strings. Long operations return acceptance or
in-progress state first and are polled with bounded deadlines.

`stage_data` is deliberately used instead of the standard image-upload shortcut:
it invokes the application slot owner/writer and forces a real secondary-slot
copy even if that image is already active on the Lead. Standard image management
is disabled (`CONFIG_MCUMGR_GRP_IMG=n` and `CONFIG_MCUMGR_GRP_OS=n`): standard
upload/erase/test/confirm/reset handlers are absent from the application. Its
dispatcher also rejects all groups except 64. The optional bootstrap uses the
standard image/reset commands only in the existing bootloader.

## Host validation

```sh
python -m unittest discover -s tests/ota -p "test_pc_*.py" -v
```

Host tests cover artifact domains/capacities/corruption, CBOR/framing, sequencing,
inventory mismatch, pagination, writer completion before commit, partial
postboot results and strict USB selection. They do not qualify CAN throughput,
flash timing, installed bootloader behavior, USB re-enumeration or electrical
safety. A physical test must demonstrate an application change on one Lead and
at least two followers, then another campaign using the retained service.
