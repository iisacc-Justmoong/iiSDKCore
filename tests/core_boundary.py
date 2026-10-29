"""Core production code and installed runtime must not import a feature SDK or Qt."""
import argparse
from pathlib import Path
import re


def verify(source, build=None):
    for path in (source / "src").rglob("*"):
        if path.suffix not in (".h", ".cpp", ".mm") or not path.is_file():
            continue
        text = path.read_text()
        if re.search(r'#\s*include\s*[<"](?:ii(?!SDKCore[/\"])[A-Z][^>"/]*|Qt[^>"/]*|Q[A-Z][^>"/]*)(?:[/>"])', text):
            raise RuntimeError("Core imports a feature SDK or Qt: " + str(path))
    package = (source / "cmake/iiSDKCoreConfig.cmake.in").read_text()
    dependencies = re.findall(r'find_dependency\(\s*([^\s\)]+)', package)
    if any(name != "Threads" for name in dependencies):
        raise RuntimeError("Core package has an upper dependency: " + repr(dependencies))
    if (source / "src/IO/AtomicFile.cpp").exists():
        raise RuntimeError("Concrete atomic file publication belongs to the file provider")
    if build:
        manifests = list(build.glob("core-boundary-*.txt"))
        if not manifests:
            raise RuntimeError("Core target dependency manifest missing")
        for path in manifests:
            contracts={"iiSDKCore::Contracts","iiSDKCoreContracts"}
            runtime=contracts|{"Threads::Threads","dl","iiSDKCore::Observation","iiSDKCoreObservation"}
            allowed={"Runtime":runtime,"RuntimeInterface":runtime,"Contracts":set(),"Values":contracts,
                     "Observation":contracts,"ObservationInterface":contracts}
            sections={};current=None
            for line in path.read_text().splitlines():
                if re.fullmatch(r'\[[A-Za-z]+\]',line):
                    current=line[1:-1]
                    if current not in allowed or current in sections:raise RuntimeError("Invalid Core dependency section: "+current)
                    sections[current]=[]
                elif line.strip():
                    if current is None:raise RuntimeError("Core dependency manifest lacks target sections")
                    sections[current]+=line.split(';')
            if set(sections)!=set(allowed):raise RuntimeError("Core dependency manifest is incomplete")
            for section,dependencies in sections.items():
                for dependency in dependencies:
                    dependency=re.sub(r'^\$<LINK_ONLY:([^>]+)>$',r'\1',dependency)
                    if dependency and dependency not in allowed[section]:
                        raise RuntimeError(section+" links a non-foundation dependency: "+dependency)
        if (build / "include/iiSDKCore/IO/AtomicFile.h").exists():
            raise RuntimeError("Stale concrete file header in generated Core includes")
    print("Core foundation boundary passed: standard/platform dependencies only")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--build", type=Path)
    args = parser.parse_args()
    verify(args.source.resolve(), args.build.resolve() if args.build else None)
