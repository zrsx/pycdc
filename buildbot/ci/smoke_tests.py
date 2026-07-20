#!/usr/bin/env python3
"""Run a deterministic, bounded validation corpus for normal CI."""

import argparse
import ast
import difflib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import xml.etree.ElementTree as ET


CORPUS = (
    "simple_const.1.0.pyc",
    "simple_const.1.5.pyc",
    "simple_const.2.2.pyc",
    "simple_const.2.5.pyc",
    "simple_const.2.7.pyc",
    "simple_const.3.0.pyc",
    "simple_const.3.3.pyc",
    "simple_const.3.6.pyc",
    "simple_const.3.8.pyc",
    "simple_const.3.10.pyc",
    "simple_const.3.11.pyc",
    "simple_const.3.12.pyc",
    "binary_ops.3.11.pyc",
    "build_const_key_map.2.7.pyc",
    "build_const_key_map.3.8.pyc",
    "chain_assignment.2.7.pyc",
    "chain_assignment.3.7.pyc",
    "conditional_expressions.3.1.pyc",
    "conditional_expressions.3.9.pyc",
    "contains_op.3.9.pyc",
    "f-string.3.7.pyc",
    "if_elif_else.2.7.pyc",
    "if_elif_else.3.0.pyc",
    "if_elif_else.3.7.pyc",
    "is_op.3.9.pyc",
    "iter_unpack.2.7.pyc",
    "lambdas_assignment.2.7.pyc",
    "list_extend.3.9.pyc",
    "load_method.3.7.pyc",
    "load_method.3.9.pyc",
    "matrix_mult_oper.3.5.pyc",
    "op_precedence.2.7.pyc",
    "op_precedence.3.5.pyc",
    "op_precedence.3.10.pyc",
    "private_name.2.7.pyc",
    "private_name.3.7.pyc",
    "swap.3.11.pyc",
    "test_calls.3.8.pyc",
    "test_dict.2.7.pyc",
    "yield_from.3.9.pyc",
)

AST_FIXTURES = {
    "simple_const.3.8.pyc",
    "simple_const.3.10.pyc",
    "simple_const.3.11.pyc",
    "simple_const.3.12.pyc",
    "binary_ops.3.11.pyc",
    "contains_op.3.9.pyc",
    "is_op.3.9.pyc",
    "swap.3.11.pyc",
}


def run(command, *, expected=0):
    start = time.monotonic()
    completed = subprocess.run(
        [str(item) for item in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    elapsed = time.monotonic() - start
    if completed.returncode != expected:
        raise RuntimeError(
            f"{' '.join(map(str, command))} returned {completed.returncode}; "
            f"expected {expected}\n{completed.stdout}"
        )
    return completed.stdout, elapsed


def token_name(filename):
    return re.sub(r"\.\d+\.\d+\.pyc$", "", filename)


def add_case(suite, report, name, elapsed, error=None):
    case = ET.SubElement(suite, "testcase", name=name, time=f"{elapsed:.3f}")
    result = {"name": name, "seconds": round(elapsed, 4), "status": "passed"}
    if error is not None:
        result["status"] = "failed"
        result["error"] = str(error)
        failure = ET.SubElement(case, "failure", message=str(error).splitlines()[0])
        failure.text = str(error)
    report.append(result)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path, required=True)
    args = parser.parse_args()

    root = Path.cwd()
    build_dir = args.build_dir.resolve()
    pycdc = build_dir / "pycdc"
    pycdas = build_dir / "pycdas"
    token_dump = root / "scripts" / "token_dump"
    reports = root / ".ci" / "reports"
    reports.mkdir(parents=True, exist_ok=True)

    suite = ET.Element("testsuite", name="pycdc-smoke")
    report = []
    failures = 0

    checks = (("pycdc-help", pycdc), ("pycdas-help", pycdas))
    for name, executable in checks:
        started = time.monotonic()
        try:
            output, _ = run((executable, "--help"))
            if "Usage:" not in output:
                raise RuntimeError("help output does not contain a usage line")
            add_case(suite, report, name, time.monotonic() - started)
        except Exception as error:  # pylint: disable=broad-except
            failures += 1
            add_case(suite, report, name, time.monotonic() - started, error)

    with tempfile.TemporaryDirectory(prefix="pycdc-smoke-") as directory:
        output_dir = Path(directory)
        for filename in CORPUS:
            started = time.monotonic()
            try:
                fixture = root / "tests" / "compiled" / filename
                if not fixture.is_file():
                    raise RuntimeError(f"missing fixture: {fixture}")

                disassembly, _ = run((pycdas, fixture))
                if "[Code]" not in disassembly or "[Disassembly]" not in disassembly:
                    raise RuntimeError("disassembler output lacks code/disassembly sections")

                source_path = output_dir / f"{filename}.py"
                run((pycdc, fixture, "-o", source_path))
                source = source_path.read_text(encoding="utf-8")
                if not source.startswith("# Source Generated with Decompyle++"):
                    raise RuntimeError("decompiler output lacks its source header")

                tokens, _ = run((sys.executable, token_dump, source_path))
                expected_path = root / "tests" / "tokenized" / (
                    token_name(filename) + ".txt"
                )
                expected = expected_path.read_text(encoding="utf-8")
                if tokens != expected:
                    difference = "".join(
                        difflib.unified_diff(
                            expected.splitlines(True),
                            tokens.splitlines(True),
                            fromfile=str(expected_path),
                            tofile=str(source_path),
                        )
                    )
                    raise RuntimeError("decompiler output mismatch\n" + difference)

                if filename in AST_FIXTURES:
                    ast.parse(source, filename=str(source_path))
                add_case(suite, report, filename, time.monotonic() - started)
            except Exception as error:  # pylint: disable=broad-except
                failures += 1
                add_case(suite, report, filename, time.monotonic() - started, error)

        malformed = output_dir / "malformed.pyc"
        malformed.write_bytes(b"not Python bytecode\x00\xff")
        for executable in (pycdc, pycdas):
            name = f"edge-malformed-{executable.name}"
            started = time.monotonic()
            try:
                completed = subprocess.run(
                    (str(executable), str(malformed)),
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    check=False,
                )
                if completed.returncode == 0 and "Bad MAGIC!" not in completed.stdout:
                    raise RuntimeError("malformed bytecode was accepted silently")
                add_case(suite, report, name, time.monotonic() - started)
            except Exception as error:  # pylint: disable=broad-except
                failures += 1
                add_case(suite, report, name, time.monotonic() - started, error)

    suite.set("tests", str(len(report)))
    suite.set("failures", str(failures))
    total_time = sum(item["seconds"] for item in report)
    suite.set("time", f"{total_time:.3f}")
    ET.ElementTree(suite).write(
        reports / "smoke-junit.xml", encoding="utf-8", xml_declaration=True
    )
    (reports / "smoke-results.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8"
    )

    passed = len(report) - failures
    print(f"Smoke validation: {passed}/{len(report)} checks passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
