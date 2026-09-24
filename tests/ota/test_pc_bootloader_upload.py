"""Uploader watchdog tests use local Python children, never a serial device."""
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from bootloader_upload import UploadError, upload_image, _progress, _stop_process


class BootloaderUploadTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="ota uploader test ")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.image = self.directory / "firmware with spaces.bin"
        self.image.write_bytes(b"\xff" * 227328)
        self.helper = self.directory / "fake uploader.py"
        self.lines = []

    def run_helper(self, source, **kwargs):
        self.helper.write_text("import sys, time\n" + source, encoding="utf-8")
        return upload_image([sys.executable, "-u", str(self.helper)], self.image,
                            output=self.lines.append, **kwargs)

    def test_success_handles_cr_lf_rounded_units_and_exact_argv(self):
        self.run_helper(
            "assert sys.argv[1:3] == ['image', 'upload']\n"
            "assert sys.argv[3].endswith('firmware with spaces.bin')\n"
            "sys.stdout.write('0 / 227328 0.00%\\r113664 / 227328 50.00%\\r\\n')\n"
            "sys.stdout.write('222.00 KiB / 222.00 KiB 100.00%\\r')\n")
        self.assertEqual(self.lines, ["0 / 227328 0.00%", "113664 / 227328 50.00%",
                                     "222.00 KiB / 222.00 KiB 100.00%"])

    def test_complete_line_without_final_newline(self):
        self.run_helper("sys.stdout.write('227328 / 227328 100.00%')\n")

    def test_repeated_zero_does_not_reset_stall_timeout_and_child_is_reaped(self):
        captured = []
        real_popen = subprocess.Popen

        def popen(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            captured.append(process)
            return process

        before = time.monotonic()
        with patch("bootloader_upload.subprocess.Popen", side_effect=popen):
            with self.assertRaisesRegex(UploadError, "no progress.*0 B / 222.00 KiB"):
                self.run_helper("while True:\n print('0 B / 222.00 KiB 0.00%', end='\\r', flush=True)\n time.sleep(.01)\n",
                                stall_timeout=0.5, total_timeout=5)
        self.assertLess(time.monotonic() - before, 4)
        self.assertIsNotNone(captured[0].returncode)
        self.assertTrue(captured[0].stdout.closed)
        self.assertFalse(any(thread.name == "mcumgr-output" for thread in threading.enumerate()))

    def test_progress_resets_stall_timer(self):
        self.run_helper("for n in (56832, 113664, 170496, 227328):\n"
                        " print(f'{n} / 227328 {n / 227328 * 100:.2f}%', flush=True)\n"
                        " time.sleep(.2)\n", stall_timeout=0.5, total_timeout=5)
        self.assertEqual(len(self.lines), 4)

    def test_total_timeout_applies_even_with_continuing_progress(self):
        with self.assertRaisesRegex(UploadError, "total timeout"):
            self.run_helper("for n in range(1, 227328):\n"
                            " print(f'{n} / 227328', flush=True)\n time.sleep(.03)\n",
                            stall_timeout=1, total_timeout=0.5)

    def test_success_exit_without_complete_progress_is_rejected(self):
        with self.assertRaisesRegex(UploadError, "without reporting a complete"):
            self.run_helper("print('0 B / 222.00 KiB 0.00%')\n")

    def test_nonzero_exit_preserves_stderr_and_rejects_prior_completion(self):
        with self.assertRaisesRegex(UploadError, "code 7.*serial transport failed"):
            self.run_helper("print('227328 / 227328 100.00%')\n"
                            "print('serial transport failed', file=sys.stderr)\nsys.exit(7)\n")

    def test_keyboard_interrupt_from_output_cleans_up_child(self):
        captured = []
        real_popen = subprocess.Popen

        def popen(*args, **kwargs):
            process = real_popen(*args, **kwargs)
            captured.append(process)
            return process

        self.helper.write_text("import time\nprint('ready', flush=True)\ntime.sleep(60)\n",
                               encoding="utf-8")
        with patch("bootloader_upload.subprocess.Popen", side_effect=popen):
            with self.assertRaises(KeyboardInterrupt):
                upload_image([sys.executable, "-u", str(self.helper)], self.image,
                             output=lambda _: (_ for _ in ()).throw(KeyboardInterrupt()))
        self.assertIsNotNone(captured[0].returncode)
        self.assertTrue(captured[0].stdout.closed)
        self.assertFalse(any(thread.name == "mcumgr-output" for thread in threading.enumerate()))

    def test_unresponsive_termination_escalates_to_kill_and_wait(self):
        process = Mock()
        process.poll.return_value = None
        process.wait.side_effect = [subprocess.TimeoutExpired("fake uploader", 1), -9]
        _stop_process(process)
        process.terminate.assert_called_once_with()
        process.kill.assert_called_once_with()
        self.assertEqual([call.kwargs for call in process.wait.call_args_list],
                         [{"timeout": 1}, {"timeout": 2}])

    def test_oversized_lines_and_error_history_remain_bounded(self):
        with self.assertRaises(UploadError) as failure:
            self.run_helper("print('x' * 100000)\nprint('last diagnostic', file=sys.stderr)\nsys.exit(2)\n")
        self.assertLess(len(str(failure.exception)), 5000)
        self.assertIn("last diagnostic", str(failure.exception))
        self.assertLessEqual(max(map(len, self.lines)), 8192)

    def test_wrong_size_or_impossible_percent_cannot_report_completion(self):
        for line in ("1 / 1 100.00%", "227328 / 227328 110%", "0 / 227328 100%"):
            progress = _progress(line, 227328)
            self.assertTrue(progress is None or progress[1] is False)
        self.assertEqual(_progress("222.00 KiB / 222.00 KiB 100.00%", 227328), (1, True))

    def test_invalid_timeout_or_empty_image_does_not_start_process(self):
        with patch("bootloader_upload.subprocess.Popen") as process:
            for timeout in (0, -1, float("nan"), float("inf")):
                with self.assertRaises(UploadError):
                    upload_image(["unused"], self.image, stall_timeout=timeout)
            self.image.write_bytes(b"")
            with self.assertRaisesRegex(UploadError, "empty"):
                upload_image(["unused"], self.image)
            process.assert_not_called()


if __name__ == "__main__":
    unittest.main()
