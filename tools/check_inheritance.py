"""Check project records before compilation, including expanded macros.

Uses the configured C++ preprocessor rather than another C++ toolchain. The
record-head scanner is deliberately conservative about expanding base packs.
"""
import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys


LEX = re.compile(
    r'(?P<comment>//[^\n]*|/\*.*?\*/)'
    r'|(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)")'
    r'|(?P<number>\d[\w\x27.]*)'
    r'|(?P<literal>(?:u8|u|U|L)?"(?:\\.|[^"\\])*"|(?:u8|u|U|L)?\x27(?:\\.|[^\x27\\])*\x27)'
    r'|(?P<word>[^\W\d]\w*)|(?P<punct>::|\.\.\.|[^\s])', re.DOTALL)
LINE = re.compile(r'^\s*#\s*(?:line\s+)?(\d+)\s+"((?:\\.|[^"\\])*)"')
PUBLIC_SUFFIXES = {".h", ".hpp", ".hh", ".hxx"}
SOURCE_SUFFIXES = PUBLIC_SUFFIXES | {".cpp", ".cc", ".cxx", ".mm"}


@dataclass
class Token:
    text: str
    path: Path
    line: int
    end: int


def tokenize(source, path, start_line=1):
    tokens = []
    cursor, line = 0, start_line
    for match in LEX.finditer(source):
        line += source.count("\n", cursor, match.start())
        if match.lastgroup not in {"comment", "raw", "literal", "number"}:
            tokens.append(Token(match.group(), path, line, match.end()))
        line += match.group().count("\n")
        cursor = match.end()
    return tokens


def balanced(tokens, index, opening, closing):
    depth = 0
    while index < len(tokens):
        value = tokens[index].text
        if value == opening:
            depth += 1
        elif value == closing:
            depth -= 1
            if depth == 0:
                return index + 1
        index += 1
    return index


def adornments(tokens, index):
    while index < len(tokens):
        value = tokens[index].text
        if value == "[" and index + 1 < len(tokens) and tokens[index + 1].text == "[":
            index = balanced(tokens, index, "[", "]")
        elif value in {"alignas", "__attribute__", "__declspec"} and index + 1 < len(tokens) and tokens[index + 1].text == "(":
            index = balanced(tokens, index + 1, "(", ")")
        else:
            break
    return index


def records(tokens):
    for start, token in enumerate(tokens):
        if token.text not in {"class", "struct"} or (start and tokens[start - 1].text == "enum"):
            continue
        index = adornments(tokens, start + 1)
        if index < len(tokens) and tokens[index].text == "{":
            yield {"token": token, "name": "<anonymous>", "name_end": token.end,
                   "final": False, "bases": 0, "pack": False, "malformed": False}
            continue
        if index >= len(tokens) or not tokens[index].text.isidentifier():
            continue
        name = tokens[index].text
        name_end = tokens[index].end
        index += 1
        while index < len(tokens):
            if tokens[index].text == "<":
                index = balanced(tokens, index, "<", ">")
                if index:
                    name_end = tokens[index - 1].end
            elif tokens[index].text == "::" and index + 1 < len(tokens):
                name += "::" + tokens[index + 1].text
                name_end = tokens[index + 1].end
                index += 2
            else:
                break
        index = adornments(tokens, index)
        final = index < len(tokens) and tokens[index].text == "final"
        if final:
            index = adornments(tokens, index + 1)
        if index >= len(tokens) or tokens[index].text not in {":", "{"}:
            continue  # forward declarations, template parameters, elaborated types
        bases, pack, malformed = 0, False, False
        if tokens[index].text == ":":
            bases = 1
            index += 1
            stack = []
            while index < len(tokens):
                value = tokens[index].text
                if not stack and value == "{":
                    break
                if not stack and value in {";", "}"}:
                    malformed = True
                    break
                if value in {"(", "[", "{"}:
                    stack.append({"(": ")", "[": "]", "{": "}"}[value])
                elif value == "<" and (not stack or stack[-1] == ">"):
                    stack.append(">")
                elif stack and value == stack[-1]:
                    stack.pop()
                elif not stack and value == ",":
                    bases += 1
                elif not stack and value == "...":
                    pack = True
                index += 1
            malformed |= index == len(tokens)
        yield {"token": token, "name": name, "name_end": name_end,
               "final": final, "bases": bases, "pack": pack, "malformed": malformed}


def inspect_tokens(tokens, sealed=False, public_roots=()):
    issues = []
    for record in records(tokens):
        token = record["token"]
        reason = None
        if record["bases"] > 1:
            reason = "multiple inheritance is forbidden; compose instances instead"
        elif record["pack"]:
            reason = "expanding base packs can introduce multiple inheritance and are forbidden"
        elif record["malformed"]:
            reason = "cannot verify this base clause; use an explicit single base or composition"
        elif (sealed or (token.path.suffix in PUBLIC_SUFFIXES and within(token.path, public_roots))) and not record["final"]:
            reason = "public SDK records must be final; extend them through composition"
        if reason:
            issues.append(f"{token.path}:{token.line}: {record['name']}: {reason}")
    return issues


def inspect(source, path, sealed=False):
    return inspect_tokens(tokenize(source, path), sealed)


def within(path, roots):
    return any(path.is_relative_to(root) for root in roots)


