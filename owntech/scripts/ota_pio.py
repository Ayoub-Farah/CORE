"""Shared signed-artifact checks and options for both PlatformIO OTA workflows."""
import json
from pathlib import Path
import platform
import sys

def artifact_options(env):
    """Resolve existing signing provenance identically for builds and campaigns."""
    project = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR"))
    sys.path.insert(0, str(project / "owntech" / "scripts"))
    from ota_artifact import DEFAULT_PROFILE, load_profile
    version = env.get("OWNTECH_OTA_VERSION")
    build_id = env.get("OWNTECH_OTA_BUILD_ID")
    if not version or not build_id:
        raise ValueError("pre_ota_identity.py must generate the application identity before OTA hooks")
    explicit_profile = env.GetProjectOption("custom_ota_profile", "")
    if explicit_profile:
        profile_path = Path(explicit_profile)
        if not profile_path.is_absolute():
            profile_path = project / profile_path
        profile = load_profile(profile_path)
    else:
        # Mirror the framework's existing key resolution for provenance. This
        # does not create, replace or extract any private key.
        framework = Path(env.PioPlatform().get_package_dir("framework-zephyr"))
        key = env.BoardConfig().get("build.zephyr.bootloader.signature_key_file", "")
        if not key:
            config = build / "zephyr" / ".config"
            if config.is_file():
                for line in config.read_text(encoding="utf-8").splitlines():
                    if line.startswith("CONFIG_MCUBOOT_SIGNATURE_KEY_FILE="):
                        key = line.split("=", 1)[1].strip('"')
        if not key or (not Path(key).is_absolute() and not Path(key).is_file()):
            key = str(framework / "_pio" / "bootloader" / "mcuboot" / "root-rsa-2048.pem")
        profile = dict(DEFAULT_PROFILE, signing_key=str(Path(key).resolve()))
        profile_path = build / "ota-profile.json"
        content = json.dumps(profile, indent=2) + "\n"
        build.mkdir(parents=True, exist_ok=True)
        if not profile_path.is_file() or profile_path.read_text(encoding="utf-8") != content:
            profile_path.write_text(content, encoding="utf-8")
    return profile, profile_path, version, build_id


def artifact_post_action(source, target, env):
    """Fail the ordinary build if its exact signed artifact cannot be staged."""
    try:
        profile, _, version, build_id = artifact_options(env)
        from ota_artifact import inspect_image
        image = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
        artifact = image.read_bytes()
        manifest = inspect_image(artifact, profile, version, build_id)
        manifest["filename"] = image.name
        content = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        image.with_suffix(".json").write_text(content, encoding="utf-8")
        environment = env.subst("$PIOENV")
        if not environment or "$" in environment or Path(environment).name != environment or environment in (".", ".."):
            raise ValueError("cannot resolve a safe PlatformIO environment for the OTA snapshot")
        snapshots = Path(env.subst("$PROJECT_DIR")) / ".pio" / "ota-artifacts" / environment
        snapshots.mkdir(parents=True, exist_ok=True)
        # Copy the bytes already inspected, never reread a concurrently changed
        # build output. This directory survives PlatformIO clean_build_dir().
        (snapshots / image.name).write_bytes(artifact)
        (snapshots / image.with_suffix(".json").name).write_text(content, encoding="utf-8")
        print("OTA artifact checked: %d useful / %d transmitted bytes" % (manifest["useful_size"], manifest["artifact_size"]))
        return 0
    except (OSError, ValueError) as error:
        print("OTA artifact rejected: %s" % error)
        return 1


def connection_options(env):
    """A stable board serial selects hardware; a port only selects its interface."""
    serial = env.GetProjectOption("custom_ota_serial", "") or env.GetProjectOption("board_id", "")
    port = env.GetProjectOption("custom_ota_port", "") or env.GetProjectOption("upload_port", "")
    args = []
    for value, flag in ((serial, "--serial"), (port, "--port")):
        if value:
            args.extend([flag, str(value)])
    return args


def mcumgr_path(env):
    project = Path(env.subst("$PROJECT_DIR"))
    executable = "mcumgr.exe" if platform.system() == "Windows" else (
        "mcumgr-mac" if platform.system() == "Darwin" else (
            "mcumgr-rpi" if platform.machine() in ("armv7l", "aarch64") else "mcumgr"))
    return str(env.GetProjectOption("custom_ota_mcumgr", str(project / "owntech" / "third_party" / executable)))
