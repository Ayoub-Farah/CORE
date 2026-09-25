"""Shared signed-artifact checks and options for both PlatformIO OTA workflows."""
import json
from pathlib import Path
import platform
import subprocess
import shlex
import sys
from ota_archive import archive_build


def signing_python(build):
    """Reuse the interpreter selected by the existing Zephyr signing builder."""
    for line in (Path(build) / "CMakeCache.txt").read_text(encoding="utf-8").splitlines():
        if line.startswith("PYTHON_EXECUTABLE:FILEPATH="):
            executable = Path(line.split("=", 1)[1])
            if executable.is_file():
                return str(executable)
    raise ValueError("Zephyr signing Python is missing from its configured CMake cache")

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
    """Validate USB bytes and sign a distinct compact artifact from the raw binary."""
    try:
        profile, _, version, build_id = artifact_options(env)
        from ota_artifact import inspect_image, inspect_usb_image
        image = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
        artifact = image.read_bytes()
        image_class = env.get("OWNTECH_OTA_IMAGE_CLASS", "receiver")
        manifest = inspect_usb_image(artifact, profile, version, build_id, image_class,
                                     require_class=env.subst("$PIOENV") in ("OTA", "USB_LEAD"))
        manifest["filename"] = image.name
        content = json.dumps(manifest, indent=2, sort_keys=True) + "\n"
        image.with_suffix(".json").write_text(content, encoding="utf-8")
        environment = env.subst("$PIOENV")
        if not environment or "$" in environment or Path(environment).name != environment or environment in (".", ".."):
            raise ValueError("cannot resolve a safe PlatformIO environment for the OTA snapshot")
        snapshots = Path(env.subst("$PROJECT_DIR")) / "ota-artifacts" / environment
        snapshots.mkdir(parents=True, exist_ok=True)
        if environment in ("OTA", "USB_LEAD"):
            archive_build(Path(env.subst("$PROJECT_DIR")), artifact, manifest, environment)
        # Copy the bytes already inspected, never reread a concurrently changed
        # build output. This directory survives PlatformIO clean_build_dir().
        (snapshots / image.name).write_bytes(artifact)
        (snapshots / image.with_suffix(".json").name).write_text(content, encoding="utf-8")
        if environment in ("OTA", "USB_LEAD"):
            # Never truncate a padded file: use the same imgtool/key and raw
            # application source, but deliberately omit --pad for CAN v2.
            framework = Path(env.PioPlatform().get_package_dir("framework-zephyr"))
            imgtool = framework / "_pio/bootloader/mcuboot/scripts/imgtool.py"
            compact = Path(env.subst("$BUILD_DIR/${PROGNAME}.can.bin"))
            raw = Path(env.subst("$BUILD_DIR/${PROGNAME}.bin"))
            extra = shlex.split(env.BoardConfig().get("build.zephyr.bootloader.imgtool_extra_cmds", ""))
            if not extra:
                extra = ["--custom-tlv", "0xA0", image_class]
            if any(arg in ("--pad", "--confirm") for arg in extra):
                raise ValueError("compact signing forbids activation/padding options")
            subprocess.run([signing_python(image.parent), str(imgtool), "sign", "--key", profile["signing_key"], *extra,
                            "--header-size", str(profile["header_size"]), "--align", "8",
                            "--version", version, "--slot-size", str(profile["slot_size"]),
                            str(raw), str(compact)], check=True)
            compact_data = compact.read_bytes()
            compact_manifest = inspect_image(compact_data, profile, version, build_id, image_class)
            if compact_manifest["mcuboot_image_hash"] != manifest["mcuboot_image_hash"]:
                raise ValueError("USB and CAN artifacts do not describe the same application")
            compact_manifest["filename"] = compact.name
            compact_json = json.dumps(compact_manifest, indent=2, sort_keys=True) + "\n"
            archive_build(Path(env.subst("$PROJECT_DIR")), compact_data, compact_manifest, environment)
            compact.with_suffix(".json").write_text(compact_json, encoding="utf-8")
            (snapshots / compact.name).write_bytes(compact_data)
            (snapshots / compact.with_suffix(".json").name).write_text(compact_json, encoding="utf-8")
        print("OTA artifact checked: %d useful / %d transmitted bytes" % (manifest["useful_size"], manifest["artifact_size"]))
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
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


def provision_action(source, target, env, legacy_console=False):
    project = Path(env.subst("$PROJECT_DIR"))
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from provision_ota import main
    _, profile_path, version, build_id = artifact_options(env)
    args = ["--image", env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"),
            "--profile", str(profile_path), "--version", version, "--build-id", build_id,
            "--image-class", env.get("OWNTECH_OTA_IMAGE_CLASS", "receiver"),
            "--mcumgr", mcumgr_path(env)] + connection_options(env)
    timeout = env.GetProjectOption("custom_ota_timeout", "")
    if timeout:
        args.extend(["--timeout", str(timeout)])
    if legacy_console:
        args.append("--legacy-console")
    return main(args)


def usb_init_action(source, target, env):
    from ota_gui_tasks import run_workflow
    return run_workflow(env, "initialize")


def register_artifact_validation(env):
    # Pre-scripts run before STSTM32 resolves PROGNAME. Reuse the framework's
    # alias node, whose signed-file dependency is added with the final name.
    # A string here could instead create a file named "mcuboot-image".
    env.AddPostAction(env.Alias("mcuboot-image"), artifact_post_action)


def register_usb_init(env):
    """Build the signed snapshot before the assistant selects and initializes a board."""
    from SCons.Script import COMMAND_LINE_TARGETS
    if "ota_init" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
        COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")
    env.AddCustomTarget(
        name="ota_init",
        dependencies=env.Alias("mcuboot-image"),
        actions=[env.VerboseAction(usb_init_action, "Initializing the selected board over USB")],
        title="Initialize over USB",
        description="Build/sign, select one board and initialize its OTA application over USB",
        always_build=True,
    )
