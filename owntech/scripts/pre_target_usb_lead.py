"""Install the dedicated Lead, or build OTA and distribute only to receivers."""
from pathlib import Path
import subprocess
import sys
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_pio import (connection_options, provision_action, register_artifact_validation,
                     register_usb_init)

if "upload" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")
register_usb_init(env)
register_artifact_validation(env)
if "upload" in COMMAND_LINE_TARGETS:
    env.Depends(env.Alias("upload"), env.Alias("mcuboot-image"))
env.Replace(UPLOADCMD=provision_action)


def lead_update_action(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    # This nested build is a different environment and never runs an upload.
    subprocess.run([sys.executable, "-m", "platformio", "run", "-d", str(project),
                    "-e", "OTA", "-t", "mcuboot-image"], check=True)
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from lead_update import main
    image = project / "ota-artifacts" / "OTA" / "firmware.can.bin"
    args = ["--image", str(image)] + connection_options(env)
    for option, flag in (("custom_ota_expected_count", "--expected-count"),
                         ("custom_ota_timeout", "--timeout")):
        value = env.GetProjectOption(option, "")
        if value:
            args.extend([flag, str(value)])
    ids = env.GetProjectOption("custom_ota_expected_ids", "")
    for value in ids.replace(",", " ").split():
        args.extend(["--expected-id", value])
    return main(args)


env.AddCustomTarget(
    name="lead_update", dependencies=[],
    actions=[env.VerboseAction(lead_update_action, "Updating CAN receiver boards")],
    title="Update CAN receiver boards",
    description="Build OTA receiver, serve bounded PC blocks, verify the frozen fleet; never flash the Lead",
    always_build=True,
)
