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

The existing PlatformIO `mcuboot-image` builder and its existing resolved
signature key produce `firmware.mcuboot.bin`. No bootloader is built, installed,
downloaded or modified by `lead_update`; no key is generated. The manifest
records the resolved signing key path and the image's public key digest, without
copying private key material. The currently resolved fallback is MCUboot's
published example RSA key, as in the original chain. Signature acceptance and
the identity of bootloaders actually installed on cards remain unqualified.

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

The OTA environments add the service to `src/main.cpp`; they do not substitute
another entry point. The repository's LED application declares its maintenance
and health callbacks explicitly in that file. Their success is appropriate to
that application, which does not start power conversion. It is not a general
assertion that another application or connected power stage is safe.

Applications must implement `owntech_ota_enter_maintenance()` and
`owntech_ota_check_health()` for their own electrical and control behavior. The
service's weak defaults fail closed. The maintenance callback must put the
application in a safe state and prevent application-specific restart paths;
required protection and supervision must remain active. The health callback
must verify the application's initialization before image confirmation. Both
callbacks must finish promptly within the configured startup/health budget.

Direct PWM and protected GPIO APIs remain subject to the service's maintenance
gate. Application-specific electrical safety, interrupt behavior and flash timing
need bench tests. Public LED calls use the service's LED owner so OTA state
indications can temporarily take priority over the application's normal pattern.

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

The generic application workflow passed all 56 host tests and the three firmware
builds on 2026-09-24 (PlatformIO 6.2.0, Zephyr 4.0.0, GNU Arm 12.3.1). Measurements
below come from the linker reports and inspected signed artifacts:

| Environment | Linker flash | Linker RAM | Signed useful bytes | Transmitted bytes |
|---|---:|---:|---:|---:|
| USB | 96872 | 31616 | 97208 | 227328 |
| OTA | 216432 | 97572 | 216768 | 227328 |
| USB_LEAD | 216432 | 97572 | 216768 | 227328 |

An actual build check changed the LED delay in `src/main.cpp` from 1000 to
750 ms, rebuilt, then restored 1000 ms and rebuilt incrementally with cached
CMake configuration. The edit changed both the embedded build ID and MCUboot
image hash; restoration reproduced their original values. The final `OTA` and
`USB_LEAD` artifacts have the same build ID and MCUboot image hash. The delay
change was only a build check; the committed application retains 1000 ms.

The useful image must remain below the provisional 221184-byte capacity. The
current OTA application has 4416 bytes left within that bound; application code
and enabled libraries share that budget with the service.
Linker RAM allocations are not measured stack high-water marks. Generated
binaries and build/test logs are local artifacts, not committed source.

Still to qualify on a physical Lead plus two followers: wiring/termination,
installed bootloader/signature/swap behavior, a visible application change on
every identity, real packet loss and bus load, power inhibition during flash,
stack high-water marks, measured throughput/duration, rollback and partial
recovery. Repeat the campaign after another application edit to establish that
the deployed image retains its receiver. CAN FD/BRS is disabled in prototype
profiles; its codec tests are not bus qualification.
