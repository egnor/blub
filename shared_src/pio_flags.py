# Add to extra_scripts in platformio.ini files:
# extra_scripts = pre:../shared_code/pio_flags.py

Import("env")

# Suppress a C++-only warning that happens in external library code
env.Append(CXXFLAGS=["-Wno-volatile"])
