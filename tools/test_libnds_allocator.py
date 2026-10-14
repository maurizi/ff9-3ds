#!/usr/bin/env python3
"""Test the actual vendored libnds block allocator on the host, without DS I/O."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
source = (root / 'third_party/libnds/videoGL.c').read_text()
structs = source[source.index('typedef struct s_SingleBlock'):source.index('typedef struct gl_texture_data')]
functions = source[source.index('struct s_SingleBlock *vramBlock__allocateBlock('):
                   source.index('uint8_t *vramBlock_examineSpecial(')]
out = root / 'build/tests/libnds_allocator'
out.mkdir(parents=True, exist_ok=True)
header = out / 'allocator_under_test.h'
header.write_text('#include <stdint.h>\n#include <stdlib.h>\n'
                  'typedef struct { void **data; unsigned cur_size; } DynamicArray;\n'
                  + structs + functions)
exe = out / 'probe'
subprocess.run(['gcc', '-m32', '-std=gnu17', '-O1', '-g', '-fsanitize=address,undefined',
                '-I' + str(out), str(root / 'tests/gx/allocator_probe.c'), '-o', str(exe)], check=True)
subprocess.run([str(exe)], check=True)
