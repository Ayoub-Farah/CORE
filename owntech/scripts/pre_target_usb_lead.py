"""PlatformIO Project Task: existing MCUboot build, then one USB/CAN campaign."""
import json
from pathlib import Path
import platform
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

# The framework only creates its signing builder when this target is present.
# Reuse that builder and its current key; never invoke install_bootloader.
if "lead_update" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")


def artifact_options(env):
    """Resolve existing signing provenance identically for builds and campaigns."""
    project = Path(env.subst("$PROJECT_DIR"))
    build = Path(env.subst("$BUILD_DIR"))
    sys.path.insert(0, str(project / "owntech" / "scripts"))
    from ota_artifact import DEFAULT_PROFILE, load_profile
    version = env.BoardConfig().get("build.zephyr.bootloader.app_version", "0.0.0")
    build_id = env.GetProjectOption("custom_ota_build_id", "") or None
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
        if not profile_path.is_file() or profile_path.read_text(encoding="utf-8") != content:
            profile_path.write_text(content, encoding="utf-8")
    return profile, profile_path, version, build_id


def artifact_post_action(source, target, env):
    """Fail the ordinary build if its exact signed artifact cannot be staged."""
    try:
        profile, _, version, build_id = artifact_options(env)
        from ota_artifact import inspect_image
        image = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
        manifest = inspect_image(image.read_bytes(), profile, version, build_id)
        manifest["filename"] = image.name
        image.with_suffix(".json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        print("OTA artifact checked: %d useful / %d transmitted bytes" % (manifest["useful_size"], manifest["artifact_size"]))
        return 0
    except (OSError, ValueError) as error:
        print("OTA artifact rejected: %s" % error)
        return 1


def lead_update_action(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from lead_update import main
    _, profile_path, version, build_id = artifact_options(env)
    image = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
    args = ["--image", str(image), "--profile", str(profile_path), "--version", version]
    if build_id:
        args.extend(["--build-id", build_id])
    options = {"custom_ota_serial": "--serial", "custom_ota_port": "--port", "custom_ota_expected_count": "--expected-count",
               "custom_ota_timeout": "--timeout", "custom_ota_bootstrap_image": "--bootstrap-image"}
    for option, flag in options.items():
        value = env.GetProjectOption(option, "")
        if value:
            args.extend([flag, str(value)])
    if "--serial" not in args:
        value = env.GetProjectOption("board_id", "")
        if value:
            args.extend(["--serial", value])
    ids = env.GetProjectOption("custom_ota_expected_ids", "")
    for value in ids.replace(",", " ").split():
        args.extend(["--expected-id", value])
    if str(env.GetProjectOption("custom_ota_receiver_absent", "false")).lower() in ("true", "1", "yes"):
        executable = "mcumgr.exe" if platform.system() == "Windows" else (
            "mcumgr-mac" if platform.system() == "Darwin" else (
                "mcumgr-rpi" if platform.machine() in ("armv7l", "aarch64") else "mcumgr"))
        mcumgr = env.GetProjectOption("custom_ota_mcumgr", str(project / "owntech" / "third_party" / executable))
        args.extend(["--receiver-absent", "--mcumgr", mcumgr])
    return main(args)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.mcuboot.bin", artifact_post_action)
env.AddCustomTarget(
    name="lead_update",
    dependencies=["$BUILD_DIR/${PROGNAME}.mcuboot.bin"],
    actions=[env.VerboseAction(lead_update_action, "Updating the frozen USB/CAN fleet")],
    title="Update Lead and CAN fleet",
    description="Build/sign, probe, stage exact image, broadcast, validate and verify every reboot",
    always_build=True,
)
