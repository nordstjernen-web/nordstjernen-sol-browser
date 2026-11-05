"""Builds the browser release from source with meson, or copies the notarised macOS release, into the user's data directory."""

import hashlib
import json
import os
import platform
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request
from importlib import resources

from . import product
from .dependencies import PreflightError, preflight
from .system import (build_environment, build_path, locate_msys2, run_logged,
                     runtime_environment, write)

RELEASE_URL = f"https://github.com/{product.REPO}/releases/download/{product.VERSION}"
SOURCE_URL = f"https://github.com/{product.REPO}/archive/refs/tags/{product.VERSION}.tar.gz"
RUNTIME_FILE = "runtime.json"
EXECUTABLE_DIRECTORIES = ["bin", "libexec"]


class InstallError(Exception):
    pass


def log(message):
    write(f"[{product.PACKAGE_NAME}] {message}\n")


def _data_base():
    if sys.platform == "win32":
        return os.environ.get("LOCALAPPDATA") or os.path.join(os.path.expanduser("~"), "AppData", "Local")
    if sys.platform == "darwin":
        return os.path.join(os.path.expanduser("~"), "Library", "Application Support")
    return os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")


def _cache_base():
    if sys.platform == "win32":
        return _data_base()
    if sys.platform == "darwin":
        return os.path.join(os.path.expanduser("~"), "Library", "Caches")
    return os.environ.get("XDG_CACHE_HOME") or os.path.join(os.path.expanduser("~"), ".cache")


def install_root():
    override = os.environ.get(f"{product.ENV_PREFIX}_INSTALL_DIR")
    base = override or os.path.join(_data_base(), product.PACKAGE_NAME)
    return os.path.join(base, product.VERSION)


def _uses_mac_release():
    return (sys.platform == "darwin" and platform.machine() == "arm64"
            and os.environ.get(f"{product.ENV_PREFIX}_BUILD_FROM_SOURCE") != "1")


def _executable_path():
    return product.MAC_RELEASE["executable"] if _uses_mac_release() else product.EXECUTABLE


def _read_runtime(root):
    try:
        with open(os.path.join(root, RUNTIME_FILE), encoding="utf-8") as handle:
            return json.load(handle)
    except (OSError, ValueError):
        return {}


