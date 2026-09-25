"""Deterministic application identity, independent of deployment and Git state.

PlatformIO installs dependencies before running pre scripts. Hash their sources
under a canonical library path; the receiver and dedicated Lead have distinct inputs.
The signed image's SHA-256 remains the authority for the exact distributed bytes.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shlex


SCHEMA = 2
_VERSION = re.compile(r"([0-9]+)\.([0-9]+)\.([0-9]+)(?:\+([0-9]+))?\Z")
_SKIP_DIRS = {".git", ".hg", ".svn", ".pio", ".github", "__pycache__",
              "build", "docs", "doc", "tests", "test", "samples"}
_SKIP_SUFFIXES = {".md", ".rst", ".pyc", ".pyo", ".log", ".tmp", ".bak"}
_COMPILE_OPTIONS = {
    "platform", "framework", "board", "board_version", "board_shield",
    "board_shield_version", "lib_deps", "lib_ignore", "lib_compat_mode",
    "lib_ldf_mode", "lib_archive", "debug_build_flags",
}
_SKIP_OPTIONS = {"build_cache_dir", "build_dir"}
_IGNORED_CMAKE_DEFINES = {
    "BUILD_ENV_NAME", "OWNTECH_FIRMWARE_VERSION", "OWNTECH_FIRMWARE_BUILD_ID",
    "CMAKE_BINARY_DIR", "CMAKE_INSTALL_PREFIX", "OWNTECH_OTA_IDENTITY_DIR",
}


def normalize_version(value="1.0.0"):
    """Return MCUboot's exact M.m.r+b spelling; reject overflow before signing."""
    match = _VERSION.fullmatch(str(value).strip())
    if not match:
        raise ValueError("OTA app_version must be major.minor.revision[+build]")
    parts = [int(part or 0) for part in match.groups()]
    if any(part > limit for part, limit in zip(parts, (255, 255, 65535, 0xFFFFFFFF))):
        raise ValueError("OTA app_version exceeds MCUboot's 8/8/16/32-bit fields")
    return "%d.%d.%d+%d" % tuple(parts)


def _cmake_args(value, ignored=_IGNORED_CMAKE_DEFINES):
    values = value if isinstance(value, (list, tuple)) else [value]
    args = []
    for item in values:
        args.extend(shlex.split(str(item).replace("\\", "/")))
    result = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg == "-D" and index + 1 < len(args):
            index += 1
            arg += args[index]
        name = arg[2:].split("=", 1)[0].split(":", 1)[0] if arg.startswith("-D") else ""
        if name not in ignored:
            result.append(arg)
        index += 1
    return result


def effective_settings(options, board_build=None, packages=None, project_dir=None):
    """Keep compilation inputs, not serial ports, fleet size, targets or env name."""
    roots = sorted({str(Path(project_dir).resolve()).replace("\\", "/"),
                    str(Path(project_dir).absolute()).replace("\\", "/")},
                   key=len, reverse=True) if project_dir else []

    def clean(value, key=""):
        if key == "cmake_extra_args" or key.endswith(".cmake_extra_args"):
            return [clean(arg) for arg in _cmake_args(value)]
        if key == "app_version" or key.endswith(".app_version"):
            return normalize_version(value)
        if isinstance(value, dict):
            return {str(k): clean(v, str(k)) for k, v in sorted(value.items())}
        if isinstance(value, (list, tuple)):
            return [clean(item) for item in value]
        if isinstance(value, str):
            result = value.replace("\\", "/").strip()
            for root in roots:
                result = re.sub(re.escape(root), lambda _: "${PROJECT_DIR}", result,
                                flags=re.IGNORECASE if os.name == "nt" else 0)
            return result
        if value is None or isinstance(value, (int, float, bool)):
            return value
        return str(value)

    selected = {
        key: clean(value, key) for key, value in sorted(options.items())
        if (key in _COMPILE_OPTIONS or key.startswith("board_build.")
            or key.startswith("build_")) and key not in _SKIP_OPTIONS
    }
    return {"options": selected, "board_build": clean(board_build or {}),
            "packages": clean(packages or {})}


