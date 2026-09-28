"""Pure PlatformIO-to-wizard bridge; importing it never opens a board or UI."""
from pathlib import Path
from datetime import datetime, timezone
import os
import shutil
import subprocess
import sys
import uuid


RECOVERY_TASKS = (
    ("ota_board_status", "status", "Check connected board",
     "Read the connected board's application status"),
    ("ota_recovery_inspect", "recovery-inspect", "Prepare and inspect receiver recovery",
     "Select a failed precommit campaign, build its recovery image and inspect one receiver"),
    ("ota_recovery_run", "recovery-run", "Recover interrupted receiver",
     "Guide one receiver through recovery and restoration of its original firmware"),
    ("ota_recovery_finish", "recovery-finish", "Finish receiver recovery",
     "Continue a saved receiver recovery without repeating an uncertain upload"),
    ("ota_recovery_boot_state", "recovery-boot-state", "Inspect recovery boot state",
     "Read bootloader image slots for a saved recovery; never erase, upload, confirm or reset"),
)
RECOVERY_GUI_TARGETS = frozenset(row[0] for row in RECOVERY_TASKS)
GUI_ONLY_TARGETS = frozenset({
    "ota_to_usb", "ota_to_ota", "ota_board_status", "ota_reconcile", "lead_update",
}) | RECOVERY_GUI_TARGETS


def isolate_gui_signatures(env, targets):
    """A waiting outer SCons must not replace a nested build's signature DB."""
    if targets and set(targets).issubset(GUI_ONLY_TARGETS):
        name = ".sconsign-gui%d%d" % sys.version_info[:2]
        env.SConsignFile(str(Path(env.subst("$BUILD_DIR")) / name))


def _hidden_process():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


def _console(message, *, end="\n"):
    """Keep draining the child even when a terminal cannot encode its text."""
    stream = sys.stdout
    if stream is None:
        return
    text = str(message) + end
    encoding = getattr(stream, "encoding", None)
    if encoding:
        text = text.encode(encoding, errors="backslashreplace").decode(encoding)
    stream.write(text)
    stream.flush()


def gui_python():
    """Locate installed desktop Python dependencies without creating a window."""
    candidates = [[sys.executable]]
    if os.name == "nt":
        launcher = shutil.which("py")
        if launcher:
            candidates.append([launcher, "-3"])
    base = getattr(sys, "_base_executable", None)
    if base:
        candidates.append([base])
    for name in ("python3", "python"):
        executable = shutil.which(name)
        if executable:
            candidates.append([executable])
    seen = set()
    probe = "import tkinter, serial, serial.tools.list_ports, sys; print(sys.executable)"
    for candidate in candidates:
        # Windows app-execution aliases can open the Store instead of Python.
        if os.name == "nt" and any(part.casefold() == "windowsapps" for part in Path(candidate[0]).parts):
            continue
        key = tuple(os.path.normcase(value) for value in candidate)
        if key in seen:
            continue
        seen.add(key)
        try:
            result = subprocess.run(candidate + ["-c", probe], capture_output=True,
                                    text=True, timeout=10, check=False, **_hidden_process())
        except (OSError, subprocess.TimeoutExpired):
            continue
        if result.returncode != 0:
            continue
        executable = result.stdout.strip()
        if executable and len(executable.splitlines()) == 1:
            path = Path(executable)
            if path.is_absolute() and path.is_file():
                return str(path)
    return None


def run_workflow(env, action):
    """Keep wizard lifetime outside SCons; propagate cancel/failure to its task."""
    from ota_pio import mcumgr_path
    project = Path(env.subst("$PROJECT_DIR"))
    executable = gui_python()
    if not executable:
        _console("The OwnTech board assistant needs an installed desktop Python with Tcl/Tk "
                 "and pyserial. Enable Tcl/Tk in the Python installer and add pyserial to that "
                 "interpreter, then run this PlatformIO action again. No board action was sent.")
        return 1
    command = [executable, "-u", str(project / "owntech/tools/ota_workflow.py"), action,
               "--project", str(project), "--environment", env.subst("$PIOENV"),
               "--mcumgr", mcumgr_path(env), "--pio-python", sys.executable]
    directory = project / "ota-artifacts/workflow-logs"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = directory / (stamp + "-" + uuid.uuid4().hex + ".log")
    try:
        directory.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8", newline="\n") as log:
            log.write("OwnTech board assistant\nAction: %s\nEnvironment: %s\nPython: %s\n\n" %
                      (action, env.subst("$PIOENV"), executable))
            log.flush()
            os.fsync(log.fileno())
            _console("OwnTech assistant log: " + str(path))
            child_env = dict(os.environ, PYTHONIOENCODING="utf-8", PYTHONUNBUFFERED="1")
            try:
                # CREATE_NO_WINDOW with implicit streams discards child output
                # on Windows. Explicit pipes also keep nested build/provision
                # failures visible in PlatformIO without opening a console.
                with subprocess.Popen(command, cwd=str(project), env=child_env,
                                      stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                      stderr=subprocess.STDOUT, text=True, encoding="utf-8",
                                      errors="replace", bufsize=1, **_hidden_process()) as process:
                    for line in process.stdout:
                        log.write(line)
                        log.flush()
                        _console(line, end="")
                    result = process.wait()
            except OSError as error:
                message = "Could not open the OwnTech board assistant: %s" % error
                log.write(message + "\n")
                _console(message)
                result = 1
            log.write("\nProcess exit code: %d\n" % result)
            log.flush()
            os.fsync(log.fileno())
        if result:
            _console("OwnTech assistant stopped (exit code %d). Details: %s" % (result, path))
        return result
    except OSError as error:
        _console("Could not save the OwnTech assistant log: %s" % error)
        return 1


def workflow_action(action):
    def invoke(source, target, env):
        return run_workflow(env, action)
    return invoke


def register_gui_tasks(env):
    environment = env.subst("$PIOENV")
    if environment not in ("USB", "OTA", "USB_LEAD", "OTA_RECOVERY"):
        raise ValueError("OwnTech board assistant requires USB, OTA or USB_LEAD")
    tasks = [
        ("ota_to_usb", "to-usb", "Switch to USB",
         "Select one board and return its confirmed OTA V2 application to USB"),
        ("ota_board_status", "status", "Check connected board",
         "Select one connected board and display its application status"),
    ]
    if environment in ("OTA", "USB_LEAD"):
        tasks.append(("ota_to_ota", "to-ota", "Return to OTA V2",
                      "Select one USB board and restore its OTA V2 application"))
    if environment == "USB_LEAD":
        tasks.append(("ota_reconcile", "reconcile", "Finish previous CAN update",
                      "Select the saved CAN campaign and verify every expected receiver"))
    if environment == "OTA_RECOVERY":
        tasks = RECOVERY_TASKS
    for name, action, title, description in tasks:
        env.AddCustomTarget(name=name, dependencies=[],
                            actions=[env.VerboseAction(workflow_action(action), title)],
                            title=title, description=description, always_build=True)
