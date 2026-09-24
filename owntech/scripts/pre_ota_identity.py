"""Refresh OTA identity before Zephyr's cached CMake configuration is consulted."""

from pathlib import Path
import sys

Import("env")  # noqa: F821 - provided by SCons

sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_build_identity import generate_for_environment

identity = generate_for_environment(env)
print("OTA firmware identity: %s / %s" % (identity["version"], identity["build_id"]))