def _source_files(directory):
    """Walk inputs only; never Git metadata, generated outputs or documentation."""
    directory = Path(directory)
    if not directory.is_dir():
        return
    for current, dirs, files in os.walk(directory):
        dirs[:] = sorted(name for name in dirs if name not in _SKIP_DIRS
                         and not name.startswith("."))
        for name in sorted(files):
            path = Path(current) / name
            if name.startswith(".") or path.suffix.lower() in _SKIP_SUFFIXES:
                continue
            # Effective INI values are hashed separately; deployment options in
            # src/app.ini must not make an otherwise identical firmware differ.
            if path.suffix.lower() == ".ini" or name in {"LICENSE", "NOTICE", "integrity.dat"}:
                continue
            yield path.relative_to(directory).as_posix(), path


def fingerprint(project_dir, version, settings, source_roots=None, configuration_only=False):
    """Hash canonical paths and content; no timestamps, absolute paths or Git IDs."""
    project_dir = Path(project_dir)
    version = normalize_version(version)
    roots = source_roots if source_roots is not None else [
        (name, project_dir / name) for name in ("src", "include", "zephyr", "third_party")
    ]
    digest = hashlib.sha256()
    count = 0

    def add(name, content):
        # Length prefixes avoid ambiguous concatenations and preserve binary data.
        encoded = name.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)
        digest.update(len(content).to_bytes(8, "big"))
        digest.update(content)

    payload = {"schema": SCHEMA, "version": version, "settings": settings}
    add("settings", json.dumps(payload, sort_keys=True, separators=(",", ":")).encode())
    for prefix, directory in sorted(roots, key=lambda root: root[0]):
        for relative, path in _source_files(directory):
            configuration = (path.name.startswith("Kconfig") or path.name.endswith("_defconfig")
                             or path.name in {"CMakeLists.txt", "library.json", "library.properties"}
                             or path.suffix.lower() in {".cmake", ".conf", ".overlay", ".dts", ".dtsi", ".yaml", ".yml"})
            # CMake needs a refresh for source additions/removals, but ordinary
            # C/C++ edits must retain incremental compilation. Always hash names.
            content = path.read_bytes() if not configuration_only or configuration else b""
            # Git's CRLF conversion should not distinguish identical C/C++ inputs.
            if b"\0" not in content:
                content = content.replace(b"\r\n", b"\n")
            add(prefix.rstrip("/") + "/" + relative, content)
            count += 1
    for relative in ("west.yml", "owntech/scripts/ota_build_identity.py",
                     "owntech/scripts/pre_ota_identity.py", "owntech/scripts/pre_version.py"):
        path = project_dir / relative
        if path.is_file():
            content = path.read_bytes() if not configuration_only or relative == "west.yml" else b""
            add(relative, content.replace(b"\r\n", b"\n"))
            count += 1
    result = digest.hexdigest()
    return {"schema": SCHEMA, "version": version, "build_id": "ota-" + result[:24],
            "source_sha256": result, "input_files": count}


