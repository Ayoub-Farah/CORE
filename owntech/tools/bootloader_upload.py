"""Run the existing MCUboot uploader with bounded, observable progress."""
from collections import deque
import math
from pathlib import Path
import queue
import re
import subprocess
import threading
import time


class UploadError(RuntimeError):
    pass


_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_PROGRESS = re.compile(
    r"(?P<done>\d+(?:\.\d+)?)\s*(?P<done_unit>[KMGT]?i?B)?\s*/\s*"
    r"(?P<total>\d+(?:\.\d+)?)(?:\s*(?P<total_unit>[KMGT]?i?B))?"
    r"(?:\s+(?P<percent>\d+(?:\.\d+)?)\s*%)?", re.IGNORECASE)
_UNITS = {"": 1, "B": 1, "KB": 1000, "MB": 1000**2, "GB": 1000**3,
          "TB": 1000**4, "KIB": 1024, "MIB": 1024**2, "GIB": 1024**3,
          "TIB": 1024**4}


def _progress(line, image_size):
    """Return monotonic progress evidence; human-size fields may be rounded."""
    match = _PROGRESS.search(_ANSI.sub("", line))
    if not match:
        return None
    done_unit = _UNITS[(match["done_unit"] or "").upper()]
    total_unit = _UNITS[(match["total_unit"] or "").upper()]
    done = float(match["done"]) * done_unit
    total = float(match["total"]) * total_unit
    # MCUmgr switches from exact bytes to rounded sizes (e.g. 222.00 KiB).
    decimals = len(match["total"].split(".")[1]) if "." in match["total"] else 0
    tolerance = total_unit * 0.5 * 10**(-decimals) if total_unit != 1 else 0
    if total <= 0 or abs(total - image_size) > tolerance + 1e-6:
        return None
    percent = float(match["percent"]) if match["percent"] is not None else None
    if percent is not None and not 0 <= percent <= 100:
        return None
    fraction = done / total
    if percent is not None:
        fraction = max(fraction, percent / 100)
    complete = done > 0 and (percent == 100 or (done_unit == 1 and done == image_size))
    return min(fraction, 1), complete


def _read_output(stream, events, stop):
    """Back-pressure limits queued output while the main thread checks timers."""
    try:
        while not stop.is_set():
            chunk = stream.read(1024)
            event = chunk if chunk else None
            while not stop.is_set():
                try:
                    events.put(event, timeout=0.1)
                    break
                except queue.Full:
                    continue
            if not chunk:
                return
    except (OSError, ValueError) as error:
        while not stop.is_set():
            try:
                events.put(error, timeout=0.1)
                return
            except queue.Full:
                continue


def _stop_process(process):
    try:
        if process.poll() is None:
            try:
                process.terminate()
            except OSError:
                pass  # It may have exited between poll and terminate.
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                except OSError:
                    pass
                process.wait(timeout=2)
        else:
            process.wait()
    except (OSError, subprocess.TimeoutExpired) as error:
        raise UploadError("cannot stop MCUboot uploader: %s" % error) from error


def upload_image(base, image, *, stall_timeout=20, total_timeout=240, output=print):
    """Upload using MCUmgr; require successful exit and complete progress.

    Progress output is transport evidence only; the caller must still verify
    the application identity/hash after reboot. No shell or hardware discovery
    is performed here, and the caller owns all MCUmgr flags in ``base``.
    """
    if (not math.isfinite(stall_timeout) or not math.isfinite(total_timeout)
            or stall_timeout <= 0 or total_timeout <= 0):
        raise UploadError("upload timeouts must be positive and finite")
    image = Path(image)
    try:
        image_size = image.stat().st_size
    except OSError as error:
        raise UploadError("cannot read upload image: %s" % error) from error
    if image_size <= 0:
        raise UploadError("upload image is empty")
    try:
        process = subprocess.Popen(list(base) + ["image", "upload", str(image)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   stdin=subprocess.DEVNULL, bufsize=0, shell=False)
    except OSError as error:
        raise UploadError("cannot start MCUboot uploader: %s" % error) from error
    events = queue.Queue(maxsize=64)
    stop = threading.Event()
    reader = threading.Thread(target=_read_output, args=(process.stdout, events, stop),
                              name="mcumgr-output", daemon=True)
    history = deque(maxlen=8)
    pending = bytearray()
    started = last_advance = time.monotonic()
    highest = 0.0
    complete = eof = False

    def failure(reason):
        tail = " | ".join(history)
        return UploadError(reason + ("; last uploader output: " + tail if tail else ""))

    def line_received(raw):
        nonlocal highest, last_advance, complete
        line = _ANSI.sub("", raw.decode("utf-8", errors="replace")).strip()
        if not line:
            return
        history.append(line[-512:])
        if output is print:
            output(line, flush=True)
        else:
            output(line)
        progress = _progress(line, image_size)
        if progress is not None:
            fraction, finished = progress
            if fraction > highest:
                highest = fraction
                last_advance = time.monotonic()
            complete = complete or finished

    try:
        reader.start()
        while True:
            now = time.monotonic()
            returncode = process.poll()
            if eof and returncode is not None:
                if returncode != 0:
                    raise failure("MCUboot uploader exited with code %d" % returncode)
                if not complete:
                    raise failure("MCUboot uploader exited without reporting a complete image transfer")
                return
            if now - started >= total_timeout:
                raise failure("MCUboot upload exceeded its %.1fs total timeout" % total_timeout)
            if now - last_advance >= stall_timeout:
                raise failure("MCUboot upload made no progress for %.1fs" % stall_timeout)
            wait = min(0.1, total_timeout - (now - started), stall_timeout - (now - last_advance))
            try:
                event = events.get(timeout=max(wait, 0.001))
            except queue.Empty:
                continue
            if event is None:
                if pending:
                    line_received(bytes(pending))
                    pending.clear()
                eof = True
            elif isinstance(event, Exception):
                raise failure("cannot read MCUboot uploader output: %s" % event)
            else:
                for byte in event:
                    if byte in (10, 13):
                        line_received(bytes(pending))
                        pending.clear()
                    else:
                        pending.append(byte)
                        # A broken uploader cannot accumulate an unbounded line.
                        if len(pending) >= 8192:
                            line_received(bytes(pending))
                            pending.clear()
    finally:
        stop.set()
        try:
            _stop_process(process)
        finally:
            process.stdout.close()
            if reader.ident is not None:
                reader.join(timeout=2)
