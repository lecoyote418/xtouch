import json
import subprocess
import os
import hashlib

Import("env")

env = DefaultEnvironment()


# Make sure 'vscode init' is not the current command
def is_pio_build():
    from SCons.Script import DefaultEnvironment
    env = DefaultEnvironment()
    if "IsCleanTarget" in dir(env) and env.IsCleanTarget():
        return False
    return not env.IsIntegrationDump()


def copy_config(source, target):
    if os.path.exists(source):
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with open(source, 'rb') as src_file, open(target, 'wb') as dst_file:
            dst_file.write(src_file.read())
        print(f"Copied {source} -> {target}")


if is_pio_build() == True:
    print(f"XTOUCH PREBUILD")

    # lvgl and TFT_eSPI need their config headers placed inside the fetched
    # libdeps folders; .pio/ is gitignored so this can't be committed.
    # Always overwrite (not just if missing) — TFT_eSPI ships its own default
    # User_Setup.h, so an "only if missing" copy silently keeps the wrong
    # pin config for this board and produces a blank white screen at boot.
    copy_config("resources/lv_conf.h", ".pio/libdeps/esp32dev/lv_conf.h")
    copy_config("resources/User_Setup.h", ".pio/libdeps/esp32dev/TFT_eSPI/User_Setup.h")

    result = subprocess.run(['node', 'scripts/download-errors.js'],
                            text=True, check=True, capture_output=True)
    print(result.stdout)
