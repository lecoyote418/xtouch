import subprocess
import os

Import("env")
env = DefaultEnvironment()


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
    print("XTOUCH PREBUILD")

    # TFT_eSPI (2.8" board only) has no path-override mechanism like LVGL's
    # LV_CONF_PATH, so its pin/driver config has to be physically copied into
    # its fetched libdeps folder. Always overwrite (not just if missing) —
    # TFT_eSPI ships its own default User_Setup.h, so an "only if missing"
    # copy silently keeps the wrong pin config for this board and produces a
    # blank white screen at boot.
    # Uses the actual current environment name (env["PIOENV"]) rather than a
    # hardcoded one, so this keeps working across esp32-2432s028r,
    # esp32-2432s028r-ds18b20, etc. It's a no-op on the 5" board, which uses
    # LovyanGFX instead of TFT_eSPI.
    libdeps_dir = os.path.join(".pio", "libdeps", env["PIOENV"])
    tft_espi_dir = os.path.join(libdeps_dir, "TFT_eSPI")
    if os.path.isdir(tft_espi_dir):
        copy_config("resources/User_Setup.h", os.path.join(tft_espi_dir, "User_Setup.h"))

    result = subprocess.run(
        ['node', 'scripts/download-errors.js'],
        text=True,
        check=True,
        capture_output=True
    )
    print(result.stdout)