def _mark_executable(root):
    if sys.platform == "win32":
        return
    for directory in (os.path.join(root, name) for name in EXECUTABLE_DIRECTORIES):
        if not os.path.isdir(directory):
            continue
        for name in os.listdir(directory):
            path = os.path.join(directory, name)
            if os.path.isfile(path):
                mode = os.stat(path).st_mode
                if mode & 0o111 != 0o111:
                    os.chmod(path, mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def find_installed():
    root = install_root()
    exe = os.path.join(root, _executable_path())
    if not os.path.exists(exe):
        return None
    _mark_executable(root)
    return exe, runtime_environment(_read_runtime(root).get("path"))


def _fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": f"{product.PACKAGE_NAME}/{product.VERSION}"})
    try:
        return urllib.request.urlopen(request, timeout=60)
    except urllib.error.HTTPError as error:
        raise InstallError(f"GET {url}: HTTP {error.code}") from error
    except urllib.error.URLError as error:
        raise InstallError(f"GET {url}: {error.reason}") from error


def _download(url, destination):
    log(f"downloading {url}")
    with _fetch(url) as response, open(destination, "wb") as out:
        shutil.copyfileobj(response, out)


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _verify(path, expected, name):
    actual = _sha256(path)
    if actual != expected:
        raise InstallError(f"{name} has SHA-256 {actual}, but {expected} was expected; refusing to install it")


def _source_checksum():
    try:
        recorded = json.loads(resources.files(__package__).joinpath("source.json").read_text("utf-8"))
    except (OSError, ValueError):
        return None
    return recorded.get("sha256") if recorded.get("version") == product.VERSION else None


def _release_checksum(asset):
    with _fetch(f"{RELEASE_URL}/SHA256SUMS") as response:
        text = response.read().decode("utf-8", "replace")
    for line in text.splitlines():
        parts = line.strip().split()
        if len(parts) == 2 and parts[1].lstrip("*") == asset:
            return parts[0].lower()
    raise InstallError(f"{asset} is not listed in the {product.VERSION} SHA256SUMS")


def _run(command, args):
    try:
        result = subprocess.run([command, *args], check=False)
    except OSError as error:
        raise InstallError(f"{command}: {error}") from error
    if result.returncode != 0:
        raise InstallError(f"{command} {' '.join(args)} exited with {result.returncode}")


def _install_mac_release(staging, work):
    asset, app = product.MAC_RELEASE["asset"], product.MAC_RELEASE["app"]
    dmg = os.path.join(work, asset)
    _download(f"{RELEASE_URL}/{asset}", dmg)
    _verify(dmg, _release_checksum(asset), asset)
    mountpoint = tempfile.mkdtemp(prefix="mount-", dir=work)
    _run("hdiutil", ["attach", "-nobrowse", "-readonly", "-quiet", "-mountpoint", mountpoint, dmg])
    try:
        _run("ditto", [os.path.join(mountpoint, app), os.path.join(staging, app)])
    finally:
        _run("hdiutil", ["detach", "-quiet", mountpoint])


def _build_log_path():
    directory = os.path.join(_cache_base(), product.PACKAGE_NAME)
    os.makedirs(directory, exist_ok=True)
    return os.path.join(directory, f"build-{product.VERSION}.log")


def _log_tail(path, count):
    try:
        with open(path, encoding="utf-8", errors="replace") as handle:
            return "\n".join(handle.read().rstrip().splitlines()[-count:])
    except OSError:
        return ""


def _progress(label):
    state = {"last": -1}
    tty = sys.stderr.isatty()

    def on_line(line):
        step = re.match(r"^\[(\d+)/(\d+)\]", line)
        if not step:
            return
        done, total = int(step.group(1)), int(step.group(2))
        percent = done * 100 // total
        if percent == state["last"]:
            return
        if tty:
            write(f"\r[{product.PACKAGE_NAME}] {label}: {done}/{total} ({percent}%)\x1b[K")
            if done == total:
                write("\n")
        elif percent % 10 == 0:
            log(f"{label}: {percent}%")
        state["last"] = percent

    return on_line


def _build_from_source(staging, work):
    env = build_environment(product.WINDOWS_ENV)
    try:
        preflight(env, log)
    except PreflightError as error:
        raise InstallError(str(error)) from error

    expected = _source_checksum()
    tarball = os.path.join(work, f"{product.PACKAGE_NAME}-{product.VERSION}.tar.gz")
    _download(SOURCE_URL, tarball)
    if expected:
        _verify(tarball, expected, f"the {product.VERSION} source tarball")
    else:
        log("this package records no source checksum (unreleased checkout); "
            "the tarball is trusted over HTTPS only")

    log_path = _build_log_path()
    source = os.path.join(work, product.SOURCE_DIRECTORY)
    build = os.path.join(work, "build")
    log(f"building {product.NAME} {product.VERSION} from source; this takes several minutes")
    log(f"full build output: {log_path}")
    with open(log_path, "w", encoding="utf-8") as build_log:
        try:
            tar_args = ["-xzf", build_path(tarball), "-C", build_path(work)]
            if sys.platform == "win32":
                tar_args.insert(0, "--force-local")
            run_logged("tar", tar_args, env, build_log)
            log("configuring")
            run_logged("meson", ["setup", build_path(build), build_path(source), *product.MESON_OPTIONS,
                                 f"--prefix={build_path(staging)}"], env, build_log)
            run_logged("meson", ["compile", "-C", build_path(build)], env, build_log,
                       _progress("compiling"))
            log("installing")
            run_logged("meson", ["install", "-C", build_path(build), "--quiet"], env, build_log)
        except RuntimeError as error:
            build_log.close()
            raise InstallError(f"{error}\n\nLast lines of {log_path}:\n\n{_log_tail(log_path, 25)}\n\n"
                               f"The full output is in {log_path}.") from error
    os.remove(log_path)
    msys2 = locate_msys2()
    with open(os.path.join(staging, RUNTIME_FILE), "w", encoding="utf-8") as handle:
        json.dump({"path": msys2["bin"] if msys2 else None}, handle)
        handle.write("\n")


def install():
    """Return (executable, environment) for the installed browser, installing it first if needed."""
    existing = find_installed()
    if existing:
        return existing
    root = install_root()
    os.makedirs(os.path.dirname(root), exist_ok=True)
    staging = f"{root}.partial-{os.getpid()}"
    work = tempfile.mkdtemp(prefix=f"{product.PACKAGE_NAME}-")
    shutil.rmtree(staging, ignore_errors=True)
    os.makedirs(staging)
    try:
        if _uses_mac_release():
            _install_mac_release(staging, work)
        else:
            _build_from_source(staging, work)
        if not os.path.exists(os.path.join(staging, _executable_path())):
            raise InstallError(f"the install did not produce {_executable_path()}")
        _mark_executable(staging)
        shutil.rmtree(root, ignore_errors=True)
        os.replace(staging, root)
    finally:
        shutil.rmtree(staging, ignore_errors=True)
        shutil.rmtree(work, ignore_errors=True)
    log(f"installed {product.NAME} {product.VERSION} in {root}")
    return find_installed()


def remove():
    """Delete every installed version of the browser this package put in place."""
    base = os.path.dirname(install_root())
    if os.path.isdir(base):
        shutil.rmtree(base)
        log(f"removed {base}")
    else:
        log(f"nothing installed in {base}")
