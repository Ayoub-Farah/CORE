#!/usr/bin/env python3
"""Build-independent USB Lead campaign client. See docs/ota-client.md."""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import secrets
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from ota_artifact import inspect_image, inspect_usb_image, load_profile
from smp_transport import SerialSMP, TransportError, ReceiverProbeTimeout, ProtocolError, CommandError
from bootloader_upload import upload_image, UploadError


class CampaignError(RuntimeError):
    pass


class BootloaderNotReady(CampaignError):
    """Image-service wait expired before firmware upload or post-upload reset."""


class ReceiverStatus:
    """Read-only minimal receiver status; no bytes are injected into console RX."""
    def __init__(self, device, timeout=2):
        import serial
        self.port = serial.Serial(device, baudrate=115200, timeout=0.2, write_timeout=0.2)
        self.device = device
        self.timeout = timeout
        self.last_request = -1e9

    def close(self):
        self.port.close()

    def request(self, command, payload=None):
        if command != "info" or payload:
            raise CampaignError("minimal receiver USB status is read-only")
        pause = 0.3 - (time.monotonic() - self.last_request)
        if pause > 0:
            time.sleep(pause)
        self.port.reset_input_buffer()
        deadline = time.monotonic() + self.timeout
        # A CDC line-coding request can be missed during USB startup. Retry only
        # silence, within the original response budget and the firmware's rate
        # limit. Keep queued input so a delayed reply is not discarded.
        retry_interval = max(0.3, self.timeout / 3)
        next_request = time.monotonic()
        attempts = 0
        try:
            while time.monotonic() < deadline:
                if attempts < 3 and time.monotonic() >= next_request:
                    self.port.baudrate = 115200
                    self.port.baudrate = 2400
                    self.last_request = time.monotonic()
                    next_request = self.last_request + retry_interval
                    attempts += 1
                line = self.port.readline(1025)
                if len(line) > 1024:
                    raise ProtocolError("receiver status exceeds the bounded response")
                marker = line.find(b"OTAR2 ")
                if marker < 0:
                    continue
                try:
                    result = json.loads(line[marker + 6:])
                except (ValueError, UnicodeError) as error:
                    raise ProtocolError("invalid receiver status JSON") from error
                if (not isinstance(result, dict) or result.get("service") != "owntech-ota"
                        or result.get("protocol") != 2 or result.get("image_class") not in ("receiver", "lead")):
                    raise ProtocolError("incompatible receiver status")
                result["role"] = "lead" if result["image_class"] == "lead" else "follower"
                return result
            raise ReceiverProbeTimeout(
                "no OTAR2 status on %s after %d read-only requests; board state is unknown. "
                "Close Serial Monitor/Scope and retry Check connected board; a timeout does not establish that initialization is needed"
                % (self.device, attempts))
        finally:
            self.port.baudrate = 115200


DEVICE_EVENTS = ("ERASE_BEGIN", "ERASE_END", "USB_STAGE_BEGIN", "USB_STAGE_END",
                 "CAN_TRANSFER_BEGIN", "CAN_TRANSFER_END", "FLASH_COMPLETE", "VERIFY_BEGIN",
                 "VERIFY_END", "ALL_VALIDATED", "REBOOTING", "POSTBOOT_CHECK")


def identity(value):
    if isinstance(value, bytes):
        value = value.hex()
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-fA-F]{16}", value) is None:
        raise CampaignError("invalid EUI-64 identity: %r" % value)
    return value.lower()


def json_value(value):
    if isinstance(value, bytes):
        return value.hex()
    raise TypeError(type(value).__name__)


def select_port(ports, serial_number=None, device=None):
    candidates = [port for port in ports if port.vid == 0x2FE3]
    if serial_number:
        candidates = [port for port in candidates if port.serial_number == serial_number]
    if device:
        candidates = [port for port in candidates if port.device == device]
    if len(candidates) != 1 or not candidates[0].serial_number:
        raise CampaignError("select exactly one OwnTech USB device with a stable serial number; found %d" % len(candidates))
    return candidates[0]


