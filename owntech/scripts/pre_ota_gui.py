"""Expose board assistants in PlatformIO Project Tasks without running them."""
from pathlib import Path
import sys
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech/scripts"))
from ota_gui_tasks import isolate_gui_signatures, register_gui_tasks

isolate_gui_signatures(env, COMMAND_LINE_TARGETS)
register_gui_tasks(env)
