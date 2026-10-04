"""The standalone build must consume only the explicit public contract."""
import re
from pathlib import Path

root = Path(__file__).resolve().parents[1]
assert not (root / "driver").exists()
sdk = root / "dependencies/AICAflow"
public = sdk / "driver/format/include"
assert (sdk / "driver/tools/afx_validate.c").is_file()
assert not (root / "dependencies/aicaflow-format/include/aicaflow/format.h").exists()
# Initialization must stop at the SDK: no recursive example/tool dependencies.
assert not (sdk / "dependencies/enDjinn/.git").exists()
assert not (sdk / "dependencies/AICAforge/.git").exists()
for source in (root / "src").iterdir():
    if source.suffix not in (".c", ".h"):
        continue
    for include in re.findall(r'^#include [<"]([^>"]+)[>"]', source.read_text(), re.MULTILINE):
        assert ".." not in include and "driver/" not in include, (source, include)
        if include.startswith("aicaflow/"):
            assert (public / include).is_file(), (source, include)
assert "#define AFX_FORMAT_API_VERSION 1" in (public / "aicaflow/format.h").read_text()
print("Standalone authoring dependency boundary checks passed")
