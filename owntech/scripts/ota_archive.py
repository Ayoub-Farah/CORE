"""Keep inspected application builds in immutable, content-addressed history.

The caller inspects the MCUboot bytes before archiving. This module verifies
archive consistency; it neither verifies signatures nor accesses a board.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import tempfile


class ArchiveError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise ArchiveError(message)


def _hash(value):
    return isinstance(value, str) and re.fullmatch(r"[0-9a-f]{64}", value) is not None


def _object(pairs):
    result = {}
    for key, value in pairs:
        _require(key not in result, "duplicate archive manifest key: " + key)
        result[key] = value
    return result


def _snapshot(data, manifest, environment):
    _require(isinstance(environment, str), "archive environment must be a name")
    expected_class = {"USB": "usb", "OTA": "receiver", "USB_LEAD": "lead"}.get(environment)
    _require(expected_class is not None, "unsupported archive environment")
    _require(isinstance(data, bytes) and bool(data), "archive requires nonempty inspected bytes")
    _require(isinstance(manifest, dict), "archive requires an inspected manifest")
    try:
        value = json.loads(json.dumps(manifest, allow_nan=False))
    except (ValueError, TypeError) as error:
        raise ArchiveError("manifest is not finite JSON data") from error
    digest = value.get("artifact_sha256")
    _require(_hash(digest) and digest == hashlib.sha256(data).hexdigest(), "archive artifact hash mismatch")
    _require(type(value.get("artifact_size")) is int and value["artifact_size"] == len(data),
             "archive artifact size mismatch")
    _require(type(value.get("schema_version")) is int and value["schema_version"] == 2
             and type(value.get("protocol")) is int and value["protocol"] == 2, "only v2 manifests can be archived")
    _require(value.get("image_class") == expected_class, "archive environment/image class mismatch")
    _require(_hash(value.get("mcuboot_image_hash")) and value["mcuboot_image_hash"] != "0" * 64,
             "missing internal image hash")
    for field in ("version", "build_id", "artifact_hash_domain", "mcuboot_hash_domain"):
        _require(isinstance(value.get(field), str) and bool(value[field]), "missing archive identity: " + field)
    for field in ("useful_size", "header_size", "body_size", "hardware_id", "layout_id", "bootloader_id"):
        _require(type(value.get(field)) is int and value[field] > 0, "invalid archive field: " + field)
    _require(type(value.get("protected_tlv_size")) is int and value["protected_tlv_size"] >= 0,
             "invalid protected TLV size")
    profile = value.get("profile")
    _require(isinstance(profile, dict), "missing archive hardware profile")
    for field in ("slot_size", "useful_capacity", "header_size", "write_alignment", "max_alignment", "max_sectors",
                  "hardware_id", "layout_id", "bootloader_id"):
        _require(type(profile.get(field)) is int and profile[field] > 0, "invalid archive profile: " + field)
    for field in ("header_size", "hardware_id", "layout_id", "bootloader_id"):
        _require(value[field] == profile[field], "archive profile identity mismatch: " + field)
    _require(value["header_size"] + value["body_size"] + value["protected_tlv_size"] < value["useful_size"]
             <= profile["useful_capacity"] < profile["slot_size"], "invalid archive image bounds")
    form = value.get("format")
    _require(form in ("mcuboot-usb-padded", "mcuboot-compact"), "unsupported archive artifact format")
    padded = form == "mcuboot-usb-padded"
    _require(value.get("activation_trailer") is padded and len(data) ==
             (profile["slot_size"] if padded else value["useful_size"]), "archive format/size mismatch")
    signature = value.get("signature")
    _require(isinstance(signature, dict) and signature.get("type") in ("rsa2048", "rsa3072", "ecdsa", "ed25519")
             and _hash(signature.get("key_sha256")) and type(signature.get("verified")) is bool,
             "missing archive signing provenance")
    if environment == "USB":
        proof = value.get("build_proof")
        _require(padded and value.get("execution_profile") == "usb" and isinstance(proof, dict)
                 and proof.get("ota_enabled") is False and proof.get("recovery_enabled") is False
                 and proof.get("auto_confirmation") is True and _hash(proof.get("config_sha256"))
                 and _hash(proof.get("elf_sha256")), "missing plain USB build proof")
    value["filename"] = "firmware.bin"
    return value


def _semantic(manifest):
    value = json.loads(json.dumps(manifest))
    value.pop("filename", None)
    # File locations are not a signing identity. Key hashes, geometry, build
    # proofs and all other metadata still participate in immutable comparison.
    value["profile"].pop("signing_key", None)
    value["signature"].pop("signing_key", None)
    return value


def _publish(path, data):
    """Atomically publish one complete file without ever replacing an entry."""
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".archive-", suffix=".tmp", delete=False) as stream:
            temporary = Path(stream.name)
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        try:
            os.link(temporary, path)
        except FileExistsError:
            pass  # A concurrent publisher must pass the same checks below.
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def _read_manifest(path):
    try:
        return json.loads(path.read_bytes(), object_pairs_hook=_object,
                          parse_constant=lambda value: (_ for _ in ()).throw(ArchiveError("non-finite archive JSON")))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ArchiveError("corrupt archived manifest") from error


def archive_build(project, data: bytes, manifest: dict, environment: str):
    """Return immutable image/manifest paths; retain the first valid provenance."""
    incoming = _snapshot(data, manifest, environment)
    project = Path(project).resolve()
    _require(project.is_dir(), "archive project directory does not exist")
    directory = project
    for component in ("ota-artifacts", "history", environment, incoming["artifact_sha256"]):
        directory = directory / component
        _require(not directory.is_symlink() and directory.resolve().is_relative_to(project),
                 "archive directory escapes the project")
        directory.mkdir(exist_ok=True)
    image, metadata = directory / "firmware.bin", directory / "firmware.json"
    _require(not image.is_symlink() and not metadata.is_symlink(), "archive files cannot be symbolic links")
    _require(not metadata.exists() or image.exists(), "incomplete archive: manifest exists without its image")
    if image.exists():
        _require(image.is_file() and image.read_bytes() == data, "conflicting or corrupt archived image")
    else:
        _publish(image, data)
        _require(image.is_file() and image.read_bytes() == data, "concurrent archive image conflict")
    if not metadata.exists():
        encoded = (json.dumps(incoming, sort_keys=True, indent=2, allow_nan=False) + "\n").encode("utf-8")
        _publish(metadata, encoded)
    existing = _read_manifest(metadata)
    _require(isinstance(existing, dict) and existing.get("filename") == "firmware.bin", "invalid archive image filename")
    existing = _snapshot(data, existing, environment)
    _require(_semantic(existing) == _semantic(incoming), "conflicting archived manifest identity or build proof")
    return image, metadata
