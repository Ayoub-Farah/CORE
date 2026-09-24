"""Provision one selected board using its existing MCUboot application uploader."""
from pathlib import Path
import sys

from SCons.Script import COMMAND_LINE_TARGETS

Import("env")
sys.path.insert(0, str(Path(env.subst("$PROJECT_DIR")) / "owntech" / "scripts"))
from ota_pio import artifact_options, artifact_post_action, connection_options, mcumgr_path

if "upload" in COMMAND_LINE_TARGETS and "mcuboot-image" not in COMMAND_LINE_TARGETS:
    COMMAND_LINE_TARGETS.insert(0, "mcuboot-image")


def provision_action(source, target, env):
    project = Path(env.subst("$PROJECT_DIR"))
    sys.path.insert(0, str(project / "owntech" / "tools"))
    from provision_ota import main
    _, profile_path, version, build_id = artifact_options(env)
    args = ["--image", env.subst("$BUILD_DIR/${PROGNAME}.mcuboot.bin"),
            "--profile", str(profile_path), "--version", version, "--build-id", build_id,
            "--mcumgr", mcumgr_path(env)] + connection_options(env)
    timeout = env.GetProjectOption("custom_ota_timeout", "")
    if timeout:
        args.extend(["--timeout", str(timeout)])
    return main(args)


env.AddPostAction("$BUILD_DIR/${PROGNAME}.mcuboot.bin", artifact_post_action)
# SCons resolves a callable construction variable as a FunctionAction. Keeping
# this structured avoids shell quoting and uses the framework's signed source.
env.Replace(UPLOADCMD=provision_action)
