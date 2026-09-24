# Collective ThingSet/CAN OTA prototype

Implementation reference: [the preserved design report](../Idea/thingset_can_ota_implementation_report.md).
Operator commands and recovery: [ota-client.md](ota-client.md).

This implementation targets SPIN 1.2.0 / TWIST 1.4.2, one classic CAN bus at
500 kbit/s, local route zero. A single application contains the participant,
coordinator and dedicated USB CDC/SMP endpoint. The Lead role is stored in the
existing Core NVS; it is not a different firmware image. Every follower must
first receive this application individually with its existing bootloader kept.

## Build and artifact contract

`USB` remains the ordinary application environment. `CAN_BASELINE` enables
ThingSet alone. `OTA_BLINK_A` builds version `1.0.0+0`, build `ota-blink-A`, LED
1 Hz. `USB_LEAD` and `OTA_BLINK_B` build `1.0.1+0`, `ota-blink-B`, LED 2 Hz.
Both variants retain the complete update service. Public LED calls are routed
through the service's sole LED writer, including for ordinary applications.

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
configuration or downloaded library structure changes. Use the preserved A
artifact for initial provisioning while building B.

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

For the power-off blink example, default maintenance and local-health hooks are
provided. Other applications must implement `owntech_ota_enter_maintenance()`
and `owntech_ota_check_health()` for their electrical/control behavior; their
defaults fail closed. These hooks must return within the configured health
deadline and must not stop necessary protection. Application-specific electrical
safety, zero-latency interrupt behavior and flash timing require bench tests.

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
pio run -e USB -e CAN_BASELINE -e USB_LEAD -e OTA_BLINK_A -e OTA_BLINK_B
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

The CI builds ordinary USB, CAN baseline and all three OTA environments and
archives firmware, manifests, generated configuration, devicetree and map files.
Local validation on 2026-09-24 used PlatformIO 6.2.0, ststm32 19.0.0, Zephyr
4.0.0 and GCC ARM 12.3.1. All five environments built and signed successfully;
35 host tests passed, with no skips. The OTA environments were rebuilt after
the final runtime corrections. The installed-framework native USB test measured
528 bytes for a complete status response with all twelve events, below the
1536-byte SMP buffer.

| Environment | Linker flash | Linker RAM | Signed useful bytes | Transmitted bytes |
|---|---:|---:|---:|---:|
| USB | 96,872 | 31,616 | 97,208 | 227,328 |
| CAN_BASELINE | 168,956 | 54,272 | — | 227,328 |
| USB_LEAD | 215,836 | 93,348 | 216,172 | 227,328 |
| OTA_BLINK_A | 215,836 | 93,348 | 216,172 | 227,328 |
| OTA_BLINK_B | 215,836 | 93,348 | 216,172 | 227,328 |

The OTA image leaves 5,012 bytes below the provisional useful-content limit.
The CAN baseline's useful byte count was not separately recorded. These are
linker allocations, not measured stack high-water marks. Builds retain the
original toolchain's linker RWX warning. Build logs are local
under `.pio/ota-final-build.log` and `.pio/ota-release-build.log`; the final host
run is `.pio/ota-host-tests.log`. Generated binaries and logs are not committed.

Still to qualify on a physical Lead plus two followers: wiring/termination,
installed bootloader/signature/swap behavior, blink A→B on every identity,
real packet loss and bus load, power inhibition during flash, stack high-water
marks, measured throughput/duration, rollback and partial recovery. CAN FD/BRS
is disabled in prototype profiles; its codec tests are not bus qualification.