class USBConnection:
    def __init__(self, serial_number=None, device=None, timeout=30):
        from serial.tools.list_ports import comports
        self.enumerate = comports
        ports = [port for port in self.enumerate() if port.vid == 0x2FE3 and port.serial_number
                 and (not serial_number or port.serial_number == serial_number)
                 and (not device or port.device == device)]
        serials = {port.serial_number for port in ports}
        if len(serials) != 1:
            raise CampaignError("select exactly one OwnTech USB board by stable serial number")
        self.serial_number = serials.pop()
        self.bootstrap_port = device or (ports[0].device if len(ports) == 1 else None)
        self.device = self.bootstrap_port
        self.timeout = timeout
        self.transport = None
        self.last_info = None

    def connect(self):
        candidates = [port for port in self.enumerate()
                      if port.vid == 0x2FE3 and port.serial_number == self.serial_number]
        if not 1 <= len(candidates) <= 4:
            raise TransportError("same USB serial is absent or has too many interfaces")
        if len(candidates) == 1:
            # Unknown single-CDC applications must never receive an SMP probe.
            candidate = ReceiverStatus(candidates[0].device)
            try:
                info = candidate.request("info")
            except Exception:
                candidate.close()
                raise
            self.device, self.transport, self.last_info = candidates[0].device, candidate, info
            return candidate
        selected = []
        transport_errors = []
        try:
            for port in candidates:
                candidate = None
                try:
                    candidate = SerialSMP(port.device)
                    info = candidate.request("info")
                    if info.get("service") != "owntech-ota" or info.get("protocol") != 2:
                        raise CampaignError("USB interface replied with an incompatible OTA service")
                    selected.append((port.device, candidate, info))
                except ReceiverProbeTimeout:
                    if candidate:
                        candidate.close()
                except TransportError as error:
                    # A blocked console CDC must not hide the application's
                    # separate SMP CDC. It also cannot prove receiver absence.
                    transport_errors.append((port.device, error))
                    if candidate:
                        candidate.close()
                except Exception:
                    if candidate:
                        candidate.close()
                    raise
            if not selected:
                if transport_errors:
                    details = "; ".join("%s: %s" % item for item in transport_errors)
                    raise TransportError("no application receiver could be selected; receiver absence has not "
                                         "been proven (%s)" % details) from transport_errors[0][1]
                raise ReceiverProbeTimeout("no application receiver replied on the selected USB board")
            if len(selected) != 1:
                raise CampaignError("multiple compatible SMP interfaces on the same USB board")
            self.device, self.transport, self.last_info = selected[0]
            return self.transport
        except Exception:
            for _, candidate, _ in selected:
                candidate.close()
            raise

    def reconnect(self):
        if self.transport:
            self.transport.close()
            self.transport = None
        deadline = time.monotonic() + self.timeout
        last = None
        while time.monotonic() < deadline:
            try:
                return self.connect()
            except (CampaignError, TransportError, CommandError) as error:
                last = error
                time.sleep(0.5)
        raise TransportError("same USB serial did not return: %s (%s)" % (self.serial_number, last))

    def _wait_image_service(self):
        """Require a readable legacy image service on the same physical board."""
        deadline = time.monotonic() + self.timeout
        last = "no USB interface for the selected serial"
        while time.monotonic() < deadline:
            ports = [port for port in self.enumerate()
                     if port.vid == 0x2FE3 and port.serial_number == self.serial_number]
            if len(ports) > 4:
                raise CampaignError("selected board has too many USB interfaces")
            ready = []
            unresolved = False
            for port in ports:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    unresolved = True
                    break
                candidate = None
                replied = False
                try:
                    candidate = SerialSMP(port.device, timeout=min(1.0, remaining))
                    try:
                        candidate.request("info")
                    except CommandError as error:
                        if not error.unsupported:
                            raise
                        replied = True
                    else:
                        raise CampaignError("application service replied on %s; refusing bootloader upload. "
                                            "Rerun OTA to check it or use USB_LEAD for a live OTA application" % port.device)
                    image_state = candidate.image_state()
                    if not image_state["images"]:
                        raise CampaignError("bootloader returned an empty image list: no image recognized; "
                                            "primary/secondary state cannot be verified. Stopping before firmware "
                                            "upload and post-upload reset; 1200-baud entry may already have occurred")
                    ready.append(port.device)
                except ReceiverProbeTimeout as error:
                    unresolved |= replied
                    last = "%s: %s" % (port.device, error)
                except TransportError as error:
                    unresolved = True
                    last = "%s: %s" % (port.device, error)
                finally:
                    if candidate:
                        candidate.close()
            if len(ready) > 1:
                raise CampaignError("multiple image services on the selected USB board; no upload sent")
            if ready and not unresolved:
                return ready[0]
            time.sleep(0.25)
        raise BootloaderNotReady("image service did not become ready for USB serial %s (%s). "
                            "No firmware upload or post-upload reset sent; check the board's bootloader mode and close the serial monitor"
                            % (self.serial_number, last))

    def bootstrap(self, image, mcumgr, *, enter_bootloader=True):
        """Explicit receiver-absent provisioning only, before any campaign exists."""
        import serial
        if self.transport:
            self.transport.close()
            self.transport = None
        if not Path(mcumgr).is_file():
            raise CampaignError("bootstrap needs the existing OwnTech mcumgr executable")
        if enter_bootloader:
            if not self.bootstrap_port:
                raise CampaignError("multiple USB interfaces without a receiver: select the console with --port/custom_ota_port before bootstrap")
            select_port(self.enumerate(), self.serial_number, self.bootstrap_port)
            print("Entering bootloader on %s (USB serial %s)" % (self.bootstrap_port, self.serial_number), flush=True)
            # Match PlatformIO's TouchSerialPort, including the DTR transition.
            try:
                with serial.Serial(self.bootstrap_port, baudrate=1200, timeout=0.2) as console:
                    console.setDTR(False)
            except OSError as error:
                # Windows may lose the CDC device inside SetCommState because
                # setting 1200 baud already triggered the board's reset.
                # pyserial embeds WinError's repr instead of retaining winerror.
                # Access-denied/busy/ordinary configuration errors still stop.
                detached = getattr(error, "winerror", None) in (433, 1167) or (
                    str(error).startswith("Cannot configure port") and
                    re.search(r"\b(?:OSError|WindowsError)\(.*?,\s*(?:433|1167)\)\s*$", str(error)) is not None)
                if not detached:
                    raise
                print("USB disappeared during 1200-baud entry; checking the image service on the same serial "
                      "without repeating the reset", flush=True)
            time.sleep(0.4)
        print("Waiting for the image service on USB serial %s..." % self.serial_number, flush=True)
        self.device = self._wait_image_service()
        print("Image service ready on %s; uploading %s" % (self.device, image), flush=True)
        connection = "dev=%s,baud=115200,mtu=128" % self.device
        base = [str(mcumgr), "--conntype", "serial", "--connstring", connection,
                "--timeout", "10", "--tries", "1"]
        # Neither command writes/replaces the bootloader. A padded image can be
        # pending already; only this initial provisioning path sends reset.
        upload_image(base, image)
        subprocess.run(base + ["reset"], check=True, timeout=15)
        time.sleep(2)
        return self.reconnect()


