"""Pure PlatformIO-to-wizard bridge; importing it never opens a board or UI."""
from pathlib import Path
import os
import shutil
import subprocess
import sys


GUI_ONLY_TARGETS = frozenset({
    "ota_to_usb", "ota_to_ota", "ota_board_status", "ota_reconcile", "lead_update",
})


def isolate_gui_signatures(env, targets):
    """A waiting outer SCons must not replace a nested build's signature DB."""
    if targets and set(targets).issubset(GUI_ONLY_TARGETS):
        name = ".sconsign-gui%d%d" % sys.version_info[:2]
        env.SConsignFile(str(Path(env.subst("$BUILD_DIR")) / name))


def _hidden_process():
    return {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}


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
        print("The OwnTech board assistant needs an installed desktop Python with Tcl/Tk "
              "and pyserial. Enable Tcl/Tk in the Python installer and add pyserial to that "
              "interpreter, then run this PlatformIO action again. No board action was sent.")
        return 1
    command = [executable, str(project / "owntech/tools/ota_workflow.py"), action,
               "--project", str(project), "--environment", env.subst("$PIOENV"),
               "--mcumgr", mcumgr_path(env), "--pio-python", sys.executable]
    try:
        return subprocess.run(command, cwd=str(project), check=False, **_hidden_process()).returncode
    except OSError as error:
        print("Could not open the OwnTech board assistant: %s" % error)
        return 1


def workflow_action(action):
    def invoke(source, target, env):
        return run_workflow(env, action)
    return invoke


def register_gui_tasks(env):
    environment = env.subst("$PIOENV")
    if environment not in ("USB", "OTA", "USB_LEAD"):
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
    for name, action, title, description in tasks:
        env.AddCustomTarget(name=name, dependencies=[],
                            actions=[env.VerboseAction(workflow_action(action), title)],
                            title=title, description=description, always_build=True)
