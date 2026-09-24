from pathlib import Path
from itertools import count
import struct
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech" / "tools"))
from smp_transport import (SerialSMP, ProtocolError, CommandError, TransportError,
                           ReceiverProbeTimeout, cbor_decode,
                           cbor_encode, uart_frames)


class Serial:
    def __init__(self, *args, **kwargs):
        self.lines = []
        self.written = []

    def write(self, data):
        self.written.append(data)

    def flush(self):
        pass

    def readline(self, size):
        return self.lines.pop(0) if self.lines else b""

    def close(self):
        pass


def response(payload, sequence=0, command=0, group=64, operation=3):
    data = cbor_encode(payload)
    return uart_frames(struct.pack(">BBHHBB", operation, 0, len(data), group, sequence, command) + data)


class SMPTests(unittest.TestCase):
    def test_bootloader_detection_uses_typed_unsupported_and_read_only_image_list(self):
        client = SerialSMP("fake", serial_factory=Serial)
        state = {"images": [{"slot": 0, "version": "1.0.0", "hash": b"h" * 32}]}
        client.serial.lines = response({"rc": 8}) + response(state, sequence=1, group=1, operation=1)
        with self.assertRaises(CommandError) as unsupported:
            client.request("info")
        self.assertTrue(unsupported.exception.unsupported)
        self.assertEqual(client.image_state(), state)
        expected = struct.pack(">BBHHBB", 0, 0, 1, 1, 1, 0) + cbor_encode({})
        self.assertEqual(client.serial.written[-1:], uart_frames(expected))
        for result in ({"rc": 5}, {"rc": "8"}, {"rc": 8, "err": {"group": 1, "rc": 8}}):
            self.assertFalse(CommandError("rejected", response=result).unsupported)

    def test_image_list_requires_valid_state_not_just_success_rc(self):
        no_hash = {"images": [{"slot": 0, "version": "0.9.0"}]}
        for state in ({"rc": 0}, {"images": {}}, {"images": [{}]},
                      {"images": [{"slot": 0, "version": "0.9.0", "hash": b"bad"}]},
                      {"images": []}, no_hash):
            client = SerialSMP("fake", serial_factory=Serial)
            client.serial.lines = response(state, group=1, operation=1)
            if state in ({"images": []}, no_hash):
                self.assertEqual(client.image_state(), state)
            else:
                with self.subTest(state=state), self.assertRaises(ProtocolError):
                    client.image_state()

    def test_cbor_types_indefinite_and_rejection(self):
        value = {"a": [True, False, None, -17, 2**63, b"abc", "é"]}
        self.assertEqual(cbor_decode(cbor_encode(value)), value)
        self.assertEqual(cbor_decode(b"\xbf\x61a\x9f\x01\x02\xff\xff"), {"a": [1, 2]})
        for data in (b"\xbf", b"\x01\x02", b"\xa2\x61a\x01\x61a\x02", b"\x5a\xff\xff\xff\xff"):
            with self.subTest(data=data), self.assertRaises(ProtocolError):
                cbor_decode(data)

    def test_uart_fragmentation_crc_sequence_and_console(self):
        client = SerialSMP("fake", serial_factory=Serial)
        expected = {"rc": 0, "large": b"x" * 1024}
        client.serial.lines = [b"application log\n"] + response({}, sequence=255) + response(expected)
        self.assertEqual(client.request("info"), expected)
        self.assertTrue(client.serial.written[0].startswith(b"\x06\x09"))

    def test_error_and_wrong_command(self):
        for lines, error in ((response({"rc": 5}), CommandError),
                             (response({}, command=3), ProtocolError),
                             (response({}, group=65), ProtocolError),
                             ([b"\x06\x09truncated"], ProtocolError),
                             ([b"\x06\x09!bad!\n"], ProtocolError)):
            client = SerialSMP("fake", serial_factory=Serial)
            client.serial.lines = lines
            with self.assertRaises(error):
                client.request("info")

    def test_silent_or_console_only_probe_preserves_timeout_type(self):
        for lines in ([], [b"application log\n"], [b"~~~~~~~~"]):
            with self.subTest(lines=lines):
                client = SerialSMP("fake", serial_factory=Serial)
                client.serial.lines = list(lines)
                with patch("smp_transport.time.monotonic", side_effect=count()), \
                     self.assertRaises(ReceiverProbeTimeout):
                    client.request("info")

    def test_partial_or_stale_framed_reply_is_not_receiver_absence(self):
        fragmented = response({"large": b"x" * 1024})
        for lines in (fragmented[:1], fragmented[1:2], response({}, sequence=255),
                      response({}, sequence=255) + fragmented[:1]):
            with self.subTest(lines=lines):
                client = SerialSMP("fake", serial_factory=Serial)
                client.serial.lines = list(lines)
                with patch("smp_transport.time.monotonic", side_effect=count()), \
                     self.assertRaises(TransportError) as raised:
                    client.request("info")
                self.assertNotIsInstance(raised.exception, ReceiverProbeTimeout)

    def test_serial_io_errors_remain_distinct_from_receiver_absence(self):
        for operation in ("write", "flush", "readline"):
            with self.subTest(operation=operation):
                client = SerialSMP("fake", serial_factory=Serial)
                setattr(client.serial, operation, Mock(side_effect=OSError("device disconnected")))
                with self.assertRaisesRegex(TransportError, "device disconnected") as raised:
                    client.request("info")
                self.assertNotIsInstance(raised.exception, ReceiverProbeTimeout)


if __name__ == "__main__":
    unittest.main()
