
from pathlib import Path
import json


# PlatformIO passes the SCons environment as the "env" variable.
Import("env")

project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
version_file = project_dir / "version.json"

with version_file.open("r", encoding="utf-8") as f:
    data = json.load(f)

version = data["version"]

env.Append(
    CPPDEFINES=[
        ("XTOUCH_FIRMWARE_VERSION", f'\\"{version}\\"')
    ]
)
