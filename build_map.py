"""Build a function-to-tests map using only the clean upstream revision."""
import ast
import json
import os
import subprocess
import sys
import tokenize
import unittest
from contextlib import redirect_stdout
from pathlib import Path

REV = "eb11aec4be8cb2122afe713a4816662c3f0fb2fa"


def optimizer_fixture_readers(tests):
    # On the pinned revision, flat optimizer SQL fixtures are loaded by
    # TestOptimizer.check_file or by direct loader calls in its test methods.
    # Executor class fixtures live in subdirectories and use the dynamic map.
    path = Path("tests/test_optimizer.py")
    numbers = {test["id"]: i for i, test in enumerate(tests)}
    readers = {}
    tree = ast.parse(path.read_bytes())
    for cls in (node for node in tree.body if isinstance(node, ast.ClassDef)):
        for method in cls.body:
            if not isinstance(method, ast.FunctionDef) or not method.name.startswith("test_"):
                continue
            index = numbers.get(f"{path.as_posix()}::{cls.name}::{method.name}")
            if index is None:
                continue
            for call in (node for node in ast.walk(method) if isinstance(node, ast.Call)):
                func = call.func
                if isinstance(func, ast.Attribute) and func.attr == "check_file":
                    template = "optimizer/{}.sql"
                elif isinstance(func, ast.Name) and func.id in ("load_sql_fixtures", "load_sql_fixture_pairs"):
                    template = "{}"
                else:
                    continue
                if not call.args or not isinstance(call.args[0], ast.Constant):
                    return {}
                name = Path("tests/fixtures") / template.format(call.args[0].value)
                if name.parent == Path("tests/fixtures/optimizer") and name.is_file():
                    readers.setdefault(name.as_posix(), set()).add(index)
    return {name: sorted(indices) for name, indices in readers.items()}


def protected_insertions(path):
    spans = []
    strings = []
    with path.open("rb") as stream:
        for token in tokenize.tokenize(stream.readline):
            kind = tokenize.tok_name[token.type]
            if token.start[0] < token.end[0]:
                spans.append((token.start[0], token.end[0]))
            if kind in ("FSTRING_START", "TSTRING_START"):
                strings.append(token.start[0])
            elif kind in ("FSTRING_END", "TSTRING_END"):
                spans.append((strings.pop(), token.end[0]))
    # A comment on the next line can interrupt an explicit line continuation.
    spans.extend([n, n + 1] for n, line in enumerate(path.read_bytes().splitlines(), 1)
                 if line.endswith(b"\\"))
    mask = 0
    for first, end in spans:
        mask |= ((1 << (end - first)) - 1) << first
    return format(mask, "x")


def main(root, catalog, destination):
    root = root.resolve()
    if subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip() != REV:
        raise ValueError("Expected the contest revision")
    tests = json.loads(catalog.read_text())["tests"]
    destination = destination.resolve()
    os.chdir(root)
    sys.path.insert(0, str(root))
    paths = {}
    ranges = {}
    for directory in ("tests", "sqlglot"):
        for path in Path(directory).rglob("*.py"):
            key = path.as_posix()
            paths[os.path.normcase(str(path.resolve()))] = key
            ranges[key] = {}
            for node in ast.walk(ast.parse(path.read_bytes())):
                if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
                    start = min([node.lineno] + [x.lineno for x in node.decorator_list])
                    # Restrict selection to edits strictly inside a multi-line body.
                    ranges[key][start] = [node.body[0].lineno, node.end_lineno, []]

    visited = set()
    fixture_paths = {os.path.normcase(str(p.resolve())): p.as_posix()
                     for p in Path("tests/fixtures").rglob("*.sql")}
    opened = set()
    fixtures = {key: [] for key in fixture_paths.values()}

    def record_open(event, args):
        if event == "open" and isinstance(args[0], str):
            key = fixture_paths.get(os.path.normcase(os.path.abspath(args[0])))
            if key:
                opened.add(key)

    sys.addaudithook(record_open)
    unknown = []
    monitor = sys.monitoring
    tool = 3
    monitor.use_tool_id(tool, "blast-radius")

    def entered(code, offset):
        key = paths.get(os.path.normcase(code.co_filename))
        if key and code.co_firstlineno in ranges[key]:
            visited.add((key, code.co_firstlineno))
        return monitor.DISABLE

    monitor.register_callback(tool, monitor.events.PY_START, entered)
    for index, test in enumerate(tests):
        parts = test["id"].split("::")
        name = ".".join([parts[0][:-3].replace("/", ".")] + parts[1:])
        visited.clear()
        opened.clear()
        try:
            suite = unittest.defaultTestLoader.loadTestsFromName(name)
            result = unittest.TestResult()
            monitor.restart_events()
            monitor.set_events(tool, monitor.events.PY_START)
            with redirect_stdout(sys.stderr):
                suite.run(result)
            ok = result.testsRun == 1 and result.wasSuccessful() and not result.skipped
        except Exception:
            ok = False
        finally:
            monitor.set_events(tool, 0)
        if ok:
            for key in opened:
                fixtures[key].append(index)
            for key, start in visited:
                ranges[key][start][2].append(index)
        else:
            unknown.append(index)
    for entries in ranges.values():
        for span in entries.values():
            indices = span[2]
            if len(indices) >= 64:
                mask = sum(1 << index for index in indices)
                span[2] = format(mask, "x")
    index_data = {key: list(entries.values()) for key, entries in ranges.items()}
    destination.write_text(json.dumps({"ids": [t["id"] for t in tests], "unknown": unknown,
                                      "files": index_data, "fixtures": fixtures,
                                      "continued": {key: protected_insertions(Path(key)) for key in ranges},
                                      "optimizer_readers": optimizer_fixture_readers(tests)},
                                     separators=(",", ":")))
    print(f"{len(tests)} tests, {len(unknown)} unmeasured", file=sys.stderr)


if __name__ == "__main__":
    main(*(Path(p) for p in sys.argv[1:]))