def write_identity(output_dir, identity):
    """Refresh outputs before CMake, without touching mtimes on an unchanged build."""
    output_dir = Path(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    outputs = {
        "owntech_build_info.h": (
            "/* Generated by pre_ota_identity.py; do not edit. */\n#pragma once\n"
            '#define OWNTECH_FIRMWARE_VERSION "%s"\n'
            '#define OWNTECH_FIRMWARE_BUILD_ID "%s"\n'
        ) % (identity["version"], identity["build_id"]),
        "identity.json": json.dumps(identity, sort_keys=True, indent=2) + "\n",
    }
    for name, content in outputs.items():
        path = output_dir / name
        encoded = content.encode("utf-8")
        if not path.exists() or path.read_bytes() != encoded:
            path.write_bytes(encoded)


def generate_for_environment(env):
    """Called once per SCons invocation, before framework configuration and tasks."""
    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
    options = env.GetProjectOptions(as_dict=True)
    # Environment/CLI BUILD_FLAGS overrides are already loaded by PlatformIO.
    for key in ("build_flags", "build_unflags", "build_src_filter", "build_type"):
        if key.upper() in env:
            options[key] = env[key.upper()]
    board = env.BoardConfig()
    board_build = board.get("build", {})
    version = normalize_version(board.get("build.zephyr.bootloader.app_version", "1.0.0"))
    platform = env.PioPlatform()
    packages = {
        package.metadata.name: str(package.metadata.version)
        for package in platform.get_installed_packages()
        if package.metadata.name.startswith(("framework-", "toolchain-"))
        or package.metadata.name in {"tool-cmake", "tool-dtc", "tool-ninja"}
    }
    image_class = "lead" if env["PIOENV"] == "USB_LEAD" else "receiver"
    application = project_dir / "owntech/lead" if image_class == "lead" else Path(env.subst("$PROJECT_SRC_DIR"))
    roots = [("application/" + image_class, application),
             ("include", Path(env.subst("$PROJECT_INCLUDE_DIR"))),
             ("zephyr", project_dir / "zephyr"),
             ("third_party", project_dir / "third_party")]
    library_dir = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / env["PIOENV"]
    roots.append(("libraries", library_dir))
    # Include ordinary private libraries alongside PlatformIO's env directories.
    private_dir = Path(env.subst(env.GetProjectConfig().get("platformio", "lib_dir")))
    if not private_dir.is_absolute():
        private_dir = project_dir / private_dir
    if private_dir.is_dir():
        for directory in sorted(private_dir.iterdir()):
            if directory.is_dir() and any((directory / marker).exists() for marker in
                                          ("library.json", "library.properties", "src")):
                roots.append(("private_libraries/" + directory.name, directory))
    settings = effective_settings(options, board_build, packages, project_dir)
    settings["image_class"] = image_class
    identity = fingerprint(project_dir, version, settings, roots)
    identity["image_class"] = image_class
    configuration = fingerprint(project_dir, version, settings, roots, configuration_only=True)
    environment = env["PIOENV"]
    if not environment or Path(environment).name != environment or environment in {".", ".."}:
        raise ValueError("Unsafe OTA environment name")
    build_dir = Path(env.subst("$BUILD_DIR"))
    durable_dir = Path(env.subst("$PROJECT_WORKSPACE_DIR")) / "ota-generated" / environment
    # Zephyr's PlatformIO builder deletes BUILD_DIR before reconfiguration.
    # Preserve identity outside it, then CMake can restore the exact same bytes.
    write_identity(durable_dir, identity)
    stamp = durable_dir / "configuration.sha256"
    digest = configuration["source_sha256"] + "\n"
    previous = stamp.read_text(encoding="utf-8") if stamp.is_file() else None
    if previous != digest:
        cache = build_dir / "CMakeCache.txt"
        if cache.is_file():
            cache.unlink()
        stamp.write_text(digest, encoding="utf-8")
    args = _cmake_args(board.get("build.zephyr.cmake_extra_args", ""),
                       ignored={"OWNTECH_OTA_IDENTITY_DIR"})
    args.append("-DOWNTECH_OTA_IDENTITY_DIR=" + durable_dir.resolve().as_posix())
    board.update("build.zephyr.cmake_extra_args", " ".join(shlex.quote(arg) for arg in args))
    output_dir = build_dir / "ota_generated"
    write_identity(output_dir, identity)
    env["OWNTECH_OTA_VERSION"] = identity["version"]
    env["OWNTECH_OTA_BUILD_ID"] = identity["build_id"]
    env["OWNTECH_OTA_IMAGE_CLASS"] = image_class
    env["OWNTECH_OTA_IDENTITY_FILE"] = str(output_dir / "identity.json")
    env["OWNTECH_OTA_IDENTITY_DIR"] = str(durable_dir)
    return identity
