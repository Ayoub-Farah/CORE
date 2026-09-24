"""PlatformIO Project Task: existing MCUboot build, then one USB/CAN campaign."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))

# The framework only creates its signing builder when this target is present.
# Reuse that builder and its current key; never invoke install_bootloader.
if "lead_update" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")


from ota_pio import artifact_options, artifact_post_action, connection_options, mcumgr_path


def lead_update_action(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from lead_update import main
    _, profile_path, version, build_id = artifact_options(env)
    image = Path(env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"))
    args = ["--image", str(image), "--profile", str(profile_path), "--version", version]
    if build_id:
        args.extend(["--build-id", build_id])
    args.extend(connection_options(env))
    options = {"custom_ota_expected_count": "--expected-count",
               "custom_ota_timeout": "--timeout", "custom_ota_bootstrap_image": "--bootstrap-image"}
    for option, flag in options.items():
        value = env.GetProjectOption(option, "")
        if value:
            args.extend([flag, str(value)])
    ids = env.GetProjectOption("custom_ota_expected_ids", "")
    for value in ids.replace(",", " ").split():
        args.extend(["--expected-id", value])
    if str(env.GetProjectOption("custom_ota_receiver_absent", "false")).lower() in ("true", "1", "yes"):
        args.extend(["--receiver-absent", "--mcumgr", mcumgr_path(env)])
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
