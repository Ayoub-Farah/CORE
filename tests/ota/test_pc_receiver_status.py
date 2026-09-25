"""Minimal receiver status retries are bounded and never transmit console data."""
import json
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech/tools"))
from lead_update import CampaignError, ReceiverStatus
from smp_transport import ProtocolError, ReceiverProbeTimeout


def status_line(**changes):
    info = {"service": "owntech-ota", "protocol": 2, "image_class": "receiver",
            "identity": "1ccd6d8a20769eae", **changes}
    return b"OTAR2 " + json.dumps(info).encode() + b"\n"


class ReceiverStatusTests(unittest.TestCase):
    def setUp(self):
        self.now = 0.0
        self.responses = {}
        self.requests = []
        self.baudrates = []
        test = self

        class Serial:
            def __init__(self, *args, **kwargs):
                self.timeout = kwargs["timeout"]
                self.baudrate = kwargs["baudrate"]
                self.reset_input_buffer = Mock()
                self.write = Mock(side_effect=AssertionError("status must not write console bytes"))
                self.close = Mock()

            @property
            def baudrate(self):
                return self._baudrate

            @baudrate.setter
            def baudrate(self, baudrate):
                self._baudrate = baudrate
                test.baudrates.append(baudrate)
                if baudrate == 2400:
                    test.requests.append(test.now)

            def readline(self, size):
                test.assertEqual(size, 1025)
                reply = test.responses.pop(len(test.requests), b"")
                if isinstance(reply, Exception):
                    raise reply
                if not reply:
                    test.now += self.timeout
                return reply

        serial = patch.dict(sys.modules, {"serial": SimpleNamespace(Serial=Serial)})
        monotonic = patch("lead_update.time.monotonic", side_effect=lambda: self.now)
        sleep = patch("lead_update.time.sleep", side_effect=self.advance)
        for replacement in (serial, monotonic, sleep):
            replacement.start()
            self.addCleanup(replacement.stop)
        self.client = ReceiverStatus("COM17")
        self.addCleanup(self.client.close)

    def advance(self, seconds):
        self.now += seconds

    def assert_read_only(self):
        self.assertEqual(self.client.port.baudrate, 115200)
        self.assertTrue(set(self.baudrates).issubset({115200, 2400}))
        self.client.port.write.assert_not_called()

    def test_silent_first_request_is_retried_without_discarding_pending_input(self):
        self.responses[2] = status_line()
        info = self.client.request("info")
        self.assertEqual(info["identity"], "1ccd6d8a20769eae")
        self.assertEqual(info["role"], "follower")
        self.assertEqual(len(self.requests), 2)
        self.assertGreaterEqual(self.requests[1] - self.requests[0], 0.3)
        self.client.port.reset_input_buffer.assert_called_once()
        self.assert_read_only()

    def test_permanent_silence_keeps_one_timeout_budget_and_unknown_diagnosis(self):
        with self.assertRaises(ReceiverProbeTimeout) as raised:
            self.client.request("info")
        self.assertEqual(len(self.requests), 3)
        self.assertLessEqual(self.now, self.client.timeout + self.client.port.timeout)
        self.assertTrue(all(b - a >= 0.3 for a, b in zip(self.requests, self.requests[1:])))
        self.assertIn("COM17", str(raised.exception))
        self.assertIn("board state is unknown", str(raised.exception))
        self.assertNotIn("requires explicit initialization", str(raised.exception))
        self.assert_read_only()

    def test_malformed_reply_fails_immediately_without_retry(self):
        self.responses[1] = b"OTAR2 {invalid}\n"
        self.responses[2] = status_line()
        with self.assertRaisesRegex(ProtocolError, "invalid receiver status JSON"):
            self.client.request("info")
        self.assertEqual(len(self.requests), 1)
        self.assert_read_only()

    def test_incompatible_reply_fails_immediately_without_retry(self):
        self.responses[1] = status_line(protocol=1)
        self.responses[2] = status_line()
        with self.assertRaisesRegex(ProtocolError, "incompatible receiver status"):
            self.client.request("info")
        self.assertEqual(len(self.requests), 1)
        self.assert_read_only()

    def test_oversized_reply_fails_immediately_and_restores_baudrate(self):
        self.responses[1] = b"x" * 1025
        with self.assertRaisesRegex(ProtocolError, "bounded response"):
            self.client.request("info")
        self.assertEqual(len(self.requests), 1)
        self.assert_read_only()

    def test_serial_failure_is_not_retried_and_restores_baudrate(self):
        self.responses[1] = OSError("USB disconnected")
        with self.assertRaisesRegex(OSError, "USB disconnected"):
            self.client.request("info")
        self.assertEqual(len(self.requests), 1)
        self.assert_read_only()

    def test_consecutive_calls_respect_request_spacing(self):
        self.responses.update({1: status_line(), 2: status_line(image_class="lead")})
        self.client.request("info")
        info = self.client.request("info")
        self.assertEqual(info["role"], "lead")
        self.assertEqual(len(self.requests), 2)
        self.assertGreaterEqual(self.requests[1] - self.requests[0], 0.3)
        self.assert_read_only()

    def test_short_timeout_does_not_force_additional_attempts(self):
        self.client.timeout = 0.1
        with self.assertRaises(ReceiverProbeTimeout):
            self.client.request("info")
        self.assertEqual(len(self.requests), 1)
        self.assertLessEqual(self.now, self.client.timeout + self.client.port.timeout)
        self.assert_read_only()

    def test_commands_with_side_effects_are_rejected_before_request(self):
        for command, payload in (("abort", None), ("info", {"reset": True})):
            with self.subTest(command=command, payload=payload), \
                 self.assertRaisesRegex(CampaignError, "read-only"):
                self.client.request(command, payload)
        self.assertEqual(self.requests, [])
        self.assert_read_only()


if __name__ == "__main__":
    unittest.main()
