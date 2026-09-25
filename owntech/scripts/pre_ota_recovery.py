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
transition = env.subst("$PIOENV") == "OTA_TRANSITION"
# VS Code queries every environment's metadata before any scoped operation
# exists. Export the utility's build task without configuring CMake or creating
# placeholder guards. Actual build/upload invocations still pass every guard
# below; this early exit applies only to PlatformIO's metadata request.
if getattr(env, "IsIntegrationDump", lambda: False)():
    env.AddPlatformTarget(name="mcuboot-image", dependencies=[], actions=[],
                          title="Generate MCUboot Image",
                          description="Prepare a scoped OwnTech operation before building this utility",
                          always_build=False)
    data = env.DumpIntegrationData(env)
    path = Path(env.subst("$BUILD_DIR")) / "idedata.json"
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data), encoding="utf-8")
    print("\n" + json.dumps(data) + "\n")
    env.Exit(0)
config = project / env.GetProjectOption("custom_ota_recovery_config", ".pio/ota-recovery-config")
if not (config / "owntech_ota_recovery_config.h").is_file():
    raise ValueError("Generate the scoped configuration with prepare_ota_transition.py or prepare_ota_recovery.py first")
header = (config / "owntech_ota_recovery_config.h").read_bytes()
metadata = json.loads((config / "owntech_ota_recovery_config.json").read_text(encoding="utf-8"))
digest = hashlib.sha256(header).hexdigest()
if metadata.get("header_sha256") != digest:
    raise ValueError("Recovery header and metadata disagree; regenerate from the saved campaign journal")
if transition != (metadata.get("mode") == "transition_v2"):
    raise ValueError("OTA_TRANSITION and OTA_RECOVERY configurations cannot be interchanged")
if transition:
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from prepare_ota_transition import verify_config
    verify_config(config / "owntech_ota_recovery_config.json")

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
env["OWNTECH_OTA_BUILD_ID"] = ("transition-" if transition else "recovery-") + digest[:20]


def verify_recovery_entry(source, target, env):
    if (config / "owntech_ota_recovery_config.h").read_bytes() != header:
        raise ValueError("Guard configuration changed during compilation; rebuild this operation")
    if transition:
        verify_config(config / "owntech_ota_recovery_config.json")
    # PlatformIO normally substitutes its own src/ build for CMake's app
    # library. Fail closed if that accidentally selects the user's main.
    toolchain = Path(env.PioPlatform().get_package_dir("toolchain-gccarmnoneeabi"))
    nm = toolchain / "bin" / ("arm-none-eabi-nm.exe" if os.name == "nt" else "arm-none-eabi-nm")
    elf = env.subst("$BUILD_DIR/${PROGNAME}.elf")
    symbols = subprocess.run([str(nm), "-C", "--defined-only", elf],
                             capture_output=True, text=True, check=True).stdout
    expected_entry = "ota_transition_run(" if transition else "ota_recovery_run("
    forbidden_entry = "ota_recovery_run(" if transition else "ota_transition_run("
    if (expected_entry not in symbols or forbidden_entry in symbols or "setup_routine(" in symbols
            or "_img_validation" in symbols or "initialize_runtime(" in symbols):
        raise ValueError("Recovery ELF has the wrong entry point or an automatic confirmation path")
    return 0


env.AddPostAction(env.Alias("mcuboot-image"), verify_recovery_entry)
env.AddPostAction(env.Alias("mcuboot-image"), artifact_post_action)


def reject_blind_upload(*args, **kwargs):
    print("OTA_RECOVERY / OTA_TRANSITION are build-only. Follow the USB/OTA guide to verify the "
          "selected bootloader and its slots before any erase/upload.")
    return 1


env.Replace(UPLOADCMD=reject_blind_upload)
