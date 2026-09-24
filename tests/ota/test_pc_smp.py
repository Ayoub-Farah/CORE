from pathlib import Path
import struct
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "owntech" / "tools"))
from smp_transport import (SerialSMP, ProtocolError, CommandError, cbor_decode,
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


def response(payload, sequence=0, command=0, group=64):
    data = cbor_encode(payload)
    return uart_frames(struct.pack(">BBHHBB", 3, 0, len(data), group, sequence, command) + data)


class SMPTests(unittest.TestCase):
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
                             ([b"\x06\x09!bad!\n"], ProtocolError)):
            client = SerialSMP("fake", serial_factory=Serial)
            client.serial.lines = lines
            with self.assertRaises(error):
                client.request("info")


if __name__ == "__main__":
    unittest.main()
