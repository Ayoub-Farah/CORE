"""Run production ota_usb.cpp and the installed Zephyr zcbor against PC bytes.

Only MCUmgr dispatch/Zephyr runtime boundaries are shimmed. No physical board is
used. Set ZEPHYR_BASE for a standalone Zephyr checkout or use PlatformIO's package.
"""
import base64
import binascii
import ctypes
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "owntech/tools"))
from smp_transport import SerialSMP, CommandError, cbor_encode, cbor_decode, uart_frames
from lead_update import Campaign, Journal

IDS = ["0102030405060708", "1112131415161718", "2122232425262728"]
MANIFEST = {"artifact_size": 512, "useful_size": 512, "artifact_sha256": "aa" * 32,
            "mcuboot_image_hash": "bb" * 32, "version": "1.0.1+0", "build_id": "test-app-next",
            "protocol": 2, "image_class": "receiver", "format": "mcuboot-compact", "activation_trailer": False,
            "hardware_id": 0x01020142, "layout_id": 0x00010001,
            "bootloader_id": 0x00010100, "profile": {"slot_size": 227328}}


def stage_payload():
    values = {key: value for key, value in MANIFEST.items() if key not in ("profile", "format", "activation_trailer")}
    values.update(campaign=0x0123456789ABCDEF,
                  artifact_sha256=bytes.fromhex(MANIFEST["artifact_sha256"]),
                  mcuboot_image_hash=bytes.fromhex(MANIFEST["mcuboot_image_hash"]))
    return values


class NativeSerial:
    """Physical UART boundary substitute; both application codecs are real."""
    def __init__(self, native):
        self.native = native
        self.input = bytearray()
        self.expected = None
        self.lines = []
        self.maximum_response = 0

    def write(self, line):
        data = base64.b64decode(line[2:].strip(), validate=True)
        if line[:2] == b"\x06\x09":
            self.expected = int.from_bytes(data[:2], "big")
            self.input = bytearray(data[2:])
        elif line[:2] == b"\x04\x14":
            self.input.extend(data)
        else:
            raise AssertionError("unexpected UART prefix")
        if len(self.input) == self.expected:
            if binascii.crc_hqx(self.input, 0) != 0:
                raise AssertionError("client UART CRC is invalid")
            packet = self.native(bytes(self.input[:-2]))
            self.maximum_response = max(self.maximum_response, len(packet))
            self.lines.extend(uart_frames(packet))
        return len(line)

    def flush(self):
        pass

    def readline(self, maximum):
        return self.lines.pop(0) if self.lines else b""

    def close(self):
        pass


class USBTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        zephyr = Path(os.environ.get("ZEPHYR_BASE", Path.home() / ".platformio/packages/framework-zephyr"))
        zcbor = zephyr / "_pio/modules/lib/zcbor"
        if not zcbor.is_dir():
            zcbor = zephyr.parent / "modules/lib/zcbor"
        if not (zcbor / "src/zcbor_decode.c").is_file():
            raise unittest.SkipTest("native USB suite requires installed Zephyr/zcbor; build USB_LEAD or set ZEPHYR_BASE")
        cls.directory = tempfile.TemporaryDirectory(prefix="owntech-usb-")
        build = Path(cls.directory.name)
        cc = shutil.which("clang") or shutil.which("gcc")
        cxx = shutil.which("clang++") or shutil.which("g++")
        if not cc or not cxx:
            raise RuntimeError("native C/C++ compiler required (LLVM on Windows)")
        includes = [ROOT / "tests/ota/usb_shim", ROOT / "zephyr/modules/owntech_ota/zephyr/public_api",
                    zcbor / "include", zephyr / "subsys/mgmt/mcumgr/util/include", zephyr / "include"]
        common = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-function", "-Wno-unused-variable",
                  "-Wno-unused-but-set-variable",
                  "-DZCBOR_ASSERTS", "-DZCBOR_CANONICAL", "-DZCBOR_STOP_ON_ERROR"]
        if os.name == "nt":
            includes.insert(0, ROOT / "tests/ota/usb_shim/libc")
            common += ["-ffreestanding", "-fno-stack-protector", "-mno-stack-arg-probe"]
        else:
            common += ["-fPIC"]
        common += ["-I" + str(path) for path in includes]
        sources = [zcbor / ("src/zcbor_" + name + ".c") for name in ("common", "encode", "decode")]
        sources += [zephyr / "subsys/mgmt/mcumgr/util/src/zcbor_bulk.c", ROOT / "tests/ota/usb_bridge.cpp"]
        if os.name == "nt":
            sources += [ROOT / "tests/ota/usb_libc.c"]
        objects = []
        for index, source in enumerate(sources):
            obj = build / (str(index) + ".o")
            compiler, standard = (cxx, "c++20") if source.suffix == ".cpp" else (cc, "c11")
            command = [compiler, "-std=" + standard, *common]
            if source.suffix == ".cpp":
                command += ["-fno-exceptions", "-fno-rtti"]
            command += ["-c", str(source), "-o", str(obj)]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise RuntimeError(result.stdout + result.stderr)
            objects.append(obj)
        output = build / ("usb.dll" if os.name == "nt" else "usb.so")
        command = [cxx, "-shared", *map(str, objects), "-o", str(output)]
        if os.name == "nt":
            command += ["-nostdlib", "-fuse-ld=lld", "-Wl,/noentry",
                        "-Wl,/export:usb_smp_request", "-Wl,/export:usb_reset", "-Wl,/export:usb_stat",
                        "-Wl,/export:usb_health", "-Wl,/export:usb_discovery_pending"]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.usb_smp_request.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.usb_smp_request.restype = ctypes.c_int
        cls.lib.usb_stat.argtypes = [ctypes.c_uint]
        cls.lib.usb_stat.restype = ctypes.c_uint
        cls.lib.usb_health.argtypes = [ctypes.c_bool, ctypes.c_bool, ctypes.c_int]
        cls.lib.usb_discovery_pending.argtypes = [ctypes.c_bool]

    @classmethod
    def tearDownClass(cls):
        if os.name == "nt":
            import _ctypes
            _ctypes.FreeLibrary(cls.lib._handle)
        del cls.lib
        cls.directory.cleanup()

    def setUp(self):
        self.lib.usb_reset()
        self.serial = NativeSerial(self.native)
        self.client = SerialSMP("native", serial_factory=lambda *a, **kw: self.serial)

    def native(self, packet, capacity=1536):
        incoming = ctypes.create_string_buffer(packet)
        outgoing = ctypes.create_string_buffer(capacity)
        length = self.lib.usb_smp_request(incoming, len(packet), outgoing, capacity)
        self.assertGreater(length, 0, "native SMP bridge error %d" % length)
        self.assertLessEqual(length, capacity)
        return outgoing.raw[:length]

    def raw_request(self, payload, command=3, group=64, op=2):
        packet = struct.pack(">BBHHBB", op, 0, len(payload), group, 42, command) + payload
        return cbor_decode(self.native(packet)[8:])

    def test_info_discover_and_paged_status_shape_and_size(self):
        info = self.client.request("info")
        self.assertEqual(info["service"], "owntech-ota")
        self.assertEqual(info["protocol"], 2)
        self.assertEqual(info["image_class"], "lead")
        self.assertEqual(info["identity"], "ff" * 8)
        self.assertEqual((info["slot_size"], info["useful_capacity"]), (227328, 221184))
        self.assertTrue(info["available"] and info["active_confirmed"] and info["slot_available"])
        self.assertTrue(info["local_healthy"] and info["healthy"] and info["can_ready"])
        self.assertEqual(info["error"], 0)
        inventory = self.client.request("discover")
        self.assertEqual(inventory["target_count"], 3)
        self.assertEqual(len(inventory["targets"]), 1)
        for index, expected in enumerate(IDS):
            row = self.client.request("status", {"index": index})["targets"][0]
            self.assertEqual(row["identity"], expected)
            self.assertTrue(row["compatible"] and row["available"])
            self.assertEqual(len(row["mcuboot_image_hash"]), 32)
        self.assertLess(self.serial.maximum_response, 1536)

    def test_standalone_and_failed_health_are_visible_in_info_and_status(self):
        for local, can, error, phase in ((True, False, 0, "WAITING_CAN"),
                                        (False, False, -17, "FAILED")):
            self.lib.usb_health(local, can, error)
            for command in ("info", "status"):
                with self.subTest(command=command, phase=phase):
                    reply = self.client.request(command)
                    self.assertEqual(reply["phase"], phase)
                    self.assertEqual(reply["local_healthy"], local)
                    self.assertFalse(reply["healthy"])
                    self.assertFalse(reply["can_ready"])
                    self.assertEqual(reply["error"], error)
                    if command == "info":
                        self.assertFalse(reply["available"])
        self.assertLess(self.serial.maximum_response, 1536)

    def test_discovery_token_roundtrip_and_complete_pages_only(self):
        token = 0xFEDCBA9876543210
        self.lib.usb_discovery_pending(True)
        reply = self.client.request("discover", {"campaign": token, "index": 15})
        self.assertEqual(reply["phase"], "DISCOVERING")
        self.assertEqual((reply["target_count"], reply["targets"]), (0, []))
        self.assertEqual((self.lib.usb_stat(5), self.lib.usb_stat(6)),
                         (token & 0xFFFFFFFF, token >> 32))
        self.lib.usb_discovery_pending(False)
        for index, expected in enumerate(IDS):
            reply = self.client.request("discover", {"campaign": token, "index": index})
            self.assertEqual(reply["target_count"], 3)
            self.assertEqual(reply["targets"][0]["identity"], expected)
            self.assertEqual((self.lib.usb_stat(5), self.lib.usb_stat(6)),
                             (token & 0xFFFFFFFF, token >> 32))
        self.client.request("discover")
        self.assertEqual((self.lib.usb_stat(5), self.lib.usb_stat(6)), (0, 0))

    def test_full_python_campaign_over_uart_smp_cbor_native_handlers(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "campaign.jsonl", 42)
            try:
                campaign = Campaign(self.client, MANIFEST, b"A" * 512, journal, expected_ids=IDS,
                                    timeout=2, poll_interval=0, output=lambda line: None)
                self.assertEqual(campaign.run(), "SUCCESS")
                contents=journal.path.read_text(encoding="utf-8")
                self.assertIn('"device_event": 11', contents)
                self.assertIn('"device_uptime_ms": 5', contents)
            finally:
                journal.close()
        self.assertEqual(self.lib.usb_stat(0), 1)
        self.assertEqual(self.lib.usb_stat(1), 1)
        self.assertEqual(self.lib.usb_stat(2), 512)
        self.assertEqual(self.lib.usb_stat(3), MANIFEST["hardware_id"])
        self.assertEqual(self.lib.usb_stat(4), MANIFEST["useful_size"])
        self.assertLess(self.serial.maximum_response, 1536)

    def test_required_stage_fields_types_hashes_and_strings(self):
        values = stage_payload()
        for field in values:
            self.lib.usb_reset()
            candidate = dict(values)
            del candidate[field]
            with self.subTest(field=field), self.assertRaises(CommandError):
                self.client.request("stage_begin", candidate)
        for field, value in (("artifact_sha256", b"x" * 31), ("mcuboot_image_hash", "11" * 32),
                             ("version", "A\x00B"), ("build_id", "X" * 32), ("protocol", 256),
                             ("campaign", -1), ("artifact_size", 2**32)):
            candidate = dict(values, **{field: value})
            with self.subTest(field=field, value=value), self.assertRaises(CommandError):
                self.client.request("stage_begin", candidate)

    def test_malformed_cbor_and_target_lists_rejected_before_runtime(self):
        for payload in (b"", b"\xbf", b"\xa1\x61a", b"\x01", b"\xa2\x68campaign\x01\x68campaign\x02"):
            with self.subTest(payload=payload):
                self.assertNotEqual(self.raw_request(payload)["rc"], 0)
        for targets in (["bad"], ["zz" * 8], IDS * 6):
            with self.subTest(targets=targets), self.assertRaises(CommandError):
                self.client.request("start", {"campaign": 1, "targets": targets})
        self.assertEqual(self.lib.usb_stat(0), 0)

    def test_guard_rejects_standard_image_and_os_reset_before_dispatch(self):
        for group in (0, 1, 2, 63, 65):
            for command in (0, 1, 2, 5):
                for op in (0, 2):
                    with self.subTest(group=group, command=command, op=op):
                        reply = self.raw_request(cbor_encode({}), command, group, op)
                        self.assertEqual(reply["rc"], 11)  # actual Zephyr MGMT_ERR_EACCESSDENIED
        self.assertEqual(self.lib.usb_stat(0), 0)
        self.assertEqual(self.lib.usb_stat(1), 0)


if __name__ == "__main__":
    unittest.main()
