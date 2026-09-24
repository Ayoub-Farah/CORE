# USB Lead campaign client

`USB_LEAD` / `lead_update` builds the existing PlatformIO `mcuboot-image` target,
inspects its **unchanged** `firmware.mcuboot.bin`, stages every byte on the USB
Lead, distributes it through the application CAN service, and checks every
frozen identity after reboot. It does not install or replace a bootloader.

The same application must already provide the OTA receiver on every follower.
The prototype assumes no unexpected reset or power interruption between staging
and the final campaign reboot. The existing padded artifact contains activation
magic: aborting a campaign does **not** disarm it. The installed bootloader's
signature acceptance, rollback behavior and useful capacity still need hardware
qualification.

## One PlatformIO task

Configure an exact inventory, including the Lead, in the selected environment or
`src/app.ini`. A total expected count may be used instead of identities on a
controlled bench; discovered identities are then frozen before any erase.

```ini
[env:USB_LEAD]
custom_ota_serial = YOUR_USB_SERIAL_NUMBER
custom_ota_expected_ids = 0102030405060708, 1112131415161718, 2122232425262728
; Alternatively: custom_ota_expected_count = 3
custom_ota_timeout = 180
```

```sh
pio run -e USB_LEAD -t lead_update
```

The Project Task is named **Update Lead and CAN fleet**. Select it in PlatformIO,
or use the command above. `OTA_BLINK_A` and `OTA_BLINK_B` can use the same task
for the versioned LED example. Keep the target image's service enabled for the
next campaign. Close the serial monitor first: the client is the only reader
of the SMP CDC port.

An explicit `custom_ota_serial` (or existing `board_id`) never falls back to a
different board. Without it, exactly one OwnTech USB board with a stable serial
number must be present. Its CDC interfaces are probed read-only; exactly one
compatible SMP service must reply. Reconnection uses the same serial number and verifies
the firmware's EUI-64 again. An occupied port, ambiguous device or incompatible
response causes a bounded failure.

If an operator has established that the selected Lead lacks the application
receiver, set `custom_ota_receiver_absent = true` for its initial provisioning.
This is an explicit assertion, not a conclusion inferred from a timeout. The
existing `owntech/third_party/mcumgr` executable must be installed (or set
`custom_ota_mcumgr` to its path). Only after an unanswered probe with that
assertion does the task touch 1200 baud, upload the application using the existing
MCUmgr tool, send its initial reset, and reconnect. A compatible receiver skips
this path even when its version differs. Remove the assertion after provisioning.
No bootloader download, installation target or new signing key is involved.
By default the initial application is the campaign image. To observe a complete
A → B transition on the Lead too, build `OTA_BLINK_A` first and set
`custom_ota_bootstrap_image = .pio/build/OTA_BLINK_A/firmware.mcuboot.bin` while
running the B task. The standalone equivalent is `--bootstrap-image PATH`.
The initial image is independently inspected with the same compatibility/size
profile; its version can differ from the campaign target. It must provide the
compatible OTA service. After its initial reboot, the task still stages the
entire B artifact before CAN distribution.
When an old application exposes multiple CDC interfaces and none replies to SMP,
also set `custom_ota_port` (or `--port`) to its console port for the initial 1200
baud touch; the client will not choose an arbitrary interface for bootstrap.

## Initial A image on the followers

Prepare the shared CAN bus with the intended Lead and two followers before the
receiver's first health initialization. Use the qualified wiring, termination
and power arrangement for the bench. Build A once:

```sh
pio run -e OTA_BLINK_A
```

Provision each follower individually over USB, selecting its exact USB serial.
Do not run `lead_update` on a follower for initial provisioning: that task selects
the USB board's persisted role as Lead. A fresh application's default role is
follower. The following Python example uses the existing application's 1200-baud
bootloader entry, the existing MCUmgr application upload, and one initial reset.
It never writes or replaces the bootloader. Use it only after establishing that
this particular board lacks the OTA receiver.

```python
from pathlib import Path
import sys
sys.path.insert(0, "owntech/tools")
from lead_update import USBConnection
from smp_transport import ReceiverProbeTimeout
from ota_artifact import inspect_image

image = Path(".pio/build/OTA_BLINK_A/firmware.mcuboot.bin")
inspect_image(image.read_bytes(), version="1.0.0")
link = USBConnection(serial_number="EXACT_FOLLOWER_USB_SERIAL")
# For an old application with multiple CDC ports, also pass device="COM9"
# (its identified console port). A single-interface board needs no port hint.
try:
    try:
        smp = link.connect()  # bounded probe of this physical board only
    except ReceiverProbeTimeout:
        # Receiver absence was established by the operator before this script.
        smp = link.bootstrap(image, Path("owntech/third_party/mcumgr.exe"))
    info = smp.request("info")
    if info["version"] not in ("1.0.0", "1.0.0+0"):
        raise RuntimeError("An existing compatible receiver is running another version; no blind reinstall")
    if info["role"] != "follower":
        smp.request("set_role", {"role": "follower"})
    print(smp.request("info"))  # record EUI-64, version A, and follower role
finally:
    if link.transport:
        link.transport.close()
```

On Linux/macOS, use the corresponding existing `mcumgr` / `mcumgr-mac` executable
path. An occupied port or incompatible framed reply fails without bootstrap.
Repeat for the second follower and retain both EUI-64s. To start the Lead on A
too, use the same initial application provisioning procedure on its exact USB
serial; leaving it in the default follower role is intentional. Then run the B
`lead_update` task with USB connected to that Lead and all three expected EUI-64s.
The task persists the Lead role, verifies the complete inventory, stages B and
updates all three boards. Initial application resets occur before this campaign.

## Artifact and limits

The inspector uses the Python standard library:

Ordinary builds also run this check immediately after the existing signer and
write the adjacent `.json` manifest. Invalid padding/structure or useful content
over the separate capacity limit fails the build before upload.

```sh
python owntech/scripts/ota_artifact.py inspect .pio/build/USB_LEAD/firmware.mcuboot.bin --output firmware.mcuboot.json
```

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
python owntech/tools/lead_update.py --image .pio/build/USB_LEAD/firmware.mcuboot.bin --build-id ota-blink-B --serial YOUR_USB_SERIAL_NUMBER --expected-count 3
python owntech/tools/lead_update.py --status --serial YOUR_USB_SERIAL_NUMBER
python owntech/tools/lead_update.py --reconcile-journal .pio/build/USB_LEAD/campaign-ID.jsonl
python owntech/tools/lead_update.py --abort-journal .pio/build/USB_LEAD/campaign-ID.jsonl
```

`--status` is a read-only snapshot: it calls only `info` and paginated `status`.
It requires neither an image nor an expected fleet and performs no discovery,
role selection, bootstrap, upload or reset. Run it while the campaign client is
closed because each USB port has one reader.

Each campaign creates or verifies `firmware.mcuboot.json` and writes a timestamped
JSONL journal in the build directory. The journal contains USB serial, Lead EUI-64, immutable
target set, version/hash domains, transitions and errors. Recovery reloads this
set from the journal; it does not substitute newly discovered devices. Partial
image writes are never resumed after a reset. An aborted or failed campaign
keeps maintenance/diagnostics; follow the board's recovery procedure before
attempting a new erase.

For direct invocation, supply the build ID compiled into the application (the
example uses `ota-blink-B`). When an adjacent build manifest already exists, the
client verifies it against the image and reuses its build metadata. A mismatched
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
safety. The one-Lead/two-follower blink A → B bench test remains necessary.
