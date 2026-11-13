"""Keep the NativeWeb asset namespace while sharing the runtime-test rule."""

load("//bzl/valdi:test_assets.bzl", _runtime_test_assets = "runtime_test_assets")

def runtime_test_assets(name, **kwargs):
    _runtime_test_assets(name = name, namespace = "NativeWebTestAssets", **kwargs)
