"""Build the scoped USB repair image; never auto-select or reset a board."""
from pathlib import Path
import shlex
import hashlib
import json
import sys
import subprocess
import os

Import("env")

project = Path(env.subst("$PROJECT_DIR"))
config = project / ".pio" / "ota-recovery-config"
if not (config / "owntech_ota_recovery_config.h").is_file():
    raise ValueError("Run owntech/tools/prepare_ota_recovery.py --journal <campaign.jsonl> first")
header = (config / "owntech_ota_recovery_config.h").read_bytes()
metadata = json.loads((config / "owntech_ota_recovery_config.json").read_text(encoding="utf-8"))
digest = hashlib.sha256(header).hexdigest()
if metadata.get("header_sha256") != digest:
    raise ValueError("Recovery header and metadata disagree; regenerate from the saved campaign journal")

args = env.BoardConfig().get("build.zephyr.cmake_extra_args", [])
if isinstance(args, str):
    args = shlex.split(args)
args = [arg for arg in args if not arg.startswith("-DOWNTECH_RECOVERY_CONFIG_DIR=")]
args.append("-DOWNTECH_RECOVERY_CONFIG_DIR=" + config.resolve().as_posix())
env.BoardConfig().update("build.zephyr.cmake_extra_args", " ".join(shlex.quote(arg) for arg in args))
# This infrequent utility always reconfigures: its guard header, profile and
# exclusion of automatic image confirmation must never reuse stale Kconfig.
cache = Path(env.subst("$BUILD_DIR")) / "CMakeCache.txt"
if cache.is_file():
    cache.unlink()

# Reuse the ordinary exact signed-image checks and durable artifact snapshot.
# This identity belongs only to the repair artifact, not the normal application.
sys.path.insert(0, str(project / "owntech" / "scripts"))
from ota_pio import artifact_post_action
env["OWNTECH_OTA_VERSION"] = "0.0.1+0"
env["OWNTECH_OTA_BUILD_ID"] = "recovery-" + digest[:20]


def verify_recovery_entry(source, target, env):
    # PlatformIO normally substitutes its own src/ build for CMake's app
    # library. Fail closed if that accidentally selects the user's main.
    toolchain = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi"))
    nm = toolchain / "bin" / ("arm-none-eabi-nm.exe" if os.name == "nt" else "arm-none-eabi-nm")
    elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    symbols = subprocess.run([str(nm), "-C", "--defined-only", elf],
                             capture_output=True, text=True, check=True).stdout
    if ("ota_recovery_run(" not in symbols or "setup_routine(" in symbols
            or "_img_validation" in symbols or "initialize_runtime(" in symbols):
        raise ValueError("Recovery ELF has the wrong entry point or an automatic confirmation path")
    return 0


env.AddPostAction("$BUILD_DIR/${PROGNAME}.mcuboot.bin", verify_recovery_entry)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.mcuboot.bin", artifact_post_action)


def reject_blind_upload(*args, **kwargs):
    print("OTA_RECOVERY is build-only. Follow docs/ota-recovery.md to verify the "
          "selected bootloader and its slots before any erase/upload.")
    return 1


env.Replace(UPLOADCMD=reject_blind_upload)
