"""Desktop recovery of an interrupted compact receiver campaign, one USB board at a time."""
import configparser
from datetime import datetime, timezone
import math
from pathlib import Path
import re
import shutil
import tempfile
import time

from bootloader_upload import upload_image
from lead_update import CampaignError, Journal, ReceiverStatus, identity, prepare_manifest, select_port
from prepare_ota_recovery import generate, recovery_config
from recover_ota import recover, verify_inputs, verify_slots
from smp_transport import CommandError, SerialSMP, TransportError
from provision_ota import _verify_image, _provision_readiness
import transition_ota_usb as transition


def require(condition, message):
    if not condition:
        raise CampaignError(message)


def collect_result(serial_number, target, output, *, enumerate_ports, timeout=60,
                   console_factory=None, clock=time.monotonic, sleep=time.sleep):
    """Read the helper console without commands, resets or baud-rate triggers."""
    require(isinstance(timeout, (int, float)) and not isinstance(timeout, bool)
            and math.isfinite(timeout) and 0 < timeout <= 120, "Recovery result timeout must be within 0..120 seconds")
    if console_factory is None:
        import serial
        console_factory = serial.Serial
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    diagnostic = output.with_name(output.stem + "-console.log")
    pattern = re.compile(rb"OTA_RECOVERY (RECOVERED|ALREADY_RECOVERED) rc=([01]) EUI=([0-9a-f]{16}) confirmed=1; outputs inhibited")
    deadline = clock() + timeout
    received = 0
    with diagnostic.open("a", encoding="utf-8") as log:
        def note(message):
            log.write(message + "\n")
            log.flush()

        note("\n%s | USB %s | EUI %s" % (datetime.now(timezone.utc).isoformat(), serial_number, target))
        print("OwnTech: waiting for the recovery result (up to %g seconds). Console log: %s" % (timeout, diagnostic), flush=True)
        while clock() < deadline:
            ports = list(enumerate_ports())
            if not any(p.vid == 0x2FE3 and p.serial_number == serial_number for p in ports):
                sleep(min(0.25, max(0, deadline - clock())))
                continue
            device = select_port(ports, serial_number).device
            try:
                with console_factory(device, baudrate=115200, timeout=0.2, write_timeout=0.2) as console:
                    note("Opened " + device + " at 115200 baud")
                    # Preserve startup output already queued by the device.
                    pending = b""
                    last_data = clock()
                    while clock() < deadline:
                        chunk = console.readline(513)
                        if not chunk:
                            if clock() - last_data >= 3:
                                note("Silent interface; reopen the same USB serial after enumeration")
                                break
                            continue
                        received += len(chunk)
                        note("RX " + repr(chunk))
                        last_data = clock()
                        pending += chunk
                        # pyserial readline may return a partial line on timeout.
                        # Keep those fragments until the actual newline arrives.
                        while b"\n" in pending:
                            raw, pending = pending.split(b"\n", 1)
                            require(len(raw) <= 512, "Recovery console line exceeds the response bound")
                            line = raw.strip()
                            # A queued startup fragment may precede the marker.
                            marker = line.find(b"OTA_RECOVERY ")
                            if marker < 0:
                                continue
                            line = line[marker:]
                            match = pattern.fullmatch(line)
                            require(match is not None and match[3].decode() == target
                                    and ((match[1] == b"RECOVERED" and match[2] == b"0")
                                         or (match[1] == b"ALREADY_RECOVERED" and match[2] == b"1")),
                                    "Recovery refused or belongs to another board: " + line.decode(errors="replace")
                                    + "\nConsole log: " + str(diagnostic))
                            select_port(enumerate_ports(), serial_number, device)
                            result = dict(identity=target, usb_serial=serial_number, line=line.decode("ascii"))
                            transition._save(output, result)
                            note("Confirmed recovery result saved")
                            return result
                        require(len(pending) <= 512, "Recovery console line exceeds the response bound")
            except OSError as error:
                # Windows can retain the pre-reset COM interface briefly, then
                # disconnect it. Retry only observation of the same USB serial.
                note("USB read/open error: " + str(error))
            sleep(min(0.25, max(0, deadline - clock())))
        note("Timed out; received %d bytes" % received)
    detail = "No console bytes received." if not received else "Console data received, but no complete recovery result."
    next_step = ("Use Inspect recovery boot state with this archive and follow its BOOT + RESET prompt."
                 if not received else "Close Serial Monitor/Scope and preserve the console log for diagnosis.")
    raise CampaignError("No confirmed recovery result. " + detail + "\n" + next_step
                        + " No upload or reset was retried.\nConsole log: " + str(diagnostic))


