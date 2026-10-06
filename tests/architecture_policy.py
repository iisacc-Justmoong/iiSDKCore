"""Regression checks for the mandatory C++ inheritance policy."""
import importlib.util
import argparse
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("inheritance_policy", REPO / "tools/check_inheritance.py")
policy = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = policy
spec.loader.exec_module(policy)
CONFIG, BUILD = None, None


class InheritancePolicyTests(unittest.TestCase):
    def findings(self, source, sealed=False):
        return policy.inspect(source, Path("fixture.h"), sealed=sealed)

    def test_multiple_bases(self):
        for source in [
            "struct Child : First, Second {};",
            "class Child final : public First, private Second {};",
            "struct Child : virtual First, virtual Second {};",
            "struct Child : Base<int, float>, Other {};",
            "struct Child : Base<Inner<int, float>>, Other {};",
            "struct Child : Base<(1 < 2), int>, Other {};",
        ]:
            with self.subTest(source=source):
                self.assertTrue(self.findings(source))

    def test_single_inheritance_and_composition(self):
        for source in [
            "struct Child final : Base<int, float> {};",
            "template<class T, class U> struct Child final : Base<T,U> {};",
            "struct Child final : decltype(factory(1, 2)) {};",
            "struct Outer final { First first; Second second; };",
            "enum class Kind : unsigned { first, second };",
            "struct Forward; class Other; struct stat info{};",
            "struct Owner::State final {};",
        ]:
            with self.subTest(source=source):
                self.assertFalse(self.findings(source))

    def test_comments_strings_and_attributes(self):
        self.assertFalse(self.findings('/* class Bad : A,B {}; */ struct [[nodiscard]] Good final {};'))
        self.assertFalse(self.findings('const char* text="struct Bad : A,B {};";'))
        self.assertFalse(self.findings('const char* text=R"tag(struct Bad : A,B {}; )tag";'))
        self.assertTrue(self.findings('class __declspec(dllexport) Bad final : A,B {};'))

    def test_base_pack_is_not_a_safe_single_base(self):
        self.assertTrue(self.findings("template<class... Bases> struct Child : Bases... {};"))
        self.assertFalse(self.findings("template<class... T> struct Child final : Base<T...> {};"))

    def test_sealed_public_records(self):
        self.assertTrue(self.findings("struct Options { int count; };", sealed=True))
        self.assertFalse(self.findings("struct Options final { int count; };", sealed=True))
        self.assertFalse(self.findings("class Forward;", sealed=True))
        self.assertTrue(self.findings("typedef struct { int count; } Options;", sealed=True))

    def test_inactive_source_still_rejects_multiple_inheritance(self):
        self.assertTrue(self.findings("#if 0\nstruct Bad : A,B {};\n#endif\n"))

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_utf8_compiler_output_retains_policy_records(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            path = Path(temporary) / "utf8.cpp"
            path.write_text('const char* text="한글"; struct A {}; struct B {}; struct Bad final : A,B {};\n', encoding="utf-8")
            config = dict(CONFIG)
            if config["msvc"]:
                config["flags"] = [*config["flags"], "/utf-8"]
            output = policy.preprocess(config, str(path))
            self.assertIn("한글", output)
            tokens, _ = policy.preprocessed_tokens(output, [path.parent])
            self.assertTrue(policy.inspect_tokens(tokens))

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_macros_are_checked_after_expansion(self):
        for text in [
            "struct A {}; struct B {};\n#define BASES A,B\nstruct Child final : BASES {};",
            "struct A {}; struct B {};\n#define DECL(N,...) struct N final : __VA_ARGS__ {}\nDECL(Child,A,B);",
        ]:
            with self.subTest(text=text), tempfile.TemporaryDirectory(dir=BUILD) as temporary:
                path = Path(temporary) / "macro.cpp"
                path.write_text(text)
                tokens, _ = policy.preprocessed_tokens(policy.preprocess(CONFIG, str(path)), [path.parent])
                self.assertTrue(policy.inspect_tokens(tokens))

    @unittest.skipUnless(sys.platform == "darwin" and "--config" in sys.argv, "Apple compiler configuration required")
    def test_objective_cpp_bridge_cannot_hide_multiple_inheritance(self):
        if "em++" in CONFIG["compiler"] or any(flag.startswith("--target=") for flag in CONFIG["flags"]):
            self.skipTest("native Objective-C++ compiler required")
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            path = Path(temporary) / "bridge.mm"
            path.write_text("struct A {}; struct B {};\n#define BASES A,B\nstruct Bridge final : BASES {};\n")
            tokens, _ = policy.preprocessed_tokens(policy.preprocess(CONFIG, str(path)), [path.parent])
            self.assertTrue(policy.inspect_tokens(tokens))

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_public_sdk_types_cannot_be_inherited(self):
        prefix = '#include <iiSDKCore/Runtime/Runtime.h>\n#include <iiSDKCore/Runtime/ObjectProperties.h>\n#include <iiSDKCore/IO/AsyncIO.h>\n'
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            path = Path(temporary) / "consumer.cpp"
            architecture = (CONFIG["architectures"] or [None])[0]
            for base in ["ii::runtime::RuntimeOptions", "ii::runtime::PropertyDefinition", "ii::io::Read"]:
                with self.subTest(base=base):
                    path.write_text(prefix + f"struct Forbidden : {base} {{}};\n")
                    result = subprocess.run(policy.compiler_command(CONFIG, path, syntax=True, architecture=architecture),
                                            text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                    self.assertNotEqual(result.returncode, 0)
                    self.assertRegex(result.stdout + result.stderr, r"(?i)final|C3246")
            path.write_text(prefix + 'struct Valid { ii::runtime::RuntimeOptions runtime; ii::runtime::PropertyDefinition property; ii::io::Read io; };\n')
            result = subprocess.run(policy.compiler_command(CONFIG, path, syntax=True, architecture=architecture),
                                    text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_public_records_created_by_macros_must_be_final(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            root = Path(temporary).resolve()
            path = root / "public.h"
            for qualifier in ["", "final"]:
                path.write_text(f"#define RECORD struct\n#define DECLARE(Name) RECORD Name {qualifier} {{}}\nDECLARE(Value);\n")
                self.assertFalse(policy.inspect(path.read_text(), path, sealed=True))
                tokens, _ = policy.preprocessed_tokens(policy.preprocess(CONFIG, str(path)), [root])
                self.assertEqual(bool(policy.inspect_tokens(tokens, public_roots=[root])), not qualifier)

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_cpp_standard_single_inheritance_is_accepted(self):
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            path = Path(temporary) / "single.cpp"
            path.write_text("template<class T,class U> struct Base {}; struct Child final : Base<int,float> {};\n")
            tokens, _ = policy.preprocessed_tokens(policy.preprocess(CONFIG, str(path)), [path.parent])
            self.assertFalse(policy.inspect_tokens(tokens))

    @unittest.skipUnless("--config" in sys.argv, "CMake compiler configuration required")
    def test_build_without_tests_rejects_private_macro_and_invalidates_cache(self):
        if "em++" in CONFIG["compiler"] or any(flag.startswith("--target=") for flag in CONFIG["flags"]):
            self.skipTest("nested native build is tested by native configurations")
        with tempfile.TemporaryDirectory(dir=BUILD) as temporary:
            root = Path(temporary).resolve()
            (root / "src").mkdir()
            (root / "tools").mkdir()
            shutil.copyfile(REPO / "tools/check_inheritance.py", root / "tools/check_inheritance.py")
            source = root / "src/value.cpp"
            source.write_text('struct A {}; struct B {};\n#if SECOND_BASE && (!defined(__STRICT_ANSI__) || defined(_MSC_VER))\n#define BASES A,B\n#else\n#define BASES A\n#endif\nstruct Value final : BASES {};\n')
            template = f'''cmake_minimum_required(VERSION 3.31)
project(PolicyFixture LANGUAGES CXX)
include(CTest)
include("{(REPO / 'cmake/ArchitecturePolicy.cmake').as_posix()}")
add_library(iiSDKCore STATIC src/value.cpp)
target_compile_features(iiSDKCore PUBLIC cxx_std_23)
target_compile_definitions(iiSDKCore PRIVATE SECOND_BASE={{second}})
iisdkcore_enforce_architecture()
'''

            def configure(second):
                (root / "CMakeLists.txt").write_text(template.format(second=second))
                result = subprocess.run([shutil.which("cmake"), "-S", str(root), "-B", str(root / "build"),
                    "-DBUILD_TESTING=OFF", "-DCMAKE_CXX_COMPILER=" + CONFIG["compiler"]],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                self.assertEqual(result.returncode, 0, result.stdout)

            def build():
                return subprocess.run([shutil.which("cmake"), "--build", str(root / "build"),
                    "--target", "iiSDKCore", "--config", "Release"],
                    text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

            configure(0)
            accepted = build()
            self.assertEqual(accepted.returncode, 0, accepted.stdout)
            objects = {path: path.stat().st_mtime_ns for path in (root / "build").rglob("*")
                       if path.suffix in {".o", ".obj"}}
            self.assertTrue(objects)
            configure(1)
            rejected = build()
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("multiple inheritance is forbidden", rejected.stdout)
            self.assertTrue(all(path.stat().st_mtime_ns == stamp for path, stamp in objects.items()))
            source.write_text("struct A {}; struct B {}; struct Value final { A a; B b; };\n")
            repaired = build()
            self.assertEqual(repaired.returncode, 0, repaired.stdout)
            header = root / "src/public.h"
            header.write_text("#define RECORD struct\n#define DECLARE(Name) RECORD Name {}\nDECLARE(Unsealed);\n")
            source.write_text('#include "public.h"\nstruct Value final {};\n')
            rejected = build()
            self.assertNotEqual(rejected.returncode, 0)
            self.assertIn("public SDK records must be final", rejected.stdout)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path)
    parser.add_argument("--build", type=Path)
    args, remaining = parser.parse_known_args()
    if args.config:
        CONFIG = json.loads(args.config.read_text())
        BUILD = args.build
        BUILD.mkdir(parents=True, exist_ok=True)
    unittest.main(argv=[sys.argv[0], *remaining])
