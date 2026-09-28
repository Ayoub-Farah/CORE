"""Desktop recovery of an interrupted compact receiver campaign, one USB board at a time."""
import configparser
from pathlib import Path
import re
import shutil
import tempfile
import time

from bootloader_upload import upload_image
from lead_update import CampaignError, Journal, identity, prepare_manifest, select_port
from prepare_ota_recovery import generate, recovery_config
from recover_ota import recover, verify_inputs, verify_slots
from smp_transport import CommandError, SerialSMP
import transition_ota_usb as transition


def require(condition, message):
    if not condition:
        raise CampaignError(message)


def collect_result(serial_number, target, output, *, enumerate_ports, timeout=30,
                   console_factory=None, clock=time.monotonic):
    """Read the helper console without commands, resets or baud-rate triggers."""
    if console_factory is None:
        import serial
        console_factory = serial.Serial
    device = select_port(enumerate_ports(), serial_number).device
    pattern = re.compile(rb"OTA_RECOVERY (RECOVERED|ALREADY_RECOVERED) rc=([01]) EUI=([0-9a-f]{16}) confirmed=1; outputs inhibited")
    with console_factory(device, baudrate=115200, timeout=0.2, write_timeout=0.2) as console:
        console.reset_input_buffer()
        deadline = clock() + timeout
        while clock() < deadline:
            line = console.readline(513).strip()
            require(len(line) <= 512, "Recovery console line exceeds the response bound")
            if not line.startswith(b"OTA_RECOVERY "):
                continue
            match = pattern.fullmatch(line)
            require(match is not None and match[3].decode() == target
                    and ((match[1] == b"RECOVERED" and match[2] == b"0")
                         or (match[1] == b"ALREADY_RECOVERED" and match[2] == b"1")),
                    "Recovery refused or belongs to another board: " + line.decode(errors="replace"))
            select_port(enumerate_ports(), serial_number, device)
            result = dict(identity=target, usb_serial=serial_number, line=line.decode("ascii"))
            transition._save(output, result)
            return result
    raise CampaignError("No confirmed recovery result. Keep the archive and board state; an interrupted upload is not retried automatically.")


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
            candidates.append((str(path), path.parent.name + "/" + path.name))
        candidates.append(("browse", "Select another failed CAN campaign log"))
        selected = self.ui.choose("Interrupted CAN campaign",
                                  "Select the interrupted update for this receiver. Campaigns with activation or reboot are refused.", candidates)
        return self.ui.file("Select failed CAN campaign", self.w.project / "ota-journals", [("Campaign log", "*.jsonl")]) if selected == "browse" else Path(selected)

    @staticmethod
    def match_board(info, config):
        target = next((row for row in config["targets"] if row["identity"] == info.get("identity")), None)
        require(info.get("image_class") == "receiver" and target is not None,
                "The connected board must be a receiver in the frozen campaign; the Lead is excluded")
        require(info.get("active_confirmed") is True and info.get("local_healthy") is True
                and info.get("mcuboot_image_hash") == target["original_active_hash"],
                "The receiver is not running its exact healthy, confirmed original image")
        require(info.get("maintenance") is True and info.get("phase") in ("RECOVERY_REQUIRED", "ABORTED", "FAILED"),
                "This action is for a stopped receiver retained in maintenance after an interrupted transfer")

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
        require(info.get("image_class") == "receiver", "Connect a receiver; this recovery mode never repairs the Lead")
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
        path, state = self.choose_operation(serial_number, new_allowed=action != "recovery-finish")
        _, helper, receiver, data = self.context(path, state)
        require(state["phase"] in ("PREPARED", "INSPECTED", "HELPER_INSTALLING", "HELPER_STARTED", "RECOVERED",
                                   "RECEIVER_INSTALLING", "RECEIVER_STARTED", "COMPLETE"), "Unknown recovery archive phase")
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
            result = transition.verify_ota(dict(identity=state["identity"], usb_serial=serial_number), receiver)
            transition._save(path / "verification.json", result)
            self.w.save(path, state, "COMPLETE")
            self.ui.show_status("Receiver recovery complete", dict(result["info"], usb_serial=serial_number))