def restore_receiver(serial_number, target, helper, receiver, data, log_path, mcumgr, *,
                     enumerate_ports, transport_factory=SerialSMP, uploader=upload_image):
    """Restore only from the exact confirmed helper and original nonpending backup."""
    device = select_port(enumerate_ports(), serial_number).device
    journal = Journal(log_path, 0)
    transport = None

    def check_port():
        select_port(enumerate_ports(), serial_number, device)

    def open_bootloader():
        check_port()
        connection = transport_factory(device, timeout=10)
        try:
            try:
                connection.request("info")
            except CommandError as error:
                require(error.unsupported, "The bootloader must report the OTA service as unsupported")
            else:
                raise CampaignError("Enter the existing bootloader using BOOT + RESET first")
        except Exception:
            connection.close()
            raise
        return connection

    def state(event):
        value = transport.image_state()
        journal.emit(event, target, usb_serial=serial_number, image_state=value)
        return value

    try:
        require(Path(mcumgr).is_file(), "The existing mcumgr uploader was not found")
        transport = open_bootloader()
        initial = state("RESTORE_INSPECT")
        primary = verify_slots(initial, helper["mcuboot_image_hash"], receiver["mcuboot_image_hash"],
                               secondary_pending=False)
        check_port()
        require(state("RESTORE_BEFORE_WRITE") == initial, "Image state changed before restoration")
        # Prove the expected secondary-slot image service before any upload.
        journal.emit("RESTORE_ERASE_REQUEST", target, slot=1)
        transport._request(2, 1, 5, "prepare receiver secondary slot", {"slot": 1})
        verify_slots(state("RESTORE_AFTER_ERASE"), helper["mcuboot_image_hash"], primary=primary)
        transport.close()
        transport = None
        check_port()
        with tempfile.TemporaryDirectory(prefix="owntech-receiver-restore-") as temporary:
            snapshot = Path(temporary) / "firmware.mcuboot.bin"
            snapshot.write_bytes(data)
            base = [str(mcumgr), "--conntype", "serial", "--connstring",
                    "dev=%s,baud=115200,mtu=128" % device, "--timeout", "10", "--tries", "1"]
            journal.emit("RESTORE_UPLOAD_REQUEST", target, artifact_sha256=receiver["artifact_sha256"])
            uploader(base, snapshot)
        transport = open_bootloader()
        transition._candidate(state("RESTORE_AFTER_UPLOAD"), primary, receiver)
        check_port()
        journal.emit("RESTORE_RESET_REQUEST", target)
        transport._request(2, 0, 5, "boot verified receiver", {})
        journal.emit("RESTORE_RESET_ACCEPTED", target)
    except Exception as error:
        journal.emit("RESTORE_STOPPED", target, error=str(error))
        raise
    finally:
        if transport:
            transport.close()
        journal.close()


def verify_restored_receiver(serial_number, target, manifest, *, enumerate_ports, timeout=60,
                             receiver_factory=ReceiverStatus, clock=time.monotonic, sleep=time.sleep):
    """Wait for the same receiver after reset, using only its read-only USB status."""
    require(isinstance(timeout, (int, float)) and not isinstance(timeout, bool)
            and math.isfinite(timeout) and 0 < timeout <= 120, "Receiver startup timeout must be within 0..120 seconds")
    deadline = clock() + timeout
    last = "USB serial not yet enumerated"
    print("OwnTech: waiting for restored receiver USB %s (up to %g seconds)" % (serial_number, timeout), flush=True)
    while clock() < deadline:
        ports = list(enumerate_ports())
        matching = [p for p in ports if p.vid == 0x2FE3 and p.serial_number == serial_number]
        if matching:
            # A receiver has one CDC. Multiple matching interfaces are a real
            # ambiguity, unlike the temporary absence after its verified reset.
            device = select_port(ports, serial_number).device
            transport = None
            try:
                transport = receiver_factory(device, timeout=min(2, max(0.01, deadline - clock())))
                info = transport.request("info")
            except (OSError, TransportError) as error:
                last = str(error)
            else:
                current = [p for p in enumerate_ports() if p.vid == 0x2FE3 and p.serial_number == serial_number]
                if not current or (len(current) == 1 and current[0].device != device):
                    last = "USB re-enumerated during the status read"
                else:
                    select_port(current, serial_number)
                    require(info.get("service") == "owntech-ota" and info.get("protocol") == 2
                            and info.get("image_class") == "receiver", "Unexpected service after receiver restoration")
                    if info.get("phase") == "BOOT":
                        last = "receiver is still starting"
                    else:
                        require(identity(info.get("identity")) == target, "Restored receiver EUI differs from selected board")
                        _verify_image(info, manifest)
                        require(info.get("maintenance") is False, "Restored receiver is still in maintenance")
                        require(info.get("phase") in ("IDLE", "WAITING_CAN"),
                                "Restored receiver did not reach an idle state: " + str(info.get("phase")))
                        if _provision_readiness(info) in ("READY", "WAITING_FOR_PEER"):
                            return dict(result="OTA_VERIFIED", identity=target, info=info)
                        last = "receiver is not yet confirmed and ready"
            finally:
                if transport:
                    transport.close()
        else:
            last = "USB serial not yet enumerated"
        sleep(min(0.25, max(0, deadline - clock())))
    raise CampaignError("Restored receiver %s did not become ready within %g seconds: %s. "
                        "Use Finish receiver recovery with the same archive; no upload or reset was repeated."
                        % (serial_number, timeout, last))


