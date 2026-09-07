import subprocess

Import("env")
env = DefaultEnvironment()


def is_pio_build():
    from SCons.Script import DefaultEnvironment
    env = DefaultEnvironment()
    if "IsCleanTarget" in dir(env) and env.IsCleanTarget():
        return False
    return not env.IsIntegrationDump()


if is_pio_build() == True:
    print("XTOUCH PREBUILD")
    result = subprocess.run(
        ['node', 'scripts/download-errors.js'],
        text=True,
        check=True,
        capture_output=True
    )
    print(result.stdout)
