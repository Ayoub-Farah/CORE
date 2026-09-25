"""Firmware dialog semantics with mocked Tk; no display or hardware access."""
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from ota_workflow_dialogs import Cancelled, Dialogs


class Selector:
    def __init__(self):
        self.index = -1
        self.pack = Mock()
        self.focus_set = Mock()

    def current(self, index=None):
        if index is not None:
            self.index = index
        return self.index


class DialogTests(unittest.TestCase):
    def setUp(self):
        self.root = Mock()
        self.window = Mock()
        self.window.winfo_exists.return_value = True
        self.window.destroy.side_effect = lambda: setattr(self.window.winfo_exists, "return_value", False)
        self.tk = SimpleNamespace(Tk=Mock(return_value=self.root), Toplevel=Mock(return_value=self.window))
        self.selector = Selector()
        self.ttk = SimpleNamespace(Label=Mock(), Frame=Mock(), Button=Mock(),
                                   Combobox=Mock(return_value=self.selector))
        self.messagebox, self.filedialog, self.simpledialog = Mock(), Mock(), Mock()
        modules = {"tkinter": self.tk, "tkinter.ttk": self.ttk, "tkinter.messagebox": self.messagebox,
                   "tkinter.filedialog": self.filedialog, "tkinter.simpledialog": self.simpledialog}
        self.importer = Mock(side_effect=lambda name: modules[name])
        self.patch = patch("ota_workflow_dialogs.import_module", self.importer)
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.dialogs = Dialogs()
        self.root.wait_window.side_effect = lambda window: self.press("Continue")

    def press(self, name):
        for call in self.ttk.Button.call_args_list:
            if call.kwargs["text"] == name:
                call.kwargs["command"]()
                return
        raise AssertionError("No button named " + name)

    def test_initialization_is_lazy_and_reused_with_hidden_named_root(self):
        self.importer.assert_not_called()
        self.dialogs.notice("Ready", "Message")
        self.dialogs.problem("Stopped", "Reason")
        self.tk.Tk.assert_called_once_with()
        self.root.withdraw.assert_called_once_with()
        self.root.title.assert_called_once_with("OwnTech Firmware")
        self.messagebox.showinfo.assert_called_once_with(title="Ready", message="Message", parent=self.root)
        self.messagebox.showerror.assert_called_once_with(title="Stopped", message="Reason", parent=self.root)

    def test_tk_missing_or_desktop_failure_is_actionable_without_terminal_fallback(self):
        for error in (ImportError("No tkinter"), RuntimeError("no display")):
            self.importer.side_effect = error
            with self.subTest(error=error), self.assertRaisesRegex(RuntimeError, "Tcl/Tk.*interactive desktop") as caught:
                self.dialogs.notice("Ready", "Message")
            self.assertIn("desktop Python interpreter", str(caught.exception))
            self.assertIn("may differ from PlatformIO's build interpreter", str(caught.exception))
            self.assertIn("Terminal input is not supported", str(caught.exception))
        self.tk.Tk.assert_not_called()

    def test_failed_tk_root_setup_destroys_partial_root(self):
        self.root.withdraw.side_effect = RuntimeError("window system unavailable")
        with self.assertRaisesRegex(RuntimeError, "desktop"):
            self.dialogs.notice("Ready", "Message")
        self.root.destroy.assert_called_once()
        self.assertIsNone(self.dialogs._root)

    def test_physical_step_requires_ok_and_defaults_to_cancel(self):
        self.messagebox.askokcancel.return_value = True
        self.dialogs.continue_step("Enter bootloader", "Press BOOT and RESET.")
        self.messagebox.askokcancel.assert_called_once_with(title="Enter bootloader", message="Press BOOT and RESET.",
                                                           parent=self.root, default="cancel")
        self.messagebox.askokcancel.return_value = False
        with self.assertRaises(Cancelled):
            self.dialogs.continue_step("Enter bootloader", "Press BOOT and RESET.")

    def test_choice_is_readonly_modal_and_explicit_continue_returns_original_value(self):
        values = [(object(), "Board one — serial A"), (object(), "Board two — serial B")]
        def select_and_accept(window):
            self.assertEqual(self.selector.index, 0)
            self.selector.index = 1
            self.press("Continue")
        self.root.wait_window.side_effect = select_and_accept
        self.assertIs(self.dialogs.choose("Board", "Select a physical board", values), values[1][0])
        self.assertEqual(self.ttk.Combobox.call_args.kwargs["state"], "readonly")
        self.assertEqual(self.ttk.Combobox.call_args.kwargs["values"], [item[1] for item in values])
        self.window.grab_set.assert_called_once()
        self.root.wait_window.assert_called_once_with(self.window)
        self.window.destroy.assert_called_once()
        self.assertEqual([call.kwargs["text"] for call in self.ttk.Button.call_args_list], ["Continue", "Cancel"])

    def test_choice_cancel_button_window_close_and_escape_raise_cancelled(self):
        def close_by_protocol(window):
            self.window.protocol.call_args.args[1]()
        def escape(window):
            self.window.bind.call_args.args[1](object())
        for action in (lambda window: self.press("Cancel"), close_by_protocol, escape):
            self.window.winfo_exists.return_value = True
            self.root.wait_window.side_effect = action
            with self.subTest(action=action), self.assertRaises(Cancelled):
                self.dialogs.choose("Board", "Select one", [(1, "Board one")])
            self.assertEqual(self.window.protocol.call_args.args[0], "WM_DELETE_WINDOW")
            self.assertEqual(self.window.bind.call_args.args[0], "<Escape>")

    def test_choice_default_selection_does_not_accept_without_button(self):
        self.root.wait_window.side_effect = None
        with self.assertRaises(Cancelled):
            self.dialogs.choose("Board", "Select one", [(1, "Board one")])
        self.window.destroy.assert_called_once()

    def test_choice_invalid_index_keeps_dialog_open_until_cancelled(self):
        def invalid_then_cancel(window):
            self.selector.index = -1
            self.press("Continue")
            self.window.destroy.assert_not_called()
            self.messagebox.showerror.assert_called_once()
            self.press("Cancel")
        self.root.wait_window.side_effect = invalid_then_cancel
        with self.assertRaises(Cancelled):
            self.dialogs.choose("Board", "Select one", [(1, "Board one")])

    def test_choice_rejects_empty_bad_or_ambiguous_items_before_gui(self):
        for items in ([], [(1,)], [(1, "")], [(1, " ")], [(1, "same"), (2, "same")], ["wrong"]):
            with self.subTest(items=items), self.assertRaises(ValueError):
                self.dialogs.choose("Board", "Select one", items)
        self.importer.assert_not_called()

    def test_file_picker_preserves_filters_and_returns_path(self):
        self.filedialog.askopenfilename.return_value = "C:/firmware/signed.bin"
        patterns = [("Firmware images", "*.bin"), ("All files", "*")]
        self.assertEqual(self.dialogs.file("Signed image", Path("C:/firmware"), patterns), Path("C:/firmware/signed.bin"))
        self.filedialog.askopenfilename.assert_called_once_with(title="Signed image", initialdir=str(Path("C:/firmware")),
                                                               filetypes=patterns, parent=self.root)
        self.filedialog.askopenfilename.return_value = ""
        with self.assertRaises(Cancelled):
            self.dialogs.file("Signed image", Path("."), patterns)

    def test_file_picker_rejects_invalid_filters_before_gui(self):
        for patterns in ([], [("Firmware",)], [("Firmware", "")], ["*.bin"]):
            with self.subTest(patterns=patterns), self.assertRaises(ValueError):
                self.dialogs.file("Signed image", Path("."), patterns)
        self.importer.assert_not_called()

    def test_folder_picker_requires_existing_directory_and_cancel_is_not_dot(self):
        self.filedialog.askdirectory.return_value = "C:/artifacts"
        self.assertEqual(self.dialogs.folder("Archive", Path(".")), Path("C:/artifacts"))
        self.filedialog.askdirectory.assert_called_once_with(title="Archive", initialdir=".", mustexist=True, parent=self.root)
        self.filedialog.askdirectory.return_value = ""
        with self.assertRaises(Cancelled):
            self.dialogs.folder("Archive", Path("."))

    def test_number_picker_passes_bounds_accepts_boundaries_and_cancels(self):
        for value in (1, 16):
            self.simpledialog.askinteger.return_value = value
            self.assertEqual(self.dialogs.number("Board count", "Expected receivers", 1, 16), value)
        self.simpledialog.askinteger.assert_called_with(title="Board count", prompt="Expected receivers",
                                                        minvalue=1, maxvalue=16, parent=self.root)
        self.simpledialog.askinteger.return_value = None
        with self.assertRaises(Cancelled):
            self.dialogs.number("Board count", "Expected receivers", 1, 16)

    def test_number_rejects_invalid_bounds_before_gui_and_invalid_results(self):
        for minimum, maximum in ((True, 16), (1, False), (1.0, 16), (16, 1)):
            with self.subTest(bounds=(minimum, maximum)), self.assertRaises(ValueError):
                self.dialogs.number("Board count", "Expected receivers", minimum, maximum)
        self.importer.assert_not_called()
        for value in (True, "2", 0, 17):
            self.simpledialog.askinteger.return_value = value
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.dialogs.number("Board count", "Expected receivers", 1, 16)

    def test_status_preserves_unknown_and_false_fields_and_shows_fleet_rows(self):
        self.dialogs.show_status("Board status", {
            "identity": bytes.fromhex("0011223344556677"), "usb_serial": "physical-A",
            "image_class": "receiver", "phase": "WAITING_CAN", "local_healthy": True,
            "healthy": False, "active_confirmed": True, "maintenance": False,
            "can_ready": False, "available": False,
            "fleet_rows": [{"identity": "aabbccddeeff0011", "phase": "PREPARED", "healthy": True,
                            "active_confirmed": False, "available": False}]})
        message = self.messagebox.showinfo.call_args.kwargs["message"]
        for text in ("Identity (EUI-64): 0011223344556677", "USB serial: physical-A", "Image class: receiver",
                     "Phase: WAITING_CAN", "Local health: yes", "Overall health: no", "Image confirmed: yes",
                     "Maintenance: no", "CAN peer ready: no", "Available: no", "Version: not reported",
                     "Fleet:", "aabbccddeeff0011 | phase: PREPARED | health: yes | confirmed: no | available: no"):
            self.assertIn(text, message)

    def test_status_accepts_target_rows_and_empty_fleet_without_inventing_health(self):
        self.dialogs.show_status("Fleet", {"targets": [{"identity": "0011223344556677", "state": "IDLE"}]})
        message = self.messagebox.showinfo.call_args.kwargs["message"]
        self.assertIn("health: not reported", message)
        self.assertIn("phase: IDLE", message)
        self.dialogs.show_status("Fleet", {"fleet": {"rows": []}})
        self.assertIn("No boards reported", self.messagebox.showinfo.call_args.kwargs["message"])
        with self.assertRaises(ValueError):
            self.dialogs.show_status("Fleet", [])

    def test_fleet_confirmation_uses_protocol_field_with_legacy_fallback(self):
        # ota_usb.cpp observation() publishes targets[].confirmed, whereas
        # the board-level info response publishes active_confirmed.
        for row, expected in (({"confirmed": True}, "yes"),
                              ({"confirmed": False, "active_confirmed": True}, "no"),
                              ({"active_confirmed": True}, "yes"), ({}, "not reported")):
            with self.subTest(row=row):
                self.dialogs.show_status("Fleet", {"fleet": {"targets": [dict(row, identity="0011223344556677")]}})
                message = self.messagebox.showinfo.call_args.kwargs["message"]
                self.assertIn("| confirmed: " + expected + " |", message)

    def test_close_is_idempotent_and_never_initializes_gui(self):
        self.dialogs.close()
        self.importer.assert_not_called()
        self.dialogs.notice("Ready", "Message")
        self.dialogs.close()
        self.dialogs.close()
        self.root.destroy.assert_called_once_with()
        self.assertIsNone(self.dialogs._root)


if __name__ == "__main__":
    unittest.main()
