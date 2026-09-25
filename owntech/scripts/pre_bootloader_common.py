Import("env")
from pathlib import Path
import sys
from SCons.Script import COMMAND_LINE_TARGETS

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech/scripts"))
from ota_gui_tasks import GUI_ONLY_TARGETS

# Make sure mcuboot-image is in the target list when uploading,
# because pio scripts will behave incorrectly if it isn't
# (wrong file gets uploaded)
# Board assistants select/inspect the existing artifact before deciding which
# environment to build. Building USB here would overwrite that evidence first.
gui_only = bool(GUI_ONLY_TARGETS.intersection(COMMAND_LINE_TARGETS)) and "upload" not in COMMAND_LINE_TARGETS
if not gui_only and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")