def classify_boot_state(image_state, helper_hash, receiver_hash):
    """Describe the observed slots; none of these outcomes authorizes a write."""
    slots = transition._slots(image_state)
    require(0 in slots, "Bootloader did not report a primary image")
    primary, secondary = slots[0], slots.get(1)
    if primary["pending"]:
        return "UNEXPECTED", "Primary image reports a pending state. Preserve this snapshot for diagnosis."
    original = primary["hash"].hex() == receiver_hash and primary["confirmed"]
    if original and secondary is None and primary["active"]:
        return "ORIGINAL_ONLY", "Only the confirmed original receiver image is recognized. No recovery image is listed."
    if original and secondary and secondary["hash"].hex() == helper_hash and not secondary["confirmed"] and not secondary["active"]:
        if secondary["pending"]:
            return "HELPER_PENDING", "The exact recovery image is still pending in the secondary slot. Its successful startup has not been established."
        if primary["active"]:
            return "ORIGINAL_WITH_HELPER_BACKUP", "The original receiver is active and the recovery image is nonpending in secondary. This snapshot alone does not prove whether a rollback occurred."
    if (primary["hash"].hex() == helper_hash and primary["active"] and secondary
            and secondary["hash"].hex() == receiver_hash and not secondary["pending"]
            and not secondary["confirmed"] and not secondary["active"]):
        if primary["confirmed"]:
            return "HELPER_CONFIRMED", "The recovery image is active and confirmed. Cleanup completion still requires its console result; confirmation alone is insufficient."
        return "HELPER_UNCONFIRMED", "The recovery image is in primary but is not confirmed. Do not reset it to retry: a rollback may occur."
    return "UNEXPECTED", "The images or activation flags do not match an expected recovery state. Preserve this snapshot for diagnosis."


def inspect_boot_state(serial_number, target, helper, receiver, output, *, enumerate_ports, transport_factory=SerialSMP):
    """Explicit physical bootloader entry, followed only by info/image-state reads."""
    device = select_port(enumerate_ports(), serial_number).device
    transport = transport_factory(device, timeout=10)
    result = dict(identity=target, usb_serial=serial_number, port=device,
                  helper_hash=helper["mcuboot_image_hash"], receiver_hash=receiver["mcuboot_image_hash"],
                  timestamp=datetime.now(timezone.utc).isoformat())
    try:
        try:
            transport.request("info")
        except CommandError as error:
            require(error.unsupported, "Bootloader service proof must be unsupported OTA group rc=8")
        else:
            raise CampaignError("The application still replies. Use the assistant's BOOT + RESET instructions; no reset was sent.")
        image_state = transport.image_state()
        select_port(enumerate_ports(), serial_number, device)
        result["image_state"] = image_state
        # Save raw evidence even when the image list is empty or malformed.
        transition._save(output, result)
        result["diagnosis"], result["explanation"] = classify_boot_state(image_state, helper["mcuboot_image_hash"], receiver["mcuboot_image_hash"])
        transition._save(output, result)
        return result
    finally:
        transport.close()


