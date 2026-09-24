"""Provision one selected board using its existing MCUboot application uploader."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_pio import provision_action, register_artifact_validation, register_usb_init

if "upload" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")


register_artifact_validation(env)
register_usb_init(env)
# Both requested targets may otherwise run independently after signing. Make
# the framework upload wait for the shared artifact validation as well.
if "upload" in COMMAND_LINE_TARGETS:
    env.Depends(env.Alias("upload"), env.Alias("mcuboot-image"))
# SCons resolves a callable construction variable as a FunctionAction. Keeping
# this structured avoids shell quoting and uses the framework's signed source.
env.Replace(UPLOADCMD=provision_action)
