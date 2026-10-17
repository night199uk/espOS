#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
"""
Check the public headers, components/*/include/**/*.{h,hpp}, against the C ABI rules.

Those headers are the contract a binding is generated from (docs/development.md,
"Public API rules"): bindgen reads them as they are and follows every #include,
so an IDF header pulled into a public header makes the binding depend on the
whole of IDF, a CONFIG_ token makes the header mean something different per
sdkconfig, and a header without an extern "C" block cannot be consumed from
C++ at all. None of that fails the firmware build, which is why it is checked
here.

Three checks, reported as path:line: kind: message.

  * includes -- a public header may include the C standard library, other
    espOS public headers and esp_err.h. Every other IDF or system header is an
    error unless ALLOWLIST names it for that header, with the reason; that
    list is frozen, an addition is a decision (docs/decisions.md). A C++
    standard header (<string>, <vector>, ...) in a C header is an error too.
  * CONFIG_ -- a CONFIG_* token in code (comments and string literals are
    stripped first), and the `#include "sdkconfig.h"` that exists only to
    carry them, are warnings: the remediation backlog, not yet fatal. The
    count is printed; --strict turns them into errors.
  * guards -- every header contains `#pragma once` and an `extern "C"` block.

Directories in CPP_ONLY hold C++ interfaces by design (classes, namespaces,
std:: types) and are not part of the C ABI: their headers are exempt from the
include and extern "C" rules (what they include is printed as a note), may
carry a classic #ifndef guard instead of #pragma once, and their CONFIG_
tokens are still counted. Headers named *.hpp are C++ by extension and not
scanned.

Exit status: 1 on any error, 0 with warnings, 2 when no header was found.
Standard library only, so it runs without the IDF environment (CI's headers
job, a pre-commit hook).

  python3 tools/check_public_headers.py            # from anywhere in the tree
  python3 tools/check_public_headers.py --root DIR
  python3 tools/check_public_headers.py --quiet    # errors and warnings only
  python3 tools/check_public_headers.py --strict   # warnings are errors
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Header (relative to the repository root) -> {included name: why it is allowed}.
# Frozen: the rule is "esp_err.h only" and these are the documented exceptions.
# A new entry is a new exception; it needs the decision recorded in
# docs/decisions.md and docs/development.md before it lands.
ALLOWLIST: dict[str, dict[str, str]] = {
    "components/espos_event/include/espos_event.h": {
        "esp_event.h": "the type on offer IS the IDF default event loop: "
                       "base, handler signature and subscribe are esp_event's",
    },
    "components/espos_httpd/include/espos_httpd.h": {
        "esp_http_server.h": "URI handlers are esp_http_server's (httpd_req_t, "
                             "httpd_uri_t); a plain-C route shim is remediation work",
    },
}

# Components whose public headers are C++ interfaces by design; not part of
# the C ABI until they get C wrappers.
CPP_ONLY = {"espos_audio", "espos_n2k", "espos_voice",
            "espos_flow", "espos_formulas", "espos_sensors", "espos_sk_flow",
            "espos_devices", "espos_usb_storage"}

# The one IDF header every public header may include: esp_err_t is the return
# type of the whole API.
IDF_ALLOWED = {"esp_err.h"}

# Exists only to carry CONFIG_ tokens, so it is reported with them (a
# warning), not as an IDF include (an error).
SDKCONFIG = "sdkconfig.h"

# C11, the language the public headers are written in.
C_STD_HEADERS = {
    "assert.h", "complex.h", "ctype.h", "errno.h", "fenv.h", "float.h",
    "inttypes.h", "iso646.h", "limits.h", "locale.h", "math.h", "setjmp.h",
    "signal.h", "stdalign.h", "stdarg.h", "stdatomic.h", "stdbool.h",
    "stddef.h", "stdint.h", "stdio.h", "stdlib.h", "stdnoreturn.h",
    "string.h", "tgmath.h", "threads.h", "time.h", "uchar.h", "wchar.h",
    "wctype.h",
}

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
PRAGMA_ONCE_RE = re.compile(r"^\s*#\s*pragma\s+once\b", re.M)
IFNDEF_GUARD_RE = re.compile(r"^\s*#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b", re.M)
EXTERN_C_RE = re.compile(r'extern\s+"C"')
CONFIG_RE = re.compile(r"\bCONFIG_[A-Z0-9_]+")


def blank_comments(text: str, strings: bool) -> str:
    """Replace comments -- and string/char literal bodies when `strings` --
    with spaces, keeping every newline so line numbers survive. Literals are
    scanned so a `//` inside "http://" is not taken for a comment."""
    out: list[str] = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "/*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("".join("\n" if ch == "\n" else " " for ch in text[i:j]))
            i = j
        elif two == "//":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n and text[j] not in (quote, "\n"):
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)  # past the closing quote (or the newline)
            literal = text[i:j]
            if strings and len(literal) >= 2:
                literal = quote + " " * (len(literal) - 2) + literal[-1]
            out.append(literal)
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def public_headers(root: Path) -> list[Path]:
    # .hpp as well as .h: the C++ components' headers are public API too, and
    # globbing only *.h made every one of them invisible to this check rather
    # than exempt from it -- which is worse, because the summary line then
    # reported a header count that quietly excluded them.
    return sorted(p for p in root.glob("components/*/include/**/*.h*")
                  if p.is_file() and p.suffix in (".h", ".hpp"))


def include_name(header: Path, root: Path) -> str:
    """The path a consumer writes in #include for this header: relative to
    its component's include/ directory."""
    parts = header.relative_to(root).parts  # components/<c>/include/...
    return "/".join(parts[3:])


