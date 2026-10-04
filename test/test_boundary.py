"""The standalone build must consume only the explicit public contract."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
assert not (root / "driver").exists()
public = root / "dependencies/aicaflow-format/include"
for source in (root / "src").iterdir():
    if source.suffix not in (".c", ".h"):
        continue
    for include in re.findall(r'^#include [<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
        assert ".." not in include and "driver/" not in include, (source, include)
        if include.startswith("aicaflow/"):
            assert (public / include).is_file(), (source, include)
assert "format-api=1" in (public.parent / "VERSION").read_text()
print("Standalone authoring dependency boundary checks passed")
