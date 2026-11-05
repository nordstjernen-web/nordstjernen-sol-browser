"""The `nordstjernen` and `nordstjernen-install` commands."""

import argparse
import signal
import subprocess
import sys
import traceback

from . import product
from .install import InstallError, find_installed, install, install_root, log, remove


def _installed_or_exit():
    try:
        return install()
    except InstallError as error:
        log(str(error))
        sys.exit(1)
    except Exception:
        log(traceback.format_exc())
        sys.exit(1)


def main():
    exe, env = _installed_or_exit()
    child = subprocess.Popen([exe, *sys.argv[1:]], env=env)
    if sys.platform != "win32":
        for name in ("SIGINT", "SIGTERM", "SIGHUP"):
            signal.signal(getattr(signal, name), lambda number, frame: child.send_signal(number))
    try:
        sys.exit(child.wait())
    except KeyboardInterrupt:
        sys.exit(child.wait())


def install_main():
    parser = argparse.ArgumentParser(
        prog="nordstjernen-install",
        description=f"Install {product.NAME} {product.VERSION} for this system, or remove it.")
    parser.add_argument("--remove", action="store_true",
                        help="delete the installed browser (run before pip uninstall)")
    parser.add_argument("--where", action="store_true", help="print the install directory and exit")
    args = parser.parse_args()
    if args.where:
        print(install_root())
        return
    if args.remove:
        remove()
        return
    if find_installed():
        log(f"{product.NAME} {product.VERSION} is already installed in {install_root()}")
        return
    _installed_or_exit()
