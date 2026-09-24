"""Provision one selected board using its existing MCUboot application uploader."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_pio import artifact_post_action, provision_action, register_usb_init

if "upload" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")


env.AddPostAction("$BUILD_DIR/${PROGNAME}.mcuboot.bin", artifact_post_action)
register_usb_init(env)
# SCons resolves a callable construction variable as a FunctionAction. Keeping
# this structured avoids shell quoting and uses the framework's signed source.
env.Replace(UPLOADCMD=provision_action)
