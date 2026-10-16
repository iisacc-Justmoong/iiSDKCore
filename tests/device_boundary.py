"""Regression contract: device/media code and native dependencies stay above Core."""
import argparse
import json
from pathlib import Path
import re


def verify(source, build=None):
    forbidden_files = list((source / "src/IO").glob("Hardware*"))
    forbidden_files += [path for path in (source / "src/IO").glob("Protocols.*")]
    if forbidden_files:
        raise RuntimeError("Device implementations remain in Core: " +
                           ", ".join(path.name for path in forbidden_files))
    for path in (source / "src").rglob("*"):
        if path.is_file() and path.suffix in (".h", ".cpp", ".mm"):
            if re.search(r'#\s*include\s*[<"]iiDeviceMultimedia/', path.read_text()):
                raise RuntimeError("Core references an upper module: " + str(path))
    package = (source / "cmake/iiSDKCoreConfig.cmake.in").read_text()
    if re.search(r'PkgConfig|pkg_check_modules|iiDeviceMultimedia', package):
        raise RuntimeError("Core package imports device dependencies")
    if build:
        for name in ("Hardware.h", "Protocols.h"):
            if (build / "include/iiSDKCore/IO" / name).exists():
                raise RuntimeError("Stale device header in generated Core includes: " + name)
        manifests = list((build / "architecture").glob("config-*.json"))
        if not manifests:
            raise RuntimeError("Core architecture manifest missing")
        for manifest in manifests:
            for unit in json.loads(manifest.read_text())["translation_units"]:
                if Path(unit).name.startswith(("Hardware", "Protocols")):
                    raise RuntimeError("Core compiles device code: " + unit)
        dependencies = list(build.glob("core-boundary-*.txt"))
        if not dependencies:
            raise RuntimeError("Core dependency manifest missing")
        forbidden = re.compile(
            r'iiDeviceMultimedia|PkgConfig|Foundation|CoreBluetooth|AudioToolbox|'
            r'CoreMIDI|AVFAudio|IOKit|IOUSBHost|CoreAudio|setupapi|cfgmgr32|'
            r'winusb|runtimeobject|(?:^|[;/])hid(?:[;/]|$)', re.I)
        for path in dependencies:
            if forbidden.search(path.read_text()):
                raise RuntimeError("Core links device/media dependencies: " + path.read_text())
    print("Core device/media boundary passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    verify(args.source.resolve(), args.build.resolve() if args.build else None)
