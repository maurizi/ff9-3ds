#!/usr/bin/env python3
"""Generate synthetic interpolation goldens from a pinned local Beetle PSX source.

The reference table/expression stay in ignored build/. See audio-validation.md.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile

SOURCE_SHA256 = "f081394befd298e1d3e70a3982009ac9555853c4dfa92162fb7bbed7d3370a99"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    args = parser.parse_args()
    raw = args.reference.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SOURCE_SHA256:
        parser.error("reference source does not match pinned SHA256")
    source = raw.decode()
    start = source.index("static const int16_t FIR_Table[256][4] =")
    table = source[start:source.index("};", start) + 2]
    start = source.index("voice_pvs = ((voice->DecodeBuffer[")
    expression = source[start:source.index(";", start) + 1]
    harness = '#include <stdint.h>\n#include <stdio.h>\n#include "interpolation_cases.h"\n'
    harness += table
    harness += r'''
int main(void) {
    puts("/* Synthetic interpolation hashes from pinned Beetle PSX, not hardware.");
    puts(" * Regenerate with tools/spu_interpolation_reference.py; see audio-validation.md. */");
    puts("static const uint32_t interpolation_reference_hashes[256] = {");
    for (unsigned phase=0; phase<256; ++phase) {
        uint32_t hash=2166136261u;
        for (unsigned scenario=0; scenario<32; ++scenario) {
            struct { int16_t DecodeBuffer[32]; } storage={0}, *voice=&storage;
            interpolation_case(scenario,voice->DecodeBuffer);
            int si=0,pi=phase;
            int32_t voice_pvs;
'''
    harness += expression
    harness += r'''
            hash=interpolation_hash(hash,(int16_t)voice_pvs);
        }
        printf("0x%08xu,%s",hash,(phase%8==7)?"\n":" ");
    }
    puts("};");
}
'''
    root = Path(__file__).resolve().parents[1]
    directory = root / "build" / "validation"
    directory.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spu-interpolation-reference-", dir=directory) as tmp:
        c = Path(tmp) / "reference.c"
        exe = Path(tmp) / "reference"
        c.write_text(harness)
        subprocess.run(["cc", "-std=gnu17", "-O2", "-I", str(root / "tests/spu"),
                        str(c), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
