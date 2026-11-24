import os
import sys

import pytest

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "python"))


@pytest.fixture(scope="session")
def root():
    return ROOT


@pytest.fixture(scope="session")
def hftbt_bin():
    path = os.environ.get("HFTBT_BIN", os.path.join(ROOT, "build", "hftbt"))
    if not os.path.exists(path):
        pytest.skip("hftbt binary not built (%s); set HFTBT_BIN" % path)
    return path
