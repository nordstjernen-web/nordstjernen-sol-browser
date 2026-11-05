"""Checks the build tools and libraries a source build needs, and names the packages that provide what is missing."""

import re
import sys

from . import product
from .system import capture, locate_msys2, reported_version

MANAGERS = {
    "apt": {"install": "sudo apt install"},
    "dnf": {"install": "sudo dnf install", "pkgconfig": lambda module: f"'pkgconfig({module})'"},
    "zypper": {"install": "sudo zypper install", "pkgconfig": lambda module: f"'pkgconfig({module})'"},
    "pacman": {"install": "sudo pacman -S --needed"},
    "apk": {"install": "sudo apk add", "pkgconfig": lambda module: f"pc:{module}"},
    "brew": {"install": "brew install"},
    "msys": {"install": "pacman -S --needed"},
}

INSTALL_AGAIN = ["", "Then run the installer again: nordstjernen-install"]


class PreflightError(Exception):
    pass


def version_at_least(found, minimum):
    a = [int(p) if p.isdigit() else 0 for p in re.split(r"[.+~-]", found)]
    b = [int(p) if p.isdigit() else 0 for p in minimum.split(".")]
    for i, wanted in enumerate(b):
        have = a[i] if i < len(a) else 0
        if have != wanted:
            return have > wanted
    return True


def _os_release():
    try:
        with open("/etc/os-release", encoding="utf-8") as handle:
            text = handle.read()
    except OSError:
        return "", ""

    def field(name):
        match = re.search(rf'^{name}="?([^"\n]*)', text, re.M)
        return match.group(1) if match else ""

    return f"{field('ID')} {field('ID_LIKE')}".lower(), field("PRETTY_NAME")


def _system_name():
    if sys.platform == "win32":
        msys2 = locate_msys2()
        return f"MSYS2 {msys2['msystem']} ({msys2['root']})" if msys2 else ""
    return _os_release()[1]


def _package_manager():
    if sys.platform == "win32":
        return "msys"
    if sys.platform == "darwin":
        return "brew"
    ids = _os_release()[0]
    for pattern, manager in [("debian|ubuntu", "apt"), ("fedora|rhel|centos", "dnf"),
                             ("suse", "zypper"), ("arch", "pacman"), ("alpine", "apk")]:
        if re.search(pattern, ids):
            return manager
    return None


def _platform():
    if sys.platform.startswith("linux"):
        return "linux"
    return sys.platform


def applies(entry):
    platform = _platform()
    if "platforms" in entry:
        return platform in entry["platforms"]
    if entry.get("linux"):
        return platform == "linux"
    if entry.get("unix"):
        return platform not in ("darwin", "win32")
    if entry.get("mac"):
        return platform == "darwin"
    if entry.get("windows"):
        return platform == "win32"
    return True


def _package_for(entry, manager):
    if not manager:
        return None
    if manager == "msys":
        if entry.get("msysBase"):
            return entry["msysBase"]
        msys2 = locate_msys2()
        return f"{msys2['package_prefix']}{entry['msys']}" if entry.get("msys") and msys2 else None
    if entry.get("module") and "pkgconfig" in MANAGERS[manager]:
        return MANAGERS[manager]["pkgconfig"](entry["module"])
    return entry.get(manager)


def _install_command(entries, manager):
    packages = list(dict.fromkeys(p for p in (_package_for(e, manager) for e in entries) if p))
    if not packages:
        return []
    lines, line = [], f"  {MANAGERS[manager]['install']}"
    for name in packages:
        if len(line) + len(name) > 76:
            lines.append(f"{line} \\")
            line = "     "
        line += f" {name}"
    lines.append(line)
    return lines


def _install_instructions(entries, manager, purpose):
    system = _system_name()
    if manager == "msys":
        msys2 = locate_msys2()
        return ["", f"{purpose}, open the MSYS2 {msys2['msystem']} terminal of {msys2['root']} and run:",
                "", *_install_command(entries, manager)]
    if not manager:
        return ["", "Install the packages that provide these pkg-config modules and programs "
                    "with your system's package manager."]
    on = f" on {system}" if system else ""
    return ["", f"{purpose}{on}, run:", "", *_install_command(entries, manager)]


def _check_tools(env):
    missing, outdated = [], []
    for tool in filter(applies, product.TOOLS):
        found = next((out for out in (capture(c, ["--version"], env) for c in tool["probe"])
                      if out is not None), None)
        if found is None:
            missing.append(tool)
        elif tool.get("minimum"):
            version = reported_version(found)
            if not version_at_least(version, tool["minimum"]):
                outdated.append(dict(tool, found=version))
    return missing, outdated


def _check_libraries(entries, env):
    missing, outdated = [], []
    for entry in filter(applies, entries):
        found = capture("pkg-config", ["--modversion", entry["module"]], env)
        if found is None:
            missing.append(entry)
        elif entry.get("minimum") and not version_at_least(found, entry["minimum"]):
            outdated.append(dict(entry, found=found))
    return missing, outdated