class Journal:
    def __init__(self, path, campaign):
        self.path = Path(path)
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self.device_events_seen = set()
        if self.path.is_file():
            for line in self.path.read_text(encoding="utf-8").splitlines():
                record = json.loads(line)
                if "device_event" in record:
                    self.device_events_seen.add((record["campaign"], record["identity"], record["device_event"]))
        self.stream = self.path.open("a", encoding="utf-8")
        self.campaign = campaign

    def emit(self, event, target=None, **data):
        row = {"timestamp": datetime.now(timezone.utc).isoformat(), "campaign": self.campaign,
               "identity": target, "event": event, **data}
        self.stream.write(json.dumps(row, default=json_value, sort_keys=True) + "\n")
        self.stream.flush()

    def close(self):
        self.stream.close()


class Campaign:
    def __init__(self, transport, manifest, artifact, journal, expected_ids=None, expected_count=None,
                 reconnect=None, timeout=180, poll_interval=0.4, clock=time.monotonic, sleep=time.sleep,
                 output=print):
        self.transport, self.manifest, self.artifact, self.journal = transport, manifest, artifact, journal
        self.expected_ids = [identity(value) for value in expected_ids] if expected_ids else None
        self.expected_count, self.reconnect = expected_count, reconnect
        self.timeout, self.poll_interval, self.clock, self.sleep = timeout, poll_interval, clock, sleep
        self.output = output
        self.targets = []
        self.rows = {}
        self.lead = None
        self.committed = False

    def request(self, name, payload=None):
        return self.transport.request(name, payload or {})

    def probe(self):
        response = self.request("info")
        if response.get("service") != "owntech-ota" or response.get("protocol") != 2:
            raise CampaignError("USB board replied with an incompatible service; no automatic bootstrap")
        if response.get("image_class") != "lead":
            raise CampaignError("campaign requires a dedicated Lead image")
        self.lead = identity(response.get("identity"))
        self.journal.emit("PROBE_LEAD", self.lead, info=response)
        if response.get("slot_size") != self.manifest["profile"]["slot_size"]:
            raise CampaignError("Lead file capacity differs from the artifact profile")
        if response.get("useful_capacity", 0) < self.manifest["useful_size"]:
            raise CampaignError("Lead useful image capacity is insufficient")
        return response

    def _collect(self, command="status", payload=None):
        result = self.request(command, payload)
        pages = []
        target = row = None
        try:
            rows = list(result.get("targets", []))
            count = result.get("target_count", len(rows))
            if type(count) is not int or not 0 <= count <= 16:
                raise CampaignError("invalid bounded target count")
            if len(rows) < count:
                rows = []
                for index in range(count):
                    page = self.request(command, {**(payload or {}), "index": index})
                    pages.append({"index": index, "response": page})
                    rows.extend(page.get("targets", []))
            found = set()
            for row in rows:
                target = None
                target = identity(row.get("identity"))
                if target in found:
                    raise CampaignError("duplicate identity in target status")
                found.add(target)
                old = self.rows.get(target, {})
                self._device_events(target, row)
                if old.get("state") != row.get("state"):
                    self.journal.emit("STATE", target, status=row)
                self.rows[target] = row
        except CampaignError as error:
            # Preserve the wire evidence before run() aborts an uncommitted
            # campaign. Invalid rows must never become accepted device events.
            self.journal.emit("STATUS_REJECTED", target, command=command, reason=str(error),
                              response=result, page_responses=pages, rejected_row=row)
            raise
        result["targets"] = rows
        return result

    def _device_events(self, target, row):
        """Preserve device-recorded transitions missed between PC polls/reboots."""
        mask = row.get("event_mask", 0)
        if not mask:
            return
        times, order, campaign = row.get("event_ms"), row.get("event_order"), row.get("campaign")
        count = len(DEVICE_EVENTS)
        if (type(mask) is not int or not 0 <= mask < 1 << count or type(campaign) is not int
                or not 0 < campaign < 1 << 64 or not isinstance(times, list) or len(times) != count
                or not isinstance(order, list) or len(order) != count):
            raise CampaignError("invalid device event history")
        present = [index for index in range(count) if mask & (1 << index)]
        if (any(type(times[index]) is not int or not 0 <= times[index] <= 0xFFFFFFFF
                or type(order[index]) is not int or not 1 <= order[index] <= count for index in present)
                or len({order[index] for index in present}) != len(present)):
            raise CampaignError("invalid device event order/timestamps")
        if campaign != self.journal.campaign:
            # A fresh inventory can still carry the previous successful
            # campaign's trace. Validate it, retain it in STATE/STATUS, and
            # never relabel or re-emit it into the new campaign's event stream.
            return
        for index in sorted(present, key=lambda index: order[index]):
            key = campaign, target, index
            if key not in self.journal.device_events_seen:
                self.journal.emit(DEVICE_EVENTS[index], target, campaign=campaign,
                                  device_event=index, device_order=order[index], device_uptime_ms=times[index])
                self.journal.device_events_seen.add(key)

    def _table(self, result):
        self.output("Campaign %s | %s | pass %s" % (self.journal.campaign, result.get("phase", result.get("state", "?")), result.get("pass", 0)))
        self.output("Identity         Role     Accepted/total   Queue State           Validated Postboot")
        for target in self.targets:
            row = self.rows.get(target, {})
            self.output("%s %-8s %6d/%-6d %5d %-15s %-9s %s" % (
                target, row.get("role", "?"), row.get("offset", 0), self.manifest["artifact_size"],
                row.get("queue_depth", 0), row.get("state", "MISSING"),
                str(row.get("validated", False)), row.get("postboot", "WAITING")))
        self.journal.emit("STATUS", status=result)

    def _poll(self, predicate, description):
        deadline = self.clock() + self.timeout
        while self.clock() < deadline:
            result = self._collect()
            self._table(result)
            if str(result.get("phase", result.get("state", ""))).upper() in ("FAILED", "ABORTED"):
                raise CampaignError("%s failed: %s" % (description, result))
            if any(row.get("error") not in (None, 0, "", "NONE") for row in result["targets"]):
                raise CampaignError("target error during %s" % description)
            if predicate(result):
                return result
            served = self._serve_source(result)
            self.sleep(0.001 if served else self.poll_interval)
        raise CampaignError("bounded timeout during " + description)

    def discover(self):
        if not self.expected_ids and not self.expected_count:
            raise CampaignError("expected identities or expected total count (excluding Lead) is required")
        if self.expected_ids and len(self.expected_ids) != len(set(self.expected_ids)):
            raise CampaignError("duplicate expected identity")
        deadline = self.clock() + self.timeout
        while True:
            result = self._collect("discover", {"campaign": self.journal.campaign})
            if str(result.get("phase", result.get("state", ""))).upper() != "DISCOVERING":
                break
            if self.clock() >= deadline:
                raise CampaignError("bounded timeout during discovery")
            self.sleep(self.poll_interval)
        rows = result["targets"]
        discovered = [identity(row["identity"]) for row in rows]
        if self.lead in discovered:
            raise CampaignError("dedicated Lead must be excluded from the receiver target list")
        addresses = [row.get("address") for row in rows]
        if None in addresses or len(addresses) != len(set(addresses)):
            raise CampaignError("discovery contains absent or duplicate CAN addresses")
        if self.expected_ids and set(self.expected_ids) != set(discovered):
            raise CampaignError("inventory differs: missing=%s unexpected=%s" % (
                sorted(set(self.expected_ids) - set(discovered)), sorted(set(discovered) - set(self.expected_ids))))
        if self.expected_count is not None and len(discovered) != self.expected_count:
            raise CampaignError("expected %d receivers excluding Lead, discovered %d" % (self.expected_count, len(discovered)))
        if not rows or any(row.get("available") is not True or row.get("compatible") is not True for row in rows):
            rejected = [row for row in rows if row.get("available") is not True or row.get("compatible") is not True]
            self.journal.emit("INVENTORY_REJECTED", inventory=rows, rejected=rejected)
            details = []
            for row in rejected:
                error = row.get("error")
                reason = "OTA_ERR_IDENTITY: CAN identity/address conflict" if error == -4 else str(error)
                details.append("%s (CAN address %s): compatible=%s, available=%s, confirmed=%s, healthy=%s, state=%s, error=%s" % (
                    row["identity"], row.get("address"), row.get("compatible"), row.get("available"),
                    row.get("confirmed"), row.get("healthy"), row.get("state"), reason))
            raise CampaignError("all expected boards must expose a compatible, available OTA receiver; "
                                + ("; ".join(details) if details else "no receiver reported"))
        self.targets = sorted(discovered)
        self.journal.emit("DISCOVER", lead_identity=self.lead, targets=self.targets, inventory=rows, manifest=self.manifest)
        return rows

    def stage(self):
        manifest = self.manifest
        if (manifest.get("protocol") != 2 or manifest.get("image_class") != "receiver"
                or manifest.get("format") != "mcuboot-compact" or manifest.get("activation_trailer") is not False):
            raise CampaignError("campaign requires a receiver compact v2 artifact without activation trailer")
        begin = {key: manifest[key] for key in ("protocol", "artifact_size", "useful_size", "version",
                                                 "build_id", "hardware_id", "layout_id", "bootloader_id")}
        begin.update(image_class="receiver", campaign=self.journal.campaign,
                     artifact_sha256=bytes.fromhex(manifest["artifact_sha256"]),
                     mcuboot_image_hash=bytes.fromhex(manifest["mcuboot_image_hash"]))
        self.journal.emit("PC_SOURCE_OPEN", self.lead, manifest=manifest)
        result = self.request("stage_begin", begin)
        if result.get("state", "").upper() == "ACCEPTED":
            self._poll(lambda state: state.get("phase", state.get("state")) == "SOURCE_OPEN", "PC source open")
        elif result.get("state", "").upper() != "SOURCE_OPEN":
            raise CampaignError("Lead did not open a bounded PC source")
        result = self.request("stage_end", {"campaign": self.journal.campaign})
        if result.get("state", "").upper() == "ACCEPTED":
            self._poll(lambda state: state.get("phase", state.get("state")) == "SOURCE_READY", "PC source readiness")
        elif result.get("state", "").upper() != "SOURCE_READY":
            raise CampaignError("Lead did not accept the PC source")

    def _serve_source(self, result):
        length = result.get("source_length", 0)
        if type(length) is not int or not 0 <= length <= 256:
            raise CampaignError("invalid source credit length")
        if not length:
            return False
        offset = result.get("source_offset")
        if (result.get("source_campaign") != self.journal.campaign or type(offset) is not int
                or offset < 0 or offset + length > len(self.artifact)):
            raise CampaignError("invalid source credit campaign or bounds")
        # Retransmissions can request older offsets. The immutable PC bytes
        # remain the authority; only the exact requested block is sent.
        reply = self.request("stage_data", {"campaign": self.journal.campaign,
            "offset": offset, "data": self.artifact[offset:offset + length]})
        if reply.get("offset") != offset + length:
            raise CampaignError("Lead did not accept the exact source credit")
        return True

    def _all_valid(self, result):
        current = {identity(row["identity"]): row for row in result["targets"]}
        return result.get("phase") == "ALL_VALIDATED" and set(current) == set(self.targets) and all(
            row.get("validated") is True and row.get("flash_complete") is True
            and row.get("offset") == self.manifest["artifact_size"] for row in current.values())

    def _wait_commit_reboot(self):
        """COMMIT accepts work asynchronously; observe its result before recovery."""
        deadline = self.clock() + self.timeout
        while self.clock() < deadline:
            try:
                result = self._collect()
                self._table(result)
                phase = str(result.get("phase", result.get("state", ""))).upper()
                if phase in ("FAILED", "ABORTED", "PARTIAL") or any(
                        row.get("error") not in (None, 0, "", "NONE") for row in result["targets"]):
                    # Keep the firmware's original error and participant state.
                    # Reconciliation while this campaign is busy only yields
                    # OTA_ERR_STATE and hides e.g. a failed journal commit.
                    raise CampaignError("commit/reboot failed: %s" % result)
                if phase in ("RECOVERY_REQUIRED", "POSTBOOT_CHECK", "SUCCESS"):
                    return
            except TransportError:
                # A lost COMMIT ACK or disappearing USB port does not establish
                # whether activation occurred. Follow only the selected Lead,
                # then observe its boot state without issuing another command.
                if not self.reconnect:
                    break
                try:
                    self.transport = self.reconnect()
                    info = self.request("info")
                    if identity(info.get("identity")) != self.lead:
                        raise CampaignError("different Lead after reconnect")
                except TransportError:
                    pass
            self.sleep(self.poll_interval)
        raise CampaignError("PARTIAL: bounded timeout waiting for commit/reboot; "
                            "activation remains uncertain, preserve the campaign journal")

    def reconcile(self):
        deadline = self.clock() + self.timeout
        expected_hash = self.manifest["mcuboot_image_hash"]
        self.journal.emit("RECONCILE_BEGIN", targets=self.targets)
        good = set()
        while self.clock() < deadline:
            try:
                result = self._collect("reconcile", {"campaign": self.journal.campaign,
                    "targets": self.targets, "mcuboot_image_hash": bytes.fromhex(expected_hash)})
                good = set()
                for row in result["targets"]:
                    target = identity(row["identity"])
                    active_hash = row.get("mcuboot_image_hash")
                    if isinstance(active_hash, bytes):
                        active_hash = active_hash.hex()
                    version = str(row.get("version", ""))
                    if version and "+" not in version:
                        version += "+0"
                    valid = (target in self.targets and active_hash == expected_hash
                             and version == self.manifest["version"]
                             and row.get("build_id") == self.manifest["build_id"]
                             and row.get("healthy") is True and row.get("confirmed") is True)
                    row["postboot"] = "SUCCESS" if valid else "WRONG_IMAGE_OR_UNHEALTHY"
                    if valid:
                        good.add(target)
                self._table(result)
                if good == set(self.targets) and result.get("phase") == "SUCCESS":
                    self.journal.emit("SUCCESS", targets=self.targets)
                    return "SUCCESS"
            except TransportError:
                if not self.reconnect:
                    break
                try:
                    self.transport = self.reconnect()
                    info = self.request("info")
                    if identity(info.get("identity")) != self.lead:
                        raise CampaignError("different Lead after reconnect")
                except TransportError:
                    pass
            self.sleep(self.poll_interval)
        for target in set(self.targets) - good:
            self.rows.setdefault(target, {"identity": target})["postboot"] = "PARTIAL/TIMEOUT"
        self._table({"phase": "PARTIAL", "targets": list(self.rows.values())})
        self.journal.emit("PARTIAL", missing_or_failed=sorted(set(self.targets) - good))
        raise CampaignError("PARTIAL: not every frozen identity returned with the expected healthy, confirmed image")

    def run(self):
        if self.manifest.get("profile", {}).get("receiver_can_update_enabled") is False:
            raise CampaignError("Receiver artifact was built with CAN updates disabled "
                                "(CONFIG_OWNTECH_OTA_DEFERRED_ARM_QUALIFIED=n). "
                                "Build the receiver for the validated bootloader or the explicit OTA test bench; "
                                "install it over USB before starting a CAN campaign.")
        info = self.probe()
        if info.get("phase") == "WAITING_CAN":
            raise CampaignError("Lead waiting for CAN; connect and power the terminated 500 kbit/s bus "
                                "with another initialized board, then retry; no upload/reset was sent")
        if info.get("available") is not True or info.get("active_confirmed") is not True or info.get("slot_available") is not True:
            raise CampaignError("Lead not available/confirmed; preserve its rollback slot and reconcile first")
        if info.get("role") != "lead":
            raise CampaignError("dedicated Lead role is required")
        self.discover()
        try:
            self.stage()
            self.journal.emit("START_REQUEST", targets=self.targets)
            self.request("start", {"campaign": self.journal.campaign, "targets": self.targets})
            self._poll(self._all_valid, "CAN transfer and validation")
            self.journal.emit("COMMIT_REQUEST", targets=self.targets)
            # Mark before transmission: an ACK can be lost after acceptance.
            self.committed = True
            try:
                self.request("commit", {"campaign": self.journal.campaign})
            except TransportError:
                pass
            self._wait_commit_reboot()
            return self.reconcile()
        except Exception as error:
            if not self.committed:
                try:
                    self.request("abort", {"campaign": self.journal.campaign})
                except (TransportError, ProtocolError):
                    pass
                self.journal.emit("FAILED", error=str(error), targets=self.targets,
                                  activation_uncertain=self.committed)
            raise


