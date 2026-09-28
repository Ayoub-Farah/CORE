#!/usr/bin/env python3
"""Graphical workflows launched by PlatformIO tasks; no terminal input required."""
import argparse
from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import uuid

from lead_update import BootloaderNotReady, CampaignError, USBConnection, identity, json_value, read_only_status
from lead_update import main as campaign_main
from provision_ota import main as provision_main
import transition_ota_usb as transition
from prepare_ota_transition import generate, transition_config, _source, _board
from ota_workflow_dialogs import Cancelled, Dialogs


def require(condition, message):
    if not condition:
        raise CampaignError(message)


@contextmanager
def project_lock(project):
    """OS-released lock also survives an interrupted UI without a stale PID file."""
    path = Path(project) / ".pio/ota-workflow.lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    stream = path.open("a+b")
    locked = False
    try:
        if not path.stat().st_size:
            stream.write(b"0")
            stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            locked = True
        except OSError as error:
            raise CampaignError("Another OwnTech firmware window is working in this project. Close or finish it first.") from error
        yield
    finally:
        if locked:
            stream.seek(0)
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)
        stream.close()


def copy_pair(image, manifest, destination):
    """Snapshot bytes before another build can replace the environment output."""
    image, manifest, destination = Path(image), Path(manifest), Path(destination)
    data, raw = image.read_bytes(), manifest.read_bytes()
    record = transition._json(manifest)
    require(isinstance(record, dict), "The firmware manifest must be a JSON object.")
    require(raw == manifest.read_bytes() and data == image.read_bytes(), "Build files changed during archiving. Finish the other build first.")
    import hashlib
    require(record.get("artifact_sha256") == hashlib.sha256(data).hexdigest()
            and record.get("artifact_size") == len(data), "The firmware and its manifest do not match.")
    destination.mkdir(parents=True, exist_ok=True)
    targets = ((destination / "image.bin", data), (destination / "manifest.json", raw))
    for path, contents in targets:
        require(not path.exists() or path.read_bytes() == contents, "An archived operation cannot be overwritten: " + str(path))
    for path, contents in targets:
        if not path.exists():
            with path.open("xb") as stream:
                stream.write(contents)
                stream.flush()
                os.fsync(stream.fileno())
    return targets[0][0], targets[1][0]


