"""Stand-ins for splat's N64-only dependencies (pygfxd, crunch64, n64img: native code the PS1 split never runs).
splat imports them when it loads its segment types; any attribute or submodule of them is a dummy."""
import importlib.abc
import importlib.machinery
import sys
import types


class _Dummy:
    def __init__(self, *a, **k):
        pass

    def __call__(self, *a, **k):
        return _Dummy()

    def __getattr__(self, name):
        return _Dummy()


class _StubModule(types.ModuleType):
    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return _Dummy


class _Finder(importlib.abc.MetaPathFinder, importlib.abc.Loader):
    ROOTS = ("pygfxd", "crunch64", "n64img")

    def find_spec(self, name, path=None, target=None):
        if name.split(".")[0] in self.ROOTS:
            return importlib.machinery.ModuleSpec(name, self, is_package=True)
        return None

    def create_module(self, spec):
        m = _StubModule(spec.name)
        m.__path__ = []
        return m

    def exec_module(self, module):
        pass


sys.meta_path.insert(0, _Finder())
