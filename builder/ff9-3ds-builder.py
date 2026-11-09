#!/usr/bin/env python3
"""Entry point (python3 builder/ff9-3ds-builder.py, and PyInstaller's script)."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ff9builder.__main__ import main  # noqa: E402

main()