def preprocessed_tokens(output, roots):
    roots = [Path(root).resolve() for root in roots]
    tokens, dependencies = [], set()
    current, number, buffered, start, scoped = None, 1, [], 1, False

    def flush():
        if buffered:
            tokens.extend(tokenize("".join(buffered), current, start))
            buffered.clear()

    for line in output.splitlines(keepends=True):
        marker = LINE.match(line)
        if marker:
            flush()
            filename = marker[2].replace("\\\\", "\\").replace('\\"', '"')
            current = None if filename.startswith("<") else Path(filename).resolve()
            scoped = current is not None and within(current, roots)
            number = int(marker[1])
            if current and current.is_file():
                dependencies.add(current)
            continue
        if scoped:
            if not buffered:
                start = number
            buffered.append(line)
        else:
            flush()
        number += 1
    flush()
    return tokens, dependencies


def compiler_command(config, source, syntax=False, architecture=None):
    compiler = config["compiler"]
    flags = config["flags"] + config["configuration_flags"].get(config["configuration"].upper(), [])
    if config["msvc"]:
        command = [compiler, *flags, "/nologo", *config.get("standard_option", ["/std:c++latest"]), "/Zs" if syntax else "/E", "/TP"]
        command += ["/I" + directory for directory in config["include_dirs"]]
        command += ["/D" + definition for definition in config["definitions"]]
    else:
        objc = Path(source).suffix == ".mm"
        command = [compiler, *flags, *config.get("standard_option", ["-std=c++23"]), "-fsyntax-only" if syntax else "-E", "-x", "objective-c++" if objc else "c++"]
        if objc:
            command += ["-fobjc-arc", "-fblocks"]
        if architecture:
            command += ["-arch", architecture]
        command += ["-I" + directory for directory in config["include_dirs"]]
        command += ["-D" + definition for definition in config["definitions"]]
    return [*command, str(source)]


def preprocess(config, source):
    outputs = []
    for architecture in config["architectures"] or [None]:
        result = subprocess.run(compiler_command(config, source, architecture=architecture), text=True,
                                encoding="utf-8", errors="replace",
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode:
            raise RuntimeError(f"{source}: inheritance preprocessing failed\n{result.stderr}")
        outputs.append(result.stdout)
    return "\n".join(outputs)


def configurations(config):
    """Use each target's evaluated usage requirements, including linked ones."""
    for context in config["contexts"]:
        sections, current = {}, None
        for line in Path(context["file"]).read_text().splitlines():
            if line in {"[options]", "[include_dirs]", "[definitions]", "[standard]", "[extensions]"}:
                current = line[1:-1]
                sections[current] = []
            elif line:
                if current is None:
                    raise ValueError(f"Invalid compiler context: {context['file']}")
                sections[current].append(line)
        target_config = dict(config)
        target_config["flags"] = config["flags"] + sections["options"]
        target_config["include_dirs"] = sections["include_dirs"]
        target_config["definitions"] = sections["definitions"]
        standard = (sections["standard"] or ["23"])[0]
        extensions = (sections["extensions"] or ["ON" if config["extensions_default"] else "OFF"])[0]
        language_options = config["standard_options"][standard]
        target_config["standard_option"] = language_options["extensions" if extensions in {"ON", "1", "TRUE"} else "strict"]
        for source in context["sources"]:
            yield context["file"] + "|" + source, source, target_config


def check_project(config, cache_path):
    root = Path(config["root"]).resolve()
    roots = [root / folder for folder in ("src", "tests", "examples")]
    roots.append(Path(config["generated_include"]).resolve())
    issues = []
    for folder in roots[:3]:
        for path in sorted(folder.rglob("*")):
            if path.is_file() and path.suffix in SOURCE_SUFFIXES:
                sealed = path.is_relative_to(root / "src") and path.suffix in PUBLIC_SUFFIXES
                issues.extend(inspect(path.read_text(encoding="utf-8"), path, sealed))
    if issues:
        return issues

    fingerprints = {}

    def fingerprint(path):
        path = Path(path)
        key = str(path)
        if key not in fingerprints:
            if not path.is_file():
                fingerprints[key] = None
            elif within(path.resolve(), roots):
                fingerprints[key] = hashlib.sha256(path.read_bytes()).hexdigest()
            else:
                stat = path.stat()
                fingerprints[key] = [stat.st_mtime_ns, stat.st_size]
        return fingerprints[key]

    try:
        cache = json.loads(cache_path.read_text())
    except (OSError, ValueError):
        cache = {}
    updated, processed = {}, 0
    checker_text = Path(__file__).read_text()
    for key, source, target_config in configurations(config):
        signature = hashlib.sha256((json.dumps(target_config, sort_keys=True) + checker_text).encode()).hexdigest()
        previous = cache.get(key, {})
        dependencies = previous.get("dependencies", {})
        if previous.get("signature") == signature and dependencies and all(fingerprint(path) == value for path, value in dependencies.items()):
            updated[key] = previous
            continue
        output = preprocess(target_config, source)
        tokens, dependencies = preprocessed_tokens(output, roots + [Path(source).resolve().parent])
        issues.extend(inspect_tokens(tokens, public_roots=[root / "src", Path(config["generated_include"]).resolve()]))
        dependencies.add(Path(source).resolve())
        dependencies.add(Path(config["compiler"]).resolve())
        updated[key] = {"signature": signature, "dependencies": {str(path): fingerprint(path) for path in sorted(dependencies)}}
        processed += 1
    if not issues:
        cache_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = cache_path.with_suffix(".tmp")
        temporary.write_text(json.dumps(updated, indent=2))
        temporary.replace(cache_path)
        print(f"inheritance policy passed: {len(updated)} translation units, {processed} preprocessed, public records sealed")
    return issues


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    args = parser.parse_args()
    try:
        issues = check_project(json.loads(args.config.read_text()), args.cache)
    except (OSError, ValueError, RuntimeError) as error:
        print(error, file=sys.stderr)
        return 1
    for issue in issues:
        print(issue, file=sys.stderr)
    return bool(issues)


if __name__ == "__main__":
    sys.exit(main())