def journal_campaign(path):
    """Recover the frozen set and target digest; never infer it from new peers."""
    inventory = None
    lead = None
    usb_serial = None
    frozen_lead = None
    frozen_serial = None
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        event = json.loads(line)
        if event.get("event") == "PROBE_LEAD":
            lead = event.get("identity")
        if event.get("event") == "USB_SELECTED":
            usb_serial = event.get("usb_serial")
        if event.get("event") == "DISCOVER":
            inventory = event
            frozen_lead = event.get("lead_identity") or lead
            frozen_serial = usb_serial
    if not inventory or not frozen_lead:
        raise CampaignError("journal has no complete frozen campaign inventory")
    return inventory, identity(frozen_lead), frozen_serial


def prepare_manifest(image_path, output_path=None, profile=None, version=None, build_id=None, image_class=None, *, usb=False):
    """Reuse verified build metadata; never overwrite contradictory provenance."""
    image_path = Path(image_path)
    output_path = Path(output_path) if output_path else image_path.with_suffix(".json")
    existing = None
    if output_path.is_file():
        existing = json.loads(output_path.read_text(encoding="utf-8"))
        if not isinstance(existing, dict):
            raise CampaignError("existing manifest must be an object")
    artifact = image_path.read_bytes()
    effective_profile = profile if profile is not None else (existing.get("profile") if existing else None)
    effective_build = build_id or (existing.get("build_id") if existing else None)
    effective_class = image_class or (existing.get("image_class") if existing else None)
    if effective_class not in ("receiver", "lead"):
        raise CampaignError("a generated manifest or explicit image class is required")
    inspector = inspect_usb_image if usb else inspect_image
    manifest = inspector(artifact, effective_profile, version, effective_build, effective_class,
                         **({"require_class": True} if usb else {}))
    manifest["filename"] = image_path.name
    if existing is not None:
        for field in ("schema_version", "protocol", "format", "activation_trailer", "image_class",
                      "artifact_size", "useful_size", "artifact_sha256", "mcuboot_image_hash",
                      "version", "build_id", "hardware_id", "layout_id", "bootloader_id"):
            if existing.get(field) != manifest[field]:
                raise CampaignError("existing manifest contradicts %s; rebuild/regenerate it explicitly before a campaign" % field)
    else:
        output_path.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return artifact, manifest


