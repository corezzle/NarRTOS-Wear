"""
Pre-build script: ESP32-S3 is Xtensa, not ARM.
Remove ARM Helium / NEON assembly files from LVGL that the Xtensa assembler cannot process.
"""
import os, shutil

Import("env")

def remove_arm_asm(*args, **kwargs):
    libdeps = env.subst("$PROJECT_LIBDEPS_DIR")
    pio_env = env.subst("$PIOENV")

    dirs_to_remove = [
        os.path.join(libdeps, pio_env, "lvgl", "src", "draw", "sw", "blend", "helium"),
        os.path.join(libdeps, pio_env, "lvgl", "src", "draw", "sw", "blend", "neon"),
    ]

    for d in dirs_to_remove:
        if os.path.isdir(d):
            shutil.rmtree(d)
            print(f"[remove_arm_asm] Removed: {d}")

remove_arm_asm()