class Workflow:
    def __init__(self, project, environment, mcumgr, ui, *, enumerate_ports=None, runner=None, pio_python=None):
        self.project = Path(project).resolve()
        self.environment = environment
        self.mcumgr = Path(mcumgr)
        self.ui = ui
        self.runner = runner or subprocess.run
        self.pio_python = str(pio_python or sys.executable)
        if enumerate_ports is None:
            from serial.tools.list_ports import comports
            enumerate_ports = comports
        self.enumerate = enumerate_ports
        self.operations = self.project / "ota-artifacts/operations"

    def board(self):
        ports = [p for p in self.enumerate() if p.vid == 0x2FE3 and p.serial_number]
        serials = sorted({p.serial_number for p in ports})
        require(serials, "Connect the OwnTech board by USB, close Serial Monitor and Scope, then click this PlatformIO action again.")
        if len(serials) == 1:
            return serials[0]
        return self.ui.choose("Select board", "Choose the board connected by USB.", [
            (serial, serial + " (" + ", ".join(p.device for p in ports if p.serial_number == serial) + ")") for serial in serials])

    def info(self, serial):
        connection = USBConnection(serial)
        transport = connection.connect()
        try:
            info = transport.request("info")
            require(info.get("service") == "owntech-ota" and info.get("protocol") == 2, "This action needs the OTA V2 application running.")
            return json.loads(json.dumps(dict(info, usb_serial=serial), default=json_value))
        finally:
            transport.close()

    def create(self, serial, kind):
        stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
        path = self.operations / (stamp + "-" + uuid.uuid4().hex[:12])
        path.mkdir(parents=True, exist_ok=False)
        state = {"schema_version": 1, "kind": kind, "serial": serial, "phase": "CREATED"}
        self.save(path, state)
        return path, state

    def save(self, path, state, phase=None):
        if phase:
            state["phase"] = phase
        transition._save(path / "operation.json", state)
        print("OwnTech: %s | archive: %s" % (state["phase"], path), flush=True)

    def existing(self, serial):
        result = []
        for file in sorted(self.operations.glob("*/operation.json"), reverse=True):
            value = transition._json(file)
            require(isinstance(value, dict) and value.get("schema_version") == 1, "Invalid operation archive: " + str(file))
            if (value.get("kind") == "usb-roundtrip" and value.get("serial") == serial
                    and value.get("phase") not in ("CREATED", "OTA_READY")):
                result.append((file.parent, value))
        return result

    def choose_operation(self, serial, *, new_allowed=False):
        existing = self.existing(serial)
        if not existing and new_allowed:
            return self.create(serial, "usb-roundtrip")
        options = [(str(path), path.name + " — " + state["phase"]) for path, state in existing]
        options.append(("browse", "Select an operation folder saved elsewhere"))
        if new_allowed:
            options.append(("new", "Start a new switch from the currently running OTA V2 application"))
        selected = self.ui.choose("Operation archive", "Continue the saved operation for this board, or select its archive folder.", options)
        if selected == "new":
            return self.create(serial, "usb-roundtrip")
        path = self.ui.folder("Select operation folder", self.operations) if selected == "browse" else Path(selected)
        state = transition._json(path / "operation.json")
        require(isinstance(state, dict) and state.get("schema_version") == 1 and state.get("kind") == "usb-roundtrip"
                and state.get("serial") == serial and state.get("phase") != "OTA_READY", "This archive belongs to another board or an already completed round trip.")
        return path, state

    def build(self, environment):
        print("OwnTech: building " + environment + ". Progress appears in the PlatformIO terminal.", flush=True)
        self.runner([self.pio_python, "-m", "platformio", "run", "-d", str(self.project),
                     "-e", environment, "-t", "mcuboot-image"], check=True, cwd=self.project)

    def latest_pair(self, environment, *, compact=False):
        folder = self.project / "ota-artifacts" / environment
        pattern = "*.usb.json" if environment == "USB" else "*.can.json" if compact else "*.mcuboot.json"
        candidates = sorted(folder.glob(pattern), key=lambda p: p.stat().st_mtime_ns, reverse=True)
        require(candidates, "Build output is missing for " + environment + ". Run its Build task first.")
        manifest = candidates[0]
        record = transition._json(manifest)
        require(isinstance(record, dict), "The build manifest must be a JSON object.")
        filename = record.get("filename")
        require(isinstance(filename, str) and Path(filename).name == filename and filename.endswith(".bin"), "Invalid build artifact filename")
        return folder / filename, manifest

    def snapshot(self, environment, destination, *, compact=False):
        return copy_pair(*self.latest_pair(environment, compact=compact), destination)

    def pick_image(self, title):
        image = self.ui.file(title, self.project / "ota-artifacts", [("Firmware", "*.bin")])
        manifest = image.with_suffix(".json")
        if not manifest.is_file():
            candidate = image.parent / "firmware.usb.json"
            manifest = candidate if candidate.is_file() else self.ui.file("Select the matching firmware manifest", image.parent, [("Firmware manifest", "*.json")])
        return image, manifest

    def source(self, info):
        manifests = list((self.project / "ota-artifacts/history").glob("*/*/firmware.json"))
        for environment in ("OTA", "USB_LEAD"):
            folder = self.project / "ota-artifacts" / environment
            manifests += list(folder.glob("*.can.json")) + list(folder.glob("*.mcuboot.json"))
        for path in manifests:
            image = path.with_name("firmware.bin") if path.name == "firmware.json" else path.with_suffix(".bin")
            try:
                manifest = _source(image.read_bytes(), transition._json(path))
                _board(info, manifest, False)
                return image, path
            except (OSError, ValueError):
                continue
        self.ui.notice("Original firmware needed", "Select the exact OTA V2 firmware already installed on this board. A new build cannot replace this original file. Its matching manifest must also be available.")
        image, path = self.pick_image("Select the currently installed OTA V2 firmware")
        _board(info, _source(image.read_bytes(), transition._json(path)), False)
        return image, path

    def history(self, path):
        journals = list((self.project / "ota-journals").glob("*.jsonl")) + list(self.operations.glob("*/campaign.jsonl"))
        valid = []
        args = (path / "source/image.bin", path / "source/manifest.json", path / "board-info.json")
        for journal in sorted(journals, reverse=True):
            try:
                transition_config(*args, journal_path=journal)
                valid.append(journal)
            except (OSError, ValueError):
                continue
        options = [(str(journal), "Completed CAN update: " + journal.parent.name + "/" + journal.name) for journal in valid]
        options += [("browse", "Select the log of the latest successful CAN update"),
                    ("unused", "This board has never participated in a CAN update")]
        choice = self.ui.choose("Board history", "Select this board's latest completed update. Choose 'never' only for a board initialized over USB without a CAN campaign.", options)
        if choice == "unused":
            transition_config(*args, no_campaign=True)
            return {"no_campaign": True}
        original = self.ui.file("Select successful CAN update log", self.project / "ota-journals", [("CAN update log", "*.jsonl")]) if choice == "browse" else Path(choice)
        shutil.copyfile(original, path / "campaign.jsonl")
        transition_config(*args, journal_path=path / "campaign.jsonl")
        return {"journal_path": path / "campaign.jsonl"}

    def prepare(self, path, state):
        transition.capture(state["serial"], path / "board-info.json")
        info = transition._json(path / "board-info.json")
        state["identity"] = identity(info["identity"])
        copy_pair(*self.source(info), path / "source")
        evidence = self.history(path)
        generate(path / "source/image.bin", path / "source/manifest.json", path / "board-info.json", path / "config", **evidence)
        destination = self.project / ".pio/ota-transition-config"
        destination.mkdir(parents=True, exist_ok=True)
        for suffix in ("h", "json"):
            shutil.copyfile(path / ("config/owntech_ota_recovery_config." + suffix), destination / ("owntech_ota_recovery_config." + suffix))
        self.build("OTA_TRANSITION")
        self.snapshot("OTA_TRANSITION", path / "helper")
        self.build("USB")
        self.snapshot("USB", path / "usb")
        self.context(path, state)  # Bind compiled helper to these exact guards.
        self.save(path, state, "PREPARED")

    def context(self, path, state):
        return transition.inputs(path / "config/owntech_ota_recovery_config.json", path / "helper/image.bin",
                                 path / "helper/manifest.json", state["serial"], state["identity"])

    def boot_prompt(self, serial, next_action, *, reason=None):
        introduction = reason + "\n\n" if reason else ""
        self.ui.continue_step(next_action, introduction + "Board: " + serial + "\n\nKeep the power stage stopped. Close Serial Monitor and Scope.\n\nHold BOOT, press and release RESET, keep BOOT held for about one second, then release it.\n\nClick OK when the board is connected in its OwnTech USB bootloader. The next step will " + next_action.lower() + ".")

    def install(self, path, state, stage, config, helper, image, data, *, usb=None):
        kwargs = dict(image=image, data=data, usb=usb, receipt=path / "cleanup-receipt.json" if stage != "install-helper" else None,
                      mcumgr=self.mcumgr, log_path=path / "transition.jsonl", enumerate_ports=self.enumerate)
        resume = False
        try:
            transition.bootloader_step(config, helper, stage, **kwargs)
        except ValueError:
            log = path / "transition.jsonl"
            if not all(transition._has_event(log, config["token"], event, image["mcuboot_image_hash"])
                       for event in ("TRANSITION_SECONDARY_READY", "TRANSITION_UPLOAD_REQUEST")):
                raise
            # The underlying client requires both durable preparation and upload
            # records, plus the exact pending candidate. No generic reset retry.
            transition.bootloader_step(config, helper, stage, resume=True, **kwargs)
            resume = True
        return transition.bootloader_step(config, helper, stage, apply=True, resume=resume, **kwargs)

    def to_usb(self, serial):
        path, state = self.choose_operation(serial, new_allowed=True)
        self.ui.continue_step("Switch to USB", "Board: " + serial + "\n\nFinish the whole CAN update before switching any receiver. Convert the Lead last. Keep outputs stopped. This wizard will install a temporary cleanup application, then this project's ordinary USB application. All files and progress are saved automatically.")
        if state["phase"] == "CREATED":
            self.prepare(path, state)
        config, helper, helper_data = self.context(path, state)
        usb, usb_data = transition._image(path / "usb/image.bin", path / "usb/manifest.json", config["source_manifest"], "usb")
        if state["phase"] == "HELPER_INSTALLING":
            next_step = self.ui.choose("Continue interrupted switch", "Choose the last completed step. The board will be verified before continuing.", [
                ("receipt", "The cleanup application started — read its result"),
                ("install", "The cleanup upload did not finish — retry through BOOT + RESET")])
            if next_step == "receipt":
                self.save(path, state, "HELPER_STARTED")
        if state["phase"] in ("PREPARED", "HELPER_INSTALLING"):
            self.boot_prompt(serial, "Install cleanup application")
            self.save(path, state, "HELPER_INSTALLING")
            self.install(path, state, "install-helper", config, helper, helper, helper_data)
            self.save(path, state, "HELPER_STARTED")
        if state["phase"] == "HELPER_STARTED":
            self.ui.continue_step("Check cleanup", "Wait for the board to restart. Leave BOOT released and close serial monitors. Click OK to read and save the cleanup result.")
            transition.collect_receipt(config, helper, path / "cleanup-receipt.json", timeout=30, enumerate_ports=self.enumerate)
            self.save(path, state, "CLEANED")
        if state["phase"] == "USB_INSTALLING":
            next_step = self.ui.choose("Continue USB installation", "The next step verifies the board, even if the previous upload reply was lost.", [
                ("verify", "The USB application started — verify it"),
                ("install", "The USB upload did not finish — retry it")])
            if next_step == "verify":
                self.save(path, state, "USB_STARTED")
        if state["phase"] in ("CLEANED", "USB_INSTALLING"):
            self.boot_prompt(serial, "Install USB application")
            self.save(path, state, "USB_INSTALLING")
            self.install(path, state, "install-usb", config, helper, usb, usb_data)
            self.save(path, state, "USB_STARTED")
        require(state["phase"] in ("USB_STARTED", "USB_READY"), "This archive is already returning to OTA. Use Return to OTA V2.")
        self.ui.continue_step("Let USB start", "Allow the ordinary USB application to finish startup. Keep the power stage stopped. Click OK before the final board check.")
        self.boot_prompt(serial, "Verify USB and return to normal operation")
        transition.bootloader_step(config, helper, "verify-usb", usb=usb, receipt=path / "cleanup-receipt.json",
                                  apply=True, log_path=path / "transition.jsonl", enumerate_ports=self.enumerate)
        self.save(path, state, "USB_READY")
        self.ui.notice("USB ready", "The board is running the verified ordinary USB application.\n\nTo come back later, choose Return to OTA V2. Keep this automatically saved folder:\n" + str(path))

    def current_usb(self, path, config):
        options = [("saved", "USB application installed by this switch")]
        if any((self.project / "ota-artifacts/USB").glob("*.usb.json")):
            options.insert(0, ("latest", "Most recent USB build in this project"))
        options.append(("browse", "Select the USB firmware currently installed"))
        choice = self.ui.choose("Current USB application", "If you uploaded another USB build since the switch, select that build. The exact running image will be checked.", options)
        pair = self.pick_image("Select the currently installed USB firmware") if choice == "browse" else (
            self.latest_pair("USB") if choice == "latest" else (path / "usb/image.bin", path / "usb/manifest.json"))
        revision = path / "usb-revisions" / uuid.uuid4().hex
        pair = copy_pair(*pair, revision)
        usb, _ = transition._image(*pair, config["source_manifest"], "usb")
        return pair, usb

    def to_ota(self, serial):
        require(self.environment in ("OTA", "USB_LEAD"), "Choose Return to OTA V2 under OTA for a receiver, or USB_LEAD for a coordinator.")
        path, state = self.choose_operation(serial)
        require(state["phase"] in ("USB_READY", "RETURN_PREPARED", "OTA_INSTALLING", "OTA_STARTED"), "Finish Switch to USB for this operation first.")
        config, helper, _ = self.context(path, state)
        require(self.environment == ("USB_LEAD" if config["image_class"] == "lead" else "OTA"),
                "Use the same role as this board's original OTA application: OTA for a receiver, USB_LEAD for the Lead.")
        transition.verify_receipt(path / "cleanup-receipt.json", config, helper)
        if state["phase"] == "USB_READY":
            pair, usb = self.current_usb(path, config)
            state["current_usb"] = str(pair[0].parent.relative_to(path))
            state["return_environment"] = self.environment
            self.build(self.environment)
            self.snapshot(self.environment, path / "return")
            self.save(path, state, "RETURN_PREPARED")
        require(state.get("return_environment") == self.environment, "Resume from the same PlatformIO environment used for this return.")
        folder = (path / state["current_usb"]).resolve()
        require(folder.is_relative_to(path.resolve()), "Invalid archived USB revision path")
        usb, _ = transition._image(folder / "image.bin", folder / "manifest.json", config["source_manifest"], "usb")
        ota, ota_data = transition._image(path / "return/image.bin", path / "return/manifest.json", config["source_manifest"], "ota")
        if state["phase"] == "OTA_INSTALLING":
            choice = self.ui.choose("Continue return to OTA", "Choose the last completed step; the board identity and image will be verified.", [
                ("verify", "OTA started — check its status"), ("install", "Upload did not finish — retry it")])
            if choice == "verify":
                self.save(path, state, "OTA_STARTED")
        if state["phase"] in ("RETURN_PREPARED", "OTA_INSTALLING"):
            self.boot_prompt(serial, "Verify USB and install OTA V2")
            if state["phase"] == "RETURN_PREPARED":
                transition.bootloader_step(config, helper, "verify-usb", usb=usb, receipt=path / "cleanup-receipt.json",
                                          log_path=path / "transition.jsonl", enumerate_ports=self.enumerate)
            self.save(path, state, "OTA_INSTALLING")
            self.install(path, state, "return-ota", config, helper, ota, ota_data, usb=usb)
            self.save(path, state, "OTA_STARTED")
        self.ui.continue_step("Check OTA startup", "Leave BOOT released and wait for the OTA application to start. Click OK to verify the running image, confirmation and local health.")
        result = transition.verify_ota(config, ota)
        self.save(path, state, "OTA_READY")
        self.ui.show_status("OTA V2 ready", dict(result["info"], usb_serial=serial))

    def initialize(self, serial):
        require(self.environment in ("OTA", "USB_LEAD"), "Choose Initialize over USB under OTA or USB_LEAD.")
        path, state = self.create(serial, "initialization")
        image, manifest = self.snapshot(self.environment, path / "firmware")
        # Existing provisioning discovers its manifest beside image.bin. Keep
        # that sidecar so the compiled build identity/profile is never lost.
        image.with_suffix(".json").write_bytes(manifest.read_bytes())
        role = "lead" if self.environment == "USB_LEAD" else "receiver"
        self.ui.continue_step("Initialize over USB", "Board: " + serial + "\nRole: " + role + "\n\nKeep outputs stopped and close Serial Monitor/Scope. This is for a board running an ordinary USB application with known clean OTA history. If you previously used Switch to USB, use Return to OTA V2 instead.")
        self.save(path, state, "INSTALLING")
        args = ["--image", str(image), "--image-class", role, "--serial", serial, "--mcumgr", str(self.mcumgr)]
        try:
            result = provision_main(args + ["--legacy-console"], raise_errors=True, return_result=True)
        except BootloaderNotReady as error:
            # This exception is raised only before the first firmware upload.
            # Other refusals and upload/postboot failures must never reset/retry.
            self.boot_prompt(serial, "Initialize OTA V2 through USB bootloader", reason=
                             "Automatic USB bootloader entry did not succeed. No firmware was sent. "
                             "Enter the bootloader manually to continue with the same saved firmware.\n\n" + str(error))
            result = provision_main(args + ["--bootloader"], raise_errors=True, return_result=True)
        require(isinstance(result, dict) and result.get("result") in ("PROVISIONED", "ALREADY_INITIALIZED")
                and result.get("usb_serial") == serial and isinstance(result.get("info"), dict),
                "Initialization did not return a verified result for the selected board.")
        transition._save(path / "verification.json", result)
        self.save(path, state, "OTA_READY")
        # Provisioning already verified this exact image, health and confirmation.
        # Reopening the CDC immediately can miss a rate-limited status reply and
        # turn a successful installation into a misleading timeout error.
        self.ui.show_status("Initialized over USB", dict(result["info"], usb_serial=serial))

    def status(self, serial):
        connection = USBConnection(serial)
        transport = connection.connect()
        try:
            info = transport.request("info")
            if info.get("image_class") == "lead":
                info = dict(info, fleet=read_only_status(transport)["status"])
            self.ui.show_status("Connected board", dict(info, usb_serial=serial))
        finally:
            transport.close()

    def can_update(self, serial):
        require(self.info(serial).get("image_class") == "lead", "Connect the dedicated Lead by USB for a CAN update.")
        count = self.ui.number("CAN receivers", "How many receiver boards must be updated? Do not count the Lead.", 1, 16)
        self.ui.continue_step("Update CAN receiver boards", "Keep the whole fleet connected and outputs stopped. The action will build this project's receiver application, update exactly %d receivers and verify their restart." % count)
        path, state = self.create(serial, "can-update")
        self.build("OTA")
        image, manifest = self.snapshot("OTA", path / "firmware", compact=True)
        self.save(path, state, "UPDATING")
        rc = campaign_main(["--image", str(image), "--manifest", str(manifest), "--serial", serial,
                            "--expected-count", str(count), "--journal", str(path / "campaign.jsonl")], raise_errors=True)
        require(rc == 0, "The CAN update did not finish. Keep the fleet connected and use Finish previous CAN update. Archive: " + str(path))
        self.save(path, state, "COMPLETE")
        self.ui.notice("CAN update complete", "Every expected receiver has been verified. The update log and firmware are saved in:\n" + str(path))

    def reconcile(self, serial):
        require(self.info(serial).get("image_class") == "lead", "Connect the original dedicated Lead by USB.")
        journals = []
        for file in self.operations.glob("*/operation.json"):
            state = transition._json(file)
            journal = file.parent / "campaign.jsonl"
            if state.get("kind") == "can-update" and state.get("serial") == serial and journal.is_file():
                journals.append(journal)
        options = [(str(p), p.parent.name) for p in sorted(journals, reverse=True)] + [("browse", "Select another saved CAN update log")]
        choice = self.ui.choose("Finish previous CAN update", "Select the original update. Keep every receiver connected. This checks/reconciles its existing result; it does not start a new upload.", options)
        journal = self.ui.file("Select CAN update log", self.project / "ota-journals", [("CAN update log", "*.jsonl")]) if choice == "browse" else Path(choice)
        records = transition._records(journal)
        if records and records[-1].get("event") == "SUCCESS":
            self.ui.notice("Update already completed", "This saved update already ended in SUCCESS. Its original log has been preserved. Use Check connected board for current status, or Switch to USB to continue.")
            return
        rc = campaign_main(["--serial", serial, "--reconcile-journal", str(journal)], raise_errors=True)
        require(rc == 0, "Reconciliation stopped. Preserve the board state and saved log; see the PlatformIO task output.")
        self.ui.notice("Update reconciled", "The original update has reached SUCCESS. Its saved log can be used by Switch to USB.")

    def run(self, action):
        if self.environment == "OTA_RECOVERY":
            require(action == "status" or action.startswith("recovery-"), "Choose a receiver recovery action under OTA_RECOVERY.")
        else:
            require(not action.startswith("recovery-"), "Choose receiver recovery under OTA_RECOVERY.")
        serial = self.board()
        if action.startswith("recovery-"):
            from ota_recovery_workflow import RecoveryWorkflow
            return RecoveryWorkflow(self).run(action, serial)
        {"initialize": self.initialize, "to-usb": self.to_usb, "to-ota": self.to_ota,
         "status": self.status, "can-update": self.can_update, "reconcile": self.reconcile}[action](serial)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("initialize", "to-usb", "to-ota", "status", "can-update", "reconcile",
                                          "recovery-inspect", "recovery-run", "recovery-finish"))
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--environment", choices=("USB", "OTA", "USB_LEAD", "OTA_RECOVERY"), required=True)
    parser.add_argument("--mcumgr", type=Path, required=True)
    parser.add_argument("--pio-python", type=Path, help="PlatformIO interpreter, separate from the desktop GUI Python")
    args = parser.parse_args(argv)
    ui = None
    try:
        ui = Dialogs()
        with project_lock(args.project):
            Workflow(args.project, args.environment, args.mcumgr, ui, pio_python=args.pio_python).run(args.action)
        return 0
    except Cancelled:
        print("OwnTech: cancelled. Completed steps and their archives are preserved; no further action was sent.")
        return 1
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        message = str(error)
        print("OwnTech workflow stopped: " + message, file=sys.stderr)
        if ui:
            try:
                ui.problem("OwnTech action stopped", message + "\n\nThe saved operation is preserved. Do not force a reset or delete its files to bypass a refusal.")
            except (OSError, RuntimeError):
                pass  # The original desktop-runtime error is already printed.
        return 1
    finally:
        if ui:
            ui.close()


if __name__ == "__main__":
    raise SystemExit(main())