def read_only_status(transport):
    """Take one complete snapshot without discovering, adopting a role or writing."""
    info = transport.request("info", {})
    if info.get("service") != "owntech-ota" or info.get("protocol") != 2:
        raise CampaignError("incompatible application service")
    result = transport.request("status", {})
    rows = list(result.get("targets", []))
    count = result.get("target_count", len(rows))
    if type(count) is not int or not 0 <= count <= 16:
        raise CampaignError("invalid bounded target count")
    if len(rows) < count:
        rows = []
        for index in range(count):
            rows.extend(transport.request("status", {"index": index}).get("targets", []))
    identities = [identity(row.get("identity")) for row in rows]
    if len(identities) != count or len(set(identities)) != count:
        raise CampaignError("incomplete or duplicate status snapshot; retry read-only status")
    result["targets"] = rows
    return {"info": info, "status": result}


def main(argv=None, *, raise_errors=False):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path)
    parser.add_argument("--bootstrap-image", type=Path,
                        help="retired: install the dedicated Lead separately with USB_LEAD ota_init")
    parser.add_argument("--profile", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--serial")
    parser.add_argument("--port")
    parser.add_argument("--expected-id", action="append")
    parser.add_argument("--expected-count", type=int)
    parser.add_argument("--journal", type=Path)
    parser.add_argument("--timeout", type=float, default=180)
    parser.add_argument("--build-id")
    parser.add_argument("--version")
    parser.add_argument("--receiver-absent", action="store_true",
                        help="retired: campaigns never bootstrap or flash the Lead")
    parser.add_argument("--mcumgr", type=Path)
    recovery = parser.add_mutually_exclusive_group()
    recovery.add_argument("--status", action="store_true", help="read info/status only; no discovery, upload, role change or reset")
    recovery.add_argument("--reconcile-journal", type=Path, help="verify an existing frozen campaign without upload/reset")
    recovery.add_argument("--abort-journal", type=Path, help="stop transfer; inspect boot state if commit was already requested")
    args = parser.parse_args(argv)
    journal = None
    connection = None
    try:
        if args.status:
            connection = USBConnection(args.serial, args.port)
            transport = connection.connect()
            print(json.dumps(read_only_status(transport), default=json_value, indent=2, sort_keys=True))
            return 0
        recovery_path = args.reconcile_journal or args.abort_journal
        if not recovery_path and not args.expected_id and not args.expected_count:
            raise CampaignError("supply --expected-id for each card or --expected-count excluding Lead")
        if args.expected_count is not None and not 1 <= args.expected_count <= 16:
            raise CampaignError("expected count must be between 1 and 16")
        if args.timeout <= 0:
            raise CampaignError("timeout must be positive")
        if recovery_path:
            inventory, lead, usb_serial = journal_campaign(recovery_path)
            journal = Journal(recovery_path, inventory["campaign"])
            connection = USBConnection(args.serial or usb_serial, args.port)
            transport = connection.connect()
            client = Campaign(transport, inventory["manifest"], b"", journal,
                              inventory["targets"], reconnect=connection.reconnect, timeout=args.timeout)
            client.probe()
            if client.lead != lead:
                raise CampaignError("recovery selected a different Lead identity")
            client.targets = inventory["targets"]
            if args.abort_journal:
                client.request("abort", {"campaign": journal.campaign})
                journal.emit("ABORTED", activation_uncertain=True)
                print("ABORTED; maintenance remains; inspect boot state if commit was requested")
            else:
                print(client.reconcile())
            return 0
        if not args.image:
            raise CampaignError("--image is required for a new campaign")
        artifact, manifest = prepare_manifest(args.image, args.manifest,
                                             load_profile(args.profile) if args.profile else None,
                                             args.version, args.build_id)
        if manifest["image_class"] != "receiver":
            raise CampaignError("the campaign artifact must be receiver class")
        campaign_id = secrets.randbits(63) or 1
        journal = Journal(args.journal or Path.cwd() / "ota-journals" / ("campaign-%016x.jsonl" % campaign_id), campaign_id)
        connection = USBConnection(args.serial, args.port)
        journal.emit("USB_SELECTED", usb_serial=connection.serial_number)
        journal.emit("PROBE_LEAD", usb_serial=connection.serial_number, state="BEGIN")
        if args.receiver_absent or args.bootstrap_image:
            raise CampaignError("campaigns never install a Lead; run USB_LEAD ota_init separately")
        transport = connection.connect()
        probe = connection.last_info
        if probe.get("service") != "owntech-ota" or probe.get("protocol") != 2:
            raise CampaignError("incompatible response; refusing blind bootstrap")
        campaign = Campaign(transport, manifest, artifact, journal, args.expected_id, args.expected_count,
                            reconnect=connection.reconnect, timeout=args.timeout)
        result = campaign.run()
        print(result + "; journal: " + str(journal.path))
        return 0
    except (OSError, ValueError, CampaignError, UploadError, subprocess.SubprocessError) as error:
        if journal:
            journal.emit("PARTIAL" if str(error).startswith("PARTIAL") else "FAILED", error=str(error))
        if raise_errors:
            raise  # Preserve the journal, then let the assistant show the cause.
        print("Lead update failed: %s" % error, file=sys.stderr)
        return 1
    finally:
        if connection and connection.transport:
            connection.transport.close()
        if journal:
            journal.close()


if __name__ == "__main__":
    raise SystemExit(main())
