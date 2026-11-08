#!/usr/bin/env python3
"""Regenerate src/ca_roots.c from include/ca_roots.pem."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent
pem = (root / "include/ca_roots.pem").read_text()
body = "".join('"' + line.replace("\\", "\\\\").replace('"', '\\"') + '\\n"\n' for line in pem.splitlines())
src = (root / "src/ca_roots.c").read_text()
head = src[: src.index("const char CA_ROOTS_PEM[] =")]
(root / "src/ca_roots.c").write_text(head + "const char CA_ROOTS_PEM[] =\n" + body + ";\nconst size_t CA_ROOTS_PEM_LEN = sizeof CA_ROOTS_PEM;   // includes the NUL Mbed TLS wants\n")
