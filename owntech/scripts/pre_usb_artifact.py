"""Attach plain USB artifact/build checks before a legacy USB upload can run."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from usb_artifact import usb_artifact_post_action

if env.subst("$PIOENV") != "USB":
    raise ValueError("pre_usb_artifact.py belongs only to the USB environment")
env.AddPostAction(env.Alias("mcuboot-image"), usb_artifact_post_action)
if "upload" in COMMAND_LINE_TARGETS:
    env.Depends(env.Alias("upload"), env.Alias("mcuboot-image"))
