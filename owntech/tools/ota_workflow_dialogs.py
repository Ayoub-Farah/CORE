"""Desktop dialogs for firmware workflows; never request terminal input."""
from collections.abc import Mapping
from importlib import import_module
from pathlib import Path


class Cancelled(Exception):
    """The operator cancelled or closed an input dialog."""


def _display(value):
    if value is None:
        return "not reported"
    if value is True:
        return "yes"
    if value is False:
        return "no"
    if isinstance(value, bytes):
        return value.hex()
    return str(value)


def _status_text(info):
    if not isinstance(info, Mapping):
        raise ValueError("Board status must be an object")
    fields = (("Identity (EUI-64)", "identity"), ("USB serial", "usb_serial"),
              ("Image class", "image_class"), ("Role", "role"), ("Phase", "phase"),
              ("Local health", "local_healthy"), ("Overall health", "healthy"),
              ("Image confirmed", "active_confirmed"), ("Maintenance", "maintenance"),
              ("CAN peer ready", "can_ready"), ("Available", "available"),
              ("Secondary slot available", "slot_available"), ("Version", "version"))
    lines = ["%s: %s" % (label, _display(info.get(key))) for label, key in fields]
    if "deferred_arm_qualified" in info:
        lines.append("CAN updates enabled in firmware: " + _display(info["deferred_arm_qualified"]))
    if "error" in info:
        lines.append("Reported error: " + _display(info["error"]))
    rows = None
    for key in ("fleet_rows", "fleet", "targets"):
        if key in info:
            rows = info[key]
            break
    if isinstance(rows, Mapping):
        rows = rows.get("rows", rows.get("targets"))
    if isinstance(rows, (list, tuple)):
        lines.extend(("", "Fleet:"))
        if not rows:
            lines.append("  No boards reported")
        for row in rows:
            if isinstance(row, Mapping):
                values = (("phase", row.get("phase", row.get("state"))),
                          ("health", row.get("healthy", row.get("local_healthy"))),
                          ("confirmed", row.get("confirmed", row.get("active_confirmed"))),
                          ("available", row.get("available")))
                lines.append("  %s | %s" % (_display(row.get("identity")),
                             " | ".join("%s: %s" % (name, _display(value)) for name, value in values)))
            else:
                lines.append("  " + _display(row))
    return "\n".join(lines)


class Dialogs:
    """Initialize Tk only when a dialog is first requested, on the main thread."""

    def __init__(self):
        self._root = None

    def _ensure(self):
        if self._root is not None:
            return
        root = None
        try:
            self._tk = import_module("tkinter")
            self._ttk = import_module("tkinter.ttk")
            self._messagebox = import_module("tkinter.messagebox")
            self._filedialog = import_module("tkinter.filedialog")
            self._simpledialog = import_module("tkinter.simpledialog")
            root = self._tk.Tk()
            root.withdraw()
            root.title("OwnTech Firmware")
            self._root = root
        except Exception as error:
            if root is not None:
                try:
                    root.destroy()
                except Exception:
                    pass
            raise RuntimeError(
                "The OwnTech firmware workflow requires Python Tcl/Tk and an interactive desktop. "
                "Install Tcl/Tk support in the desktop Python interpreter used for the assistant "
                "(the Tcl/Tk option in the Windows Python installer, or python3-tk on Linux), "
                "then run the workflow in a desktop session. This Python may differ from "
                "PlatformIO's build interpreter. Terminal input is not supported. "
                "Details: %s" % error) from error

    def continue_step(self, title, message):
        self._ensure()
        if not self._messagebox.askokcancel(title=title, message=message, parent=self._root, default="cancel"):
            raise Cancelled("Cancelled: " + title)

    def choose(self, title, message, items):
        items = list(items)
        if not items or any(not isinstance(item, (list, tuple)) or len(item) != 2
                            or not isinstance(item[1], str) or not item[1].strip() for item in items):
            raise ValueError("A choice requires one or more (value, label) items")
        labels = [item[1] for item in items]
        if len(set(labels)) != len(labels):
            raise ValueError("Choice labels must be distinct")
        self._ensure()
        window = self._tk.Toplevel(self._root)
        window.title(title)
        window.resizable(False, False)
        # Do not make this transient to the withdrawn root: some desktop
        # window managers would then hide the choice window too.
        missing = object()
        result = missing
        try:
            self._ttk.Label(window, text=message, wraplength=600, justify="left").pack(
                padx=20, pady=(20, 12), anchor="w")
            selector = self._ttk.Combobox(window, values=labels, state="readonly",
                                          width=min(90, max(36, max(map(len, labels)))))
            selector.current(0)
            selector.pack(padx=20, pady=(0, 16), fill="x")
            buttons = self._ttk.Frame(window)
            buttons.pack(padx=20, pady=(0, 16), anchor="e")

            def accept():
                nonlocal result
                index = selector.current()
                if type(index) is not int or not 0 <= index < len(items):
                    self._messagebox.showerror(title=title, message="Select an item from the list.", parent=window)
                    return
                result = items[index][0]
                window.destroy()

            def cancel():
                window.destroy()

            self._ttk.Button(buttons, text="Continue", command=accept).pack(side="left", padx=(0, 8))
            self._ttk.Button(buttons, text="Cancel", command=cancel).pack(side="left")
            window.protocol("WM_DELETE_WINDOW", cancel)
            window.bind("<Escape>", lambda event: cancel())
            window.wait_visibility()
            window.grab_set()
            selector.focus_set()
            self._root.wait_window(window)
        finally:
            if window.winfo_exists():
                window.destroy()
        if result is missing:
            raise Cancelled("Cancelled: " + title)
        return result

    def file(self, title, initial_dir, patterns):
        patterns = list(patterns)
        if not patterns or any(not isinstance(pattern, (list, tuple)) or len(pattern) != 2
                               or any(not isinstance(value, str) or not value for value in pattern)
                               for pattern in patterns):
            raise ValueError("File filters must be (label, pattern) pairs")
        self._ensure()
        value = self._filedialog.askopenfilename(title=title, initialdir=str(initial_dir),
                                                filetypes=patterns, parent=self._root)
        if not value:
            raise Cancelled("Cancelled: " + title)
        return Path(value)

    def folder(self, title, initial_dir):
        self._ensure()
        value = self._filedialog.askdirectory(title=title, initialdir=str(initial_dir),
                                             mustexist=True, parent=self._root)
        if not value:
            raise Cancelled("Cancelled: " + title)
        return Path(value)

    def number(self, title, message, minimum, maximum):
        if type(minimum) is not int or type(maximum) is not int or minimum > maximum:
            raise ValueError("Integer bounds must be ordered integers")
        self._ensure()
        value = self._simpledialog.askinteger(title=title, prompt=message, minvalue=minimum,
                                              maxvalue=maximum, parent=self._root)
        if value is None:
            raise Cancelled("Cancelled: " + title)
        if type(value) is not int or not minimum <= value <= maximum:
            raise ValueError("Selected number is outside the permitted integer range")
        return value

    def notice(self, title, message):
        self._ensure()
        self._messagebox.showinfo(title=title, message=message, parent=self._root)

    def problem(self, title, message):
        self._ensure()
        self._messagebox.showerror(title=title, message=message, parent=self._root)

    def show_status(self, title, info):
        self.notice(title, _status_text(info))

    def close(self):
        if self._root is not None:
            root, self._root = self._root, None
            root.destroy()
