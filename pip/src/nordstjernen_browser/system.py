"""Runs build programs natively, or from MSYS2's MinGW environment on Windows."""

import os
import re
import shutil
import subprocess
import sys

MSYS2_ENVIRONMENTS = [
    {"directory": "ucrt64", "msystem": "UCRT64", "package_prefix": "mingw-w64-ucrt-x86_64-"},
    {"directory": "mingw64", "msystem": "MINGW64", "package_prefix": "mingw-w64-x86_64-"},
]


def write(text):
    sys.stderr.write(text)
    sys.stderr.flush()


def _msys2_roots():
    candidates = [os.environ.get("MSYS2_ROOT"), "C:\\msys64",
                  os.path.join(os.environ.get("LOCALAPPDATA", ""), "msys64")]
    return [root for root in candidates
            if root and os.path.exists(os.path.join(root, "usr", "bin", "bash.exe"))]


def locate_msys2():
    if sys.platform != "win32":
        return None
    roots = _msys2_roots()
    if not roots:
        return None
    for root in roots:
        for environment in MSYS2_ENVIRONMENTS:
            if os.path.exists(os.path.join(root, environment["directory"], "bin", "libgtk-4-1.dll")):
                return dict(environment, root=root,
                            bin=os.path.join(root, environment["directory"], "bin"))
    environment = MSYS2_ENVIRONMENTS[0]
    return dict(environment, root=roots[0], bin=os.path.join(roots[0], environment["directory"], "bin"))


def resolve(command, env=None):
    path = (env or os.environ).get("PATH")
    return shutil.which(command, path=path) or command


def capture(command, args, env=None):
    try:
        result = subprocess.run([resolve(command, env), *args], capture_output=True, text=True,
                                env=env, check=False)
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip()


def _brew_prefix(formula):
    return capture("brew", ["--prefix", formula]) if sys.platform == "darwin" else None


def build_environment(windows_env):
    env = dict(os.environ)
    msys2 = locate_msys2()
    if msys2:
        env["PATH"] = os.pathsep.join([msys2["bin"], os.path.join(msys2["root"], "usr", "bin"),
                                       env.get("PATH", "")])
        env["MSYSTEM"] = msys2["msystem"]
        env.update(windows_env)
    kegs = [os.path.join(prefix, "lib", "pkgconfig")
            for prefix in map(_brew_prefix, ["curl", "openssl@3"]) if prefix]
    if kegs:
        existing = [os.environ["PKG_CONFIG_PATH"]] if os.environ.get("PKG_CONFIG_PATH") else []
        env["PKG_CONFIG_PATH"] = os.pathsep.join(kegs + existing)
    return env


def runtime_environment(runtime_path):
    env = dict(os.environ)
    if runtime_path:
        env["PATH"] = os.pathsep.join([runtime_path, env.get("PATH", "")])
    return env


def build_path(path):
    return path.replace("\\", "/") if sys.platform == "win32" else path


def run_logged(command, args, env, log, on_line=None):
    log.write(f"\n$ {command} {' '.join(args)}\n")
    log.flush()
    try:
        child = subprocess.Popen([resolve(command, env), *args], env=env, stdin=subprocess.DEVNULL,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, errors="replace")
    except OSError as error:
        raise RuntimeError(f"{command}: {error}") from error
    for line in child.stdout:
        log.write(line)
        if on_line:
            on_line(line.rstrip("\r\n"))
    child.wait()
    log.flush()
    if child.returncode != 0:
        raise RuntimeError(f"{command} {args[0]} failed (exit {child.returncode})")


def reported_version(output):
    match = re.search(r"\d+\.\d+(\.\d+)?", output)
    return match.group(0) if match else output.splitlines()[0].strip()
