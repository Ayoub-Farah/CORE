"""Install the dedicated Lead, or build OTA and distribute only to receivers."""
from pathlib import Path
import sys
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_pio import (provision_action, register_artifact_validation,
                     register_usb_init)
from ota_gui_tasks import run_workflow

if "upload" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")
register_usb_init(env)
register_artifact_validation(env)
if "upload" in COMMAND_LINE_TARGETS:
    env.Depends(env.Alias("upload"), env.Alias("mcuboot-image"))
env.Replace(UPLOADCMD=provision_action)


def lead_update_action(source, target, env):
    return run_workflow(env, "can-update")


env.AddCustomTarget(
    name="lead_update", dependencies=[],
    actions=[env.VerboseAction(lead_update_action, "Updating CAN receiver boards")],
    title="Update CAN receiver boards",
    description="Build OTA receiver, serve bounded PC blocks, verify the frozen fleet; never flash the Lead",
    always_build=True,
)
