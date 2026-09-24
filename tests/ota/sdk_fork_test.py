"""Pin all vendored SDK sources; use --refresh after reviewing a deliberate change."""
import hashlib
import json
from pathlib import Path
import sys
import unittest

FORK = Path(__file__).resolve().parents[2] / "third_party/thingset-zephyr-sdk"
MANIFEST = FORK / "OWNTECH_SHA256.json"
UPSTREAM = "e57447bbeb7c165e14a273242e8495343c1c6f54"


def inventory():
    files = {}
    for path in sorted(FORK.rglob("*")):
        if not path.is_file() or path == MANIFEST or ".git" in path.parts:
            continue
        data = path.read_bytes()
        try:
            data = data.decode("utf-8").replace("\r\n", "\n").encode("utf-8")
        except UnicodeDecodeError:
            pass
        files[path.relative_to(FORK).as_posix()] = hashlib.sha256(data).hexdigest()
    return {"upstream_commit": UPSTREAM, "normalization": "utf8-crlf-to-lf", "files": files}


class SdkForkTests(unittest.TestCase):
    def test_vendored_sources_are_pinned(self):
        self.assertEqual(json.loads(MANIFEST.read_text(encoding="utf-8")), inventory(),
                         "Vendored SDK differs from reviewed SHA-256 inventory")


if __name__ == "__main__":
    if sys.argv[1:] == ["--refresh"]:
        MANIFEST.write_text(json.dumps(inventory(), indent=2) + "\n", encoding="utf-8")
    else:
        unittest.main()
