# pycgfx

A writer of the 3DS's CGFX model format (the HOME Menu banner's), by Floogle: https://github.com/skyfloogle/pycgfx
(MIT, `LICENSE`), at commit 6c2e0cd53a5f4c3265b9aa9a1f30806f8291116e.

- `cgfx/`: the library, unchanged (its `swizzler.py`, which needs Pillow, is not imported).
- `convert.py`: its `main.py` (glTF to CGFX) without the command line and the file loading: the `gltflib` and Pillow
  imports are removed, the three accessor/texture readers take plain objects that carry their bytes, and the default
  sampler and material come from the caller. Nothing else is changed.

`tools/banner_model.py` drives it for the builder's animated banner (plain Python: the builder also runs in a
browser), and corrects what it gets wrong for this use (listed there).