def classify(name: str, angled: bool, header: Path, public: set[str]) -> str:
    if name in public or (header.parent / name).is_file():
        return "own"
    if name in C_STD_HEADERS:
        return "std"
    if angled and "." not in name:  # <cstdint>, <atomic>, <string>: the C++ standard library
        return "cxx"
    if name in IDF_ALLOWED:
        return "esp_err"
    if name == SDKCONFIG:
        return "sdkconfig"
    return "foreign"


class Report:
    def __init__(self, quiet: bool) -> None:
        self.quiet = quiet
        self.errors = 0
        self.warnings = 0
        self.config_tokens = 0
        self.config_tokens_cpp = 0
        self.sdkconfig_includes = 0
        self.exempt = 0

    def emit(self, path: str, line: int, kind: str, msg: str) -> None:
        if kind == "error":
            self.errors += 1
        elif kind == "warning":
            self.warnings += 1
        elif self.quiet:
            return
        where = f"{path}:{line}" if line else path
        print(f"{where}: {kind}: {msg}")


def check_header(header: Path, root: Path, public: set[str], rep: Report) -> None:
    rel = header.relative_to(root).as_posix()
    component = header.relative_to(root).parts[1]
    cpp_only = component in CPP_ONLY
    allowed = ALLOWLIST.get(rel, {})
    raw = header.read_text(encoding="utf-8")
    code = blank_comments(raw, strings=False)  # includes still readable
    tokens = blank_comments(raw, strings=True)  # CONFIG_ in code only

    foreign: list[str] = []  # IDF/system includes of a C++-only header, for the note
    for lineno, line in enumerate(code.splitlines(), 1):
        m = INCLUDE_RE.match(line)
        if not m:
            continue
        angled, name = m.group(1) == "<", m.group(2)
        kind = classify(name, angled, header, public)
        if kind in ("own", "std", "esp_err") or (cpp_only and kind == "cxx"):
            continue
        if kind == "sdkconfig":
            rep.sdkconfig_includes += 1
            rep.emit(rel, lineno, "warning",
                     f'#include "{SDKCONFIG}" (exists only to carry CONFIG_ tokens; '
                     "replace them with a runtime query or a fixed _MAX)")
            continue
        if cpp_only:
            foreign.append(name)
        elif kind == "cxx":
            rep.emit(rel, lineno, "error",
                     f"C++ standard header <{name}> in a C header")
        elif name in allowed:
            rep.emit(rel, lineno, "note",
                     f"IDF include {name} allowed (frozen exception): {allowed[name]}")
        else:
            rep.emit(rel, lineno, "error",
                     f"IDF/system include {name}: public headers include no IDF "
                     "header but esp_err.h (docs/development.md, Public API rules)")
    if cpp_only:
        rep.exempt += 1
        detail = (f"; {len(foreign)} IDF/system include(s): " + ", ".join(foreign)
                  if foreign else "")
        rep.emit(rel, 0, "note", "C++-only interface (CPP_ONLY), not part of the C ABI: "
                 'include and extern "C" rules not applied' + detail)

    for lineno, line in enumerate(tokens.splitlines(), 1):
        for m in CONFIG_RE.finditer(line):
            rep.config_tokens += 1
            if cpp_only:
                rep.config_tokens_cpp += 1
            rep.emit(rel, lineno, "warning",
                     f"{m.group(0)} in code: Kconfig leaks into the public ABI "
                     "(remediation backlog: replace with a runtime query or a "
                     "fixed _MAX)")

    if not PRAGMA_ONCE_RE.search(code):
        if cpp_only and IFNDEF_GUARD_RE.search(code):
            rep.emit(rel, 0, "note", "classic #ifndef include guard (accepted in a C++-only header)")
        else:
            rep.emit(rel, 0, "error", "missing #pragma once")
    if not cpp_only and not EXTERN_C_RE.search(code):
        rep.emit(rel, 0, "error", 'missing extern "C" block')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.strip().splitlines()[0])
    ap.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="repository root (default: the parent of tools/)",
    )
    ap.add_argument("--quiet", action="store_true", help="print errors and warnings only")
    ap.add_argument("--strict", action="store_true", help="exit 1 on warnings too")
    args = ap.parse_args()
    root = args.root.resolve()

    headers = public_headers(root)
    if not headers:
        print(f"{root}: no headers under components/*/include", file=sys.stderr)
        return 2
    public = {include_name(h, root) for h in headers}

    rep = Report(args.quiet)
    for header in headers:
        check_header(header, root, public, rep)

    summary = (f"{len(headers)} public headers ({rep.exempt} C++-only, exempt): "
               f"{rep.errors} error(s), {rep.warnings} warning(s) -- "
               f"{rep.config_tokens} CONFIG_ token(s) in code "
               f"({rep.config_tokens_cpp} in C++-only headers), "
               f"{rep.sdkconfig_includes} sdkconfig.h include(s)")
    failed = rep.errors or (args.strict and rep.warnings)
    print(("FAIL: " if failed else "ok: ") + summary, file=sys.stderr if failed else sys.stdout)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