def _describe(entry):
    name = entry.get("module") or entry["label"]
    wanted = f" {entry['minimum']} or newer" if entry.get("minimum") else ""
    if entry.get("found"):
        return f"{name} {entry['found']} ({name}{wanted} is needed)"
    return f"{name}{wanted}"


def _heading():
    return f"{product.NAME} is built from source on this system, and the build cannot start yet."


def _report(tools, libraries, manager):
    missing_tools, outdated_tools = tools
    missing_libs, outdated_libs = libraries
    lines = [_heading()]
    missing = missing_tools + missing_libs
    if missing:
        lines += ["", "Not installed:"]
        for entry in missing:
            provider = _package_for(entry, manager)
            suffix = f"  (package {provider.replace(chr(39), '')})" if provider else ""
            lines.append(f"  - {_describe(entry)}{suffix}")
        for note in dict.fromkeys(e["note"] for e in missing if e.get("note")):
            lines.append(f"    {note}")
        lines += _install_instructions(missing, manager, "To install them")
    if outdated_tools:
        lines += ["", "Older than the build needs:", *(f"  - {_describe(e)}" for e in outdated_tools)]
        if manager == "msys":
            lines += ["", "Update MSYS2 with: pacman -Syu"]
        elif manager == "brew":
            lines += ["", "Update them with: brew update && brew upgrade"]
        else:
            for entry in outdated_tools:
                if entry.get("upgrade"):
                    lines += ["", *entry["upgrade"]]
    if outdated_libs:
        system = _system_name()
        on = f" on {system}" if system else ""
        lines += ["", f"Libraries older than the build needs{on}:",
                  *(f"  - {_describe(e)}" for e in outdated_libs)]
        if manager == "msys":
            lines += ["", "Update MSYS2 with: pacman -Syu"]
        elif manager == "brew":
            lines += ["", "Update them with: brew update && brew upgrade"]
        else:
            lines += ["", "These versions come with your distribution release, so installing "
                          "packages will not fix them.",
                      f"{product.NAME} builds on {product.SUPPORTED_DISTRIBUTIONS}."]
    if missing or outdated_tools or manager in ("brew", "msys"):
        lines += INSTALL_AGAIN
    return "\n".join(lines)


def _missing_msys2_report():
    prefix = "mingw-w64-ucrt-x86_64-"
    packages = [e.get("msysBase") or (e.get("msys") and f"{prefix}{e['msys']}")
                for e in filter(applies, product.TOOLS + product.REQUIRED)]
    lines = [f"On Windows, {product.NAME} is built from source with MSYS2, which is not installed.",
             "",
             "Install MSYS2 from https://www.msys2.org (the default location is C:\\msys64; set",
             "MSYS2_ROOT if you choose another), then open its UCRT64 terminal and run:", ""]
    line = "  pacman -S --needed"
    for name in dict.fromkeys(p for p in packages if p):
        if len(line) + len(name) > 76:
            lines.append(f"{line} \\")
            line = "     "
        line += f" {name}"
    return "\n".join([*lines, line, *INSTALL_AGAIN])


def _unchecked_report(missing_tools, manager):
    lines = [_heading(), "", "Not installed:", *(f"  - {_describe(e)}" for e in missing_tools),
             "", "Without pkg-config the libraries cannot be checked."]
    if manager:
        lines += _install_instructions(missing_tools + [e for e in product.REQUIRED if applies(e)],
                                       manager,
                                       "To install the build tools and every library the build needs")
    return "\n".join([*lines, *INSTALL_AGAIN])


def _optional_report(libraries, manager):
    absent = libraries[0] + libraries[1]
    if not absent:
        return None
    features = ", ".join(dict.fromkeys(e["feature"] for e in absent))
    modules = ", ".join(e["module"] for e in absent)
    lines = [f"Building without {features}: {modules} not found."]
    command = _install_command(absent, manager) if manager else []
    if command:
        lines += ["To include them, install these first and run nordstjernen-install again:", *command]
    return "\n".join(lines)


def preflight(env, warn):
    """Raise PreflightError naming what is missing; call warn() for optional features left out."""
    if sys.platform == "win32" and not locate_msys2():
        raise PreflightError(_missing_msys2_report())
    manager = _package_manager()
    missing_tools, outdated_tools = _check_tools(env)
    if sys.platform == "darwin" and any(t.get("compiler") for t in missing_tools):
        raise PreflightError(
            f"{product.NAME} is built from source on this Mac and needs a C compiler. "
            "Install the Xcode command line tools:\n\n  xcode-select --install\n"
            + "\n".join(INSTALL_AGAIN))
    if any(t["label"] == "pkg-config" for t in missing_tools):
        raise PreflightError(_unchecked_report(missing_tools, manager))
    libraries = _check_libraries(product.REQUIRED, env)
    if missing_tools or outdated_tools or libraries[0] or libraries[1]:
        raise PreflightError(_report((missing_tools, outdated_tools), libraries, manager))
    optional = _optional_report(_check_libraries(product.OPTIONAL, env), manager)
    if optional:
        warn(optional)
