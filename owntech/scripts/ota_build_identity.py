"""Deterministic application identity, independent of deployment and Git state.

PlatformIO installs dependencies before running pre scripts. Hash their sources
under a canonical library path, so OTA and USB_LEAD describe the same application.
The signed image's SHA-256 remains the authority for the exact distributed bytes.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import shlex


SCHEMA = 1
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
    "CMAKE_BINARY_DIR", "CMAKE_INSTALL_PREFIX",
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


def _cmake_args(value):
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
        if name not in _IGNORED_CMAKE_DEFINES:
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


def fingerprint(project_dir, version, settings, source_roots=None):
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
            content = path.read_bytes()
            # Git's CRLF conversion should not distinguish identical C/C++ inputs.
            if b"\0" not in content:
                content = content.replace(b"\r\n", b"\n")
            add(prefix.rstrip("/") + "/" + relative, content)
            count += 1
    for relative in ("west.yml", "owntech/scripts/ota_build_identity.py",
                     "owntech/scripts/pre_ota_identity.py", "owntech/scripts/pre_version.py"):
        path = project_dir / relative
        if path.is_file():
            add(relative, path.read_bytes().replace(b"\r\n", b"\n"))
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
    board_build = env.BoardConfig().get("build", {})
    version = normalize_version(env.BoardConfig().get("build.zephyr.bootloader.app_version", "1.0.0"))
    platform = env.PioPlatform()
    packages = {
        package.metadata.name: str(package.metadata.version)
        for package in platform.get_installed_packages()
        if package.metadata.name.startswith(("framework-", "toolchain-"))
        or package.metadata.name in {"tool-cmake", "tool-dtc", "tool-ninja"}
    }
    roots = [("src", Path(env.subst("$PROJECT_SRC_DIR"))),
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
    identity = fingerprint(project_dir, version, settings, roots)
    output_dir = Path(env.subst("$BUILD_DIR")) / "ota_generated"
    write_identity(output_dir, identity)
    env["OWNTECH_OTA_VERSION"] = identity["version"]
    env["OWNTECH_OTA_BUILD_ID"] = identity["build_id"]
    env["OWNTECH_OTA_IDENTITY_FILE"] = str(output_dir / "identity.json")
    return identity
