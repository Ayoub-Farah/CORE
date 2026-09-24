"""Bounded MCUmgr SMP v1 over the Zephyr UART line framing (no cbor2 needed)."""
import base64
import binascii
import struct
import time

GROUP = 64
COMMANDS = {name: index for index, name in enumerate((
    "info", "set_role", "discover", "stage_begin", "stage_data", "stage_end",
    "start", "status", "commit", "abort", "reconcile"))}
MAX_PACKET = 8192


class ProtocolError(ValueError):
    pass


class TransportError(IOError):
    pass


class ReceiverProbeTimeout(TransportError):
    """No framed response; distinct from an occupied/inaccessible port."""


class CommandError(ProtocolError):
    pass


def _head(major, value):
    if value < 24:
        return bytes([(major << 5) | value])
    for size, marker in ((1, 24), (2, 25), (4, 26), (8, 27)):
        if value < 1 << (size * 8):
            return bytes([(major << 5) | marker]) + value.to_bytes(size, "big")
    raise ProtocolError("CBOR integer out of range")


def cbor_encode(value):
    if value is None:
        return b"\xf6"
    if isinstance(value, bool):
        return b"\xf5" if value else b"\xf4"
    if isinstance(value, int):
        return _head(0, value) if value >= 0 else _head(1, -1 - value)
    if isinstance(value, bytes):
        return _head(2, len(value)) + value
    if isinstance(value, str):
        data = value.encode("utf-8")
        return _head(3, len(data)) + data
    if isinstance(value, (list, tuple)):
        return _head(4, len(value)) + b"".join(cbor_encode(item) for item in value)
    if isinstance(value, dict):
        return _head(5, len(value)) + b"".join(cbor_encode(k) + cbor_encode(v) for k, v in value.items())
    raise ProtocolError("unsupported CBOR value")


def cbor_decode(data):
    position = 0

    def take(count):
        nonlocal position
        if count < 0 or position + count > len(data):
            raise ProtocolError("truncated CBOR")
        result = data[position:position + count]
        position += count
        return result

    def parse(depth=0):
        if depth > 12:
            raise ProtocolError("CBOR nesting limit")
        tag = take(1)[0]
        major, minor = tag >> 5, tag & 31
        if major == 7:
            if minor in (20, 21, 22):
                return {20: False, 21: True, 22: None}[minor]
            raise ProtocolError("unsupported CBOR simple value")
        if minor == 31:
            if major not in (4, 5):
                raise ProtocolError("unsupported indefinite CBOR")
            count = None
        elif minor < 24:
            count = minor
        elif minor in (24, 25, 26, 27):
            count = int.from_bytes(take(1 << (minor - 24)), "big")
        else:
            raise ProtocolError("invalid CBOR header")
        if major in (0, 1):
            return count if major == 0 else -1 - count
        if major in (2, 3):
            value = take(count)
            return value if major == 2 else value.decode("utf-8")
        if major in (4, 5):
            result = [] if major == 4 else {}
            index = 0
            while count is None or index < count:
                if count is None and position < len(data) and data[position] == 0xFF:
                    take(1)
                    break
                item = parse(depth + 1)
                if major == 4:
                    result.append(item)
                else:
                    if not isinstance(item, (str, int)) or item in result:
                        raise ProtocolError("invalid or duplicate CBOR map key")
                    result[item] = parse(depth + 1)
                index += 1
                if index > 1024:
                    raise ProtocolError("CBOR collection limit")
            return result
        raise ProtocolError("unsupported CBOR type")

    result = parse()
    if position != len(data):
        raise ProtocolError("trailing CBOR data")
    return result


def uart_frames(packet):
    payload = struct.pack(">H", len(packet) + 2) + packet + struct.pack(">H", binascii.crc_hqx(packet, 0))
    return [(b"\x06\x09" if offset == 0 else b"\x04\x14")
            + base64.b64encode(payload[offset:offset + 90]) + b"\n"
            for offset in range(0, len(payload), 90)]


class SerialSMP:
    def __init__(self, port, timeout=5.0, serial_factory=None):
        if serial_factory is None:
            import serial
            serial_factory = serial.Serial
        try:
            # This transport never touches 1200 baud, reset or bootloader commands.
            self.serial = serial_factory(port, baudrate=115200, timeout=0.15, write_timeout=timeout)
        except OSError as error:
            raise TransportError("cannot open %s: %s" % (port, error)) from error
        self.timeout = timeout
        self.sequence = 0

    def close(self):
        self.serial.close()

    def _receive(self, deadline):
        payload = bytearray()
        expected = None
        while time.monotonic() < deadline:
            line = self.serial.readline(MAX_PACKET * 2)
            if not line:
                continue
            if not line.endswith(b"\n"):
                raise ProtocolError("unterminated/oversized SMP line")
            prefix = line[:2]
            if prefix not in (b"\x06\x09", b"\x04\x14"):
                # Zephyr UART MCUmgr reserves its own framed lines; never treat
                # application console output as a response or bootstrap evidence.
                continue
            try:
                chunk = base64.b64decode(line[2:].strip(), validate=True)
            except binascii.Error as error:
                raise ProtocolError("invalid SMP base64") from error
            if prefix == b"\x06\x09":
                if len(chunk) < 2:
                    raise ProtocolError("missing SMP UART length")
                expected = int.from_bytes(chunk[:2], "big")
                if not 10 <= expected <= MAX_PACKET + 2:
                    raise ProtocolError("SMP packet length out of range")
                payload = bytearray(chunk[2:])
            elif expected is not None:
                payload.extend(chunk)
            else:
                continue
            if len(payload) > expected:
                raise ProtocolError("SMP UART length overflow")
            if len(payload) == expected:
                if binascii.crc_hqx(payload, 0) != 0:
                    raise ProtocolError("SMP UART CRC mismatch")
                return bytes(payload[:-2])
        raise ReceiverProbeTimeout("SMP response timeout; receiver absence has not been proven")

    def request(self, command, payload=None):
        command_id = COMMANDS[command]
        body = cbor_encode(payload or {})
        if len(body) + 8 > MAX_PACKET:
            raise ProtocolError("SMP request too large")
        sequence = self.sequence
        self.sequence = (self.sequence + 1) & 0xFF
        packet = struct.pack(">BBHHBB", 2, 0, len(body), GROUP, sequence, command_id) + body
        try:
            for frame in uart_frames(packet):
                self.serial.write(frame)
            self.serial.flush()
            deadline = time.monotonic() + self.timeout
            while True:
                response = self._receive(deadline)
                if len(response) < 8:
                    raise ProtocolError("truncated SMP header")
                op, flags, length, group, seq, cmd = struct.unpack_from(">BBHHBB", response)
                if seq != sequence:
                    continue  # late response to a previous bounded transaction
                if op != 3 or flags != 0 or group != GROUP or cmd != command_id or length != len(response) - 8:
                    raise ProtocolError("mismatched SMP response header")
                result = cbor_decode(response[8:])
                if not isinstance(result, dict):
                    raise ProtocolError("SMP response is not a map")
                if result.get("rc", 0) != 0 or result.get("err"):
                    raise CommandError("%s rejected: %s" % (command, result))
                return result
        except OSError as error:
            raise TransportError(str(error)) from error
