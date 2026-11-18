"""Assert the first-party Apache-2.0 text, notice, and package export."""
import hashlib
import subprocess
import sys
import tempfile
from pathlib import Path

EXPECTED_LICENSE_SHA256 = "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30"
REQUIRED_PHRASES = (
    "Apache License",
    "Version 2.0, January 2004",
    "http://www.apache.org/licenses/",
    "2. Grant of Copyright License.",
    "3. Grant of Patent License.",
    "END OF TERMS AND CONDITIONS",
    "http://www.apache.org/licenses/LICENSE-2.0",
)
COPYRIGHT = "Copyright (c) 2026 iisacc"
PACKAGE_SPDX = 'set(iiSDKCore_SPDX_LICENSE "Apache-2.0")'
CPYTHON_SPDX = 'set(iiSDKCoreCPython_SPDX_LICENSE "Apache-2.0")'


def fail(message):
    raise SystemExit(message)


def license_bytes(path):
    data = Path(path).read_bytes()
    if hashlib.sha256(data).hexdigest() != EXPECTED_LICENSE_SHA256:
        fail("LICENSE is not the Apache-2.0 text: " + str(path))
    text = data.decode("utf-8")
    for phrase in REQUIRED_PHRASES:
        if phrase not in text:
            fail("LICENSE is missing " + phrase + ": " + str(path))
    if "Permission is hereby granted" in text or "MIT License" in text:
        fail("LICENSE still carries the MIT grant: " + str(path))
    return data


def notice_text(path):
    text = Path(path).read_text(encoding="utf-8")
    if COPYRIGHT not in text:
        fail("NOTICE is missing the copyright line: " + str(path))
    if "Apache License, Version 2.0" not in text:
        fail("NOTICE does not name Apache-2.0: " + str(path))
    return text


def assert_sources(source, config):
    source = Path(source)
    license_bytes(source / "LICENSE")
    notice_text(source / "NOTICE")
    readme = (source / "README.md").read_text(encoding="utf-8")
    if "SPDX-License-Identifier: Apache-2.0" not in readme:
        fail("README license section is not Apache-2.0")
    if "SPDX-License-Identifier: MIT" in readme:
        fail("README still identifies the project as MIT")
    korean = (source / "docs" / "License.md").read_text(encoding="utf-8")
    if "SPDX-License-Identifier: Apache-2.0" not in korean or COPYRIGHT not in korean:
        fail("docs/License.md does not state Apache-2.0 and the copyright line")
    agent = (source / "AGENT.md").read_text(encoding="utf-8")
    if "Apache License 2.0" not in agent or COPYRIGHT not in agent:
        fail("AGENT.md does not state the Apache-2.0 policy")
    for relative in ("cmake/iiSDKCoreConfig.cmake.in", "cmake/EmbeddedConfig.cmake.in"):
        if PACKAGE_SPDX not in (source / relative).read_text(encoding="utf-8"):
            fail(relative + " does not export iiSDKCore_SPDX_LICENSE")
    if CPYTHON_SPDX not in (source / "cmake/iiSDKCoreCPythonConfig.cmake.in").read_text(encoding="utf-8"):
        fail("CPython package config does not export its SPDX identifier")
    generated = Path(config).read_text(encoding="utf-8")
    if PACKAGE_SPDX not in generated:
        fail("generated package config does not export iiSDKCore_SPDX_LICENSE")


def assert_install(build, cmake, datadir):
    source = Path(__file__).resolve().parents[1]
    original = (source / "LICENSE").read_bytes()
    with tempfile.TemporaryDirectory(prefix="iisdkcore-license-") as temporary:
        stage = Path(temporary)
        subprocess.run([cmake, "--install", build, "--prefix", str(stage)], check=True)
        installed_license = stage / datadir / "iiSDKCore" / "LICENSE"
        installed_notice = stage / datadir / "iiSDKCore" / "NOTICE"
        if installed_license.read_bytes() != original:
            fail("installed LICENSE does not match the Apache-2.0 source file")
        license_bytes(installed_license)
        notice_text(installed_notice)
        configs = list(stage.rglob("iiSDKCoreConfig.cmake"))
        if not configs or PACKAGE_SPDX not in configs[0].read_text(encoding="utf-8"):
            fail("installed package config does not export iiSDKCore_SPDX_LICENSE")


def main():
    if len(sys.argv) >= 2 and sys.argv[1] == "--install":
        if len(sys.argv) != 5:
            fail("usage: license_text.py --install BUILD CMAKE DATADIR")
        assert_install(sys.argv[2], sys.argv[3], sys.argv[4])
        return
    if len(sys.argv) != 3:
        fail("usage: license_text.py SOURCE GENERATED_CONFIG")
    assert_sources(sys.argv[1], sys.argv[2])


if __name__ == "__main__":
    main()