class RecoveryWorkflow:
    def __init__(self, workflow):
        self.w = workflow
        self.ui = workflow.ui

    def choose_operation(self, serial_number, *, new_allowed):
        candidates = []
        for file in sorted(self.w.operations.glob("*/operation.json"), reverse=True):
            state = transition._json(file)
            if (state.get("kind") == "receiver-recovery" and state.get("serial") == serial_number
                    and state.get("phase") not in ("CREATED", "COMPLETE")):
                candidates.append((file.parent, state))
        if not candidates and new_allowed:
            return self.prepare(serial_number)
        options = [(str(path), path.name + " - " + state["phase"]) for path, state in candidates]
        options.append(("browse", "Select a saved receiver recovery folder"))
        choice = self.ui.choose("Receiver recovery archive", "Continue the saved recovery for this USB board.", options)
        path = self.ui.folder("Select receiver recovery folder", self.w.operations) if choice == "browse" else Path(choice)
        state = transition._json(path / "operation.json")
        require(state.get("schema_version") == 1 and state.get("kind") == "receiver-recovery"
                and state.get("serial") == serial_number and state.get("phase") != "CREATED",
                "This is not a prepared recovery archive for the connected board")
        return path, state

    def campaign(self, info):
        journals = list((self.w.project / "ota-journals").glob("*.jsonl"))
        journals += list(self.w.operations.glob("*/campaign.jsonl"))
        candidates = []
        for path in sorted(journals, reverse=True):
            try:
                config = recovery_config(path, compact_receiver_only=True)
                self.match_board(info, config)
            except (OSError, ValueError, CampaignError):
                continue
            candidates.append((str(path), path.parent.name + "/" + path.name + " - campaign " + str(config["campaign_id"])))
        candidates.append(("browse", "Select another failed CAN campaign log"))
        selected = self.ui.choose("Interrupted CAN campaign",
                                  "Select the interrupted update for this receiver. Campaigns with activation or reboot are refused.", candidates)
        path = self.ui.file("Select failed CAN campaign", self.w.operations, [("Campaign log", "*.jsonl")]) if selected == "browse" else Path(selected)
        print("OwnTech: selected recovery campaign journal: " + str(path), flush=True)
        try:
            config = recovery_config(path, compact_receiver_only=True)
            self.match_board(info, config)
        except (OSError, ValueError, CampaignError) as error:
            alternatives = [str(value) for value, _ in candidates if value != "browse"]
            guidance = ("\n\nCompatible campaign logs for this receiver:\n" + "\n".join(alternatives)) if alternatives else (
                "\n\nSelect the original failed transfer's campaign.jsonl under ota-artifacts/operations, "
                "not a workflow/recovery log or an older campaign.")
            raise CampaignError("Cannot use campaign journal:\n" + str(path) + "\n\n" + str(error) + guidance) from error
        return path

    @staticmethod
    def require_interrupted_receiver(info):
        require(info.get("image_class") == "receiver", "Connect a receiver; this recovery mode never repairs the Lead")
        require(info.get("maintenance") is True,
                "This receiver does not report retained maintenance (phase: %s, maintenance: %s). "
                "If it was already recovered, connect the next affected receiver." % (info.get("phase"), info.get("maintenance")))
        # The receiver's USB diagnostic phase gives WAITING_CAN priority over
        # its persisted OTA state. An isolated USB receiver can still need
        # recovery; the campaign and bootloader guards below remain mandatory.
        require(info.get("phase") in ("RECOVERY_REQUIRED", "ABORTED", "FAILED", "WAITING_CAN"),
                "This receiver is still in maintenance but its phase is not eligible for interrupted-transfer recovery: "
                + str(info.get("phase")))
        if info.get("phase") == "WAITING_CAN":
            require(info.get("can_ready") is False and info.get("healthy") is False
                    and type(info.get("error")) is int and info["error"] == 0,
                    "WAITING_CAN diagnostics are inconsistent; use Check connected board before recovery")

    @staticmethod
    def match_board(info, config):
        RecoveryWorkflow.require_interrupted_receiver(info)
        target = next((row for row in config["targets"] if row["identity"] == info.get("identity")), None)
        require(info.get("image_class") == "receiver" and target is not None,
                "The connected board must be a receiver in the frozen campaign; the Lead is excluded")
        require(info.get("active_confirmed") is True and info.get("local_healthy") is True
                and info.get("mcuboot_image_hash") == target["original_active_hash"],
                "The receiver is not running its exact healthy, confirmed original image")

    def receiver_image(self, info, config):
        candidates = list((self.w.project / "ota-artifacts/history/OTA").glob("*/firmware.json"))
        candidates += list((self.w.project / "ota-artifacts/OTA").glob("*.mcuboot.json"))
        for manifest in candidates:
            image = manifest.with_name("firmware.bin") if manifest.name == "firmware.json" else manifest.with_suffix(".bin")
            try:
                data, record = prepare_manifest(image, manifest, image_class="receiver", usb=True)
                self.match_receiver(info, config, record)
                return image, manifest
            except (OSError, ValueError, CampaignError):
                continue
        image, manifest = self.w.pick_image("Select the original receiver USB firmware")
        _, record = prepare_manifest(image, manifest, image_class="receiver", usb=True)
        self.match_receiver(info, config, record)
        return image, manifest

    @staticmethod
    def match_receiver(info, config, record):
        require(record.get("image_class") == "receiver" and record["mcuboot_image_hash"] == info["mcuboot_image_hash"]
                and record["build_id"] == info["build_id"] and record["version"] == info["version"],
                "Select the exact original receiver firmware, with its matching USB manifest")
        for field in ("hardware_id", "layout_id", "bootloader_id"):
            require(record[field] == config["manifest"][field], "Receiver hardware profile differs from the campaign")
        require(record["signature"]["key_sha256"] == config["manifest"]["signature"]["key_sha256"],
                "Receiver signing key differs from the campaign")

    def prepare(self, serial_number):
        from ota_workflow import copy_pair
        info = self.w.info(serial_number)
        self.require_interrupted_receiver(info)
        journal = self.campaign(info)
        config = recovery_config(journal, compact_receiver_only=True)
        self.match_board(info, config)
        receiver = self.receiver_image(info, config)
        self.ui.continue_step("Prepare receiver recovery", "Receiver: " + info["identity"] + "\nUSB: " + serial_number
                              + "\n\nKeep outputs stopped. The assistant will archive this failed campaign and compile its recovery image. No board is changed during preparation.")
        path, state = self.w.create(serial_number, "receiver-recovery")
        state["identity"] = identity(info["identity"])
        transition._save(path / "board-info.json", info)
        shutil.copyfile(journal, path / "campaign.jsonl")
        generate(path / "campaign.jsonl", path / "config", compact_receiver_only=True)
        copy_pair(*receiver, path / "receiver")
        # Last extra config wins. Keep src/app.ini and older recovery guards intact.
        override = path / "build-override.ini"
        override.write_text("[env:OTA_RECOVERY]\ncustom_ota_recovery_config = " + (path / "config").as_posix() + "\n", encoding="utf-8")
        parser = configparser.ConfigParser(interpolation=None)
        parser.read(self.w.project / "platformio.ini", encoding="utf-8")
        parser["platformio"]["extra_configs"] = parser["platformio"].get("extra_configs", "") + "\n" + override.as_posix()
        build_config = path / "platformio.ini"
        with build_config.open("w", encoding="utf-8") as stream:
            parser.write(stream)
        self.w.runner([self.w.pio_python, "-m", "platformio", "run", "-d", str(self.w.project), "-c", str(build_config),
                       "-e", "OTA_RECOVERY", "-t", "mcuboot-image"], check=True, cwd=self.w.project)
        self.w.snapshot("OTA_RECOVERY", path / "helper")
        self.context(path, state)
        self.w.save(path, state, "PREPARED")
        return path, state

    def context(self, path, state):
        config, target, helper, _, _ = verify_inputs(path / "config/owntech_ota_recovery_config.json",
                                                   path / "helper/image.bin", path / "helper/manifest.json",
                                                   state["identity"], compact_receiver_only=True)
        info = transition._json(path / "board-info.json")
        require(info.get("usb_serial") == state["serial"] and info.get("identity") == state["identity"],
                "Recovery archive identity or USB serial changed")
        self.match_board(info, config)
        data, receiver = prepare_manifest(path / "receiver/image.bin", path / "receiver/manifest.json", image_class="receiver", usb=True)
        self.match_receiver(info, config, receiver)
        return config, helper, receiver, data

    def inspect_or_apply(self, path, state, *, apply=False):
        return recover(path / "config/owntech_ota_recovery_config.json", path / "helper/image.bin",
                       path / "helper/manifest.json", state["serial"], state["identity"],
                       compact_receiver_only=True, apply=apply, mcumgr=self.w.mcumgr,
                       log_path=path / "recovery.jsonl", enumerate_ports=self.w.enumerate)

    def run(self, action, serial_number):
        path, state = self.choose_operation(serial_number, new_allowed=action not in ("recovery-finish", "recovery-boot-state"))
        _, helper, receiver, data = self.context(path, state)
        require(state["phase"] in ("PREPARED", "INSPECTED", "HELPER_INSTALLING", "HELPER_STARTED", "RECOVERED",
                                   "RECEIVER_INSTALLING", "RECEIVER_STARTED", "COMPLETE"), "Unknown recovery archive phase")
        if action == "recovery-boot-state":
            self.w.boot_prompt(serial_number, "Inspect recovery boot state", reason=
                               "The recovery console did not establish the result. This action reads the two image slots and saves a diagnostic; it sends no erase, upload, confirmation or reset command.")
            output = path / ("recovery-boot-state-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ") + ".json")
            result = inspect_boot_state(serial_number, state["identity"], helper, receiver, output, enumerate_ports=self.w.enumerate)
            message = result["diagnosis"] + "\n\n" + result["explanation"] + "\n\nSnapshot: " + str(output)
            print("OwnTech: " + message, flush=True)
            self.ui.notice("Recovery boot state", message + "\n\nLeave the board in its bootloader until the next step is determined.")
            return
        if action == "recovery-inspect":
            require(state["phase"] in ("PREPARED", "INSPECTED"), "Recovery already started. Use Finish receiver recovery.")
        if state["phase"] in ("PREPARED", "INSPECTED"):
            self.w.boot_prompt(serial_number, "Inspect receiver recovery")
            self.inspect_or_apply(path, state)
            self.w.save(path, state, "INSPECTED")
            if action == "recovery-inspect":
                self.ui.notice("Receiver inspected", "Inspection passed without changing flash. Leave this board in its bootloader and choose Recover interrupted receiver.\nArchive: " + str(path))
                return
            self.ui.continue_step("Apply receiver recovery", "Inspection passed for receiver " + state["identity"]
                                  + ".\n\nInstall the scoped recovery image, then restore the original receiver firmware. Keep USB connected and outputs stopped.")
            self.w.save(path, state, "HELPER_INSTALLING")
            try:
                self.inspect_or_apply(path, state, apply=True)
            except Exception:
                # An unplugged board or a refused preflight made no mutation.
                # Preserve a usable inspected archive in that case only.
                events = transition._records(path / "recovery.jsonl")
                if not any(row.get("event") in ("RECOVERY_ERASE_SECONDARY_REQUEST", "RECOVERY_UPLOAD_REQUEST",
                                                "RECOVERY_RESET_REQUEST") for row in events):
                    self.w.save(path, state, "INSPECTED")
                raise
            self.w.save(path, state, "HELPER_STARTED")
        if state["phase"] in ("HELPER_INSTALLING", "HELPER_STARTED"):
            # If the upload/reset reply was lost, observe the outcome; never
            # infer success from the operator or blindly replay a flash write.
            self.ui.continue_step("Check receiver recovery", "Leave BOOT released. Wait for the recovery application to start, close Serial Monitor/Scope, then click OK to read its result. No upload or reset is retried.")
            collect_result(serial_number, state["identity"], path / "recovery-result.json", enumerate_ports=self.w.enumerate)
            self.w.save(path, state, "RECOVERED")
        if state["phase"] == "RECOVERED":
            result = transition._json(path / "recovery-result.json")
            require(result.get("identity") == state["identity"] and result.get("usb_serial") == serial_number,
                    "Missing recovery result for this board")
            self.w.boot_prompt(serial_number, "Restore original receiver firmware")
            self.w.save(path, state, "RECEIVER_INSTALLING")
            try:
                restore_receiver(serial_number, state["identity"], helper, receiver, data, path / "restore.jsonl", self.w.mcumgr,
                                 enumerate_ports=self.w.enumerate)
            except Exception:
                if not any(row.get("event") == "RESTORE_ERASE_REQUEST" for row in transition._records(path / "restore.jsonl")):
                    self.w.save(path, state, "RECOVERED")
                raise
            self.w.save(path, state, "RECEIVER_STARTED")
        if state["phase"] in ("RECEIVER_INSTALLING", "RECEIVER_STARTED", "COMPLETE"):
            self.ui.continue_step("Verify restored receiver", "Leave BOOT released and allow the receiver to finish startup. Keep the CAN bus connected if available. Click OK to verify identity, firmware, confirmation and maintenance. An uncertain upload is not repeated.")
            result = verify_restored_receiver(serial_number, state["identity"], receiver, enumerate_ports=self.w.enumerate)
            transition._save(path / "verification.json", result)
            self.w.save(path, state, "COMPLETE")
            self.ui.show_status("Receiver recovery complete", dict(result["info"], usb_serial=serial_number))
