import asyncio
import asyncio.subprocess
import re
import shutil
from pathlib import Path

import pytest

EMULATOR_PATH = Path(__file__).parent / "emulator" / "rp2040_emulate.js"


@pytest.fixture(scope="module")
def emulated_output(request, timeout=30.0) -> str:
    """Builds the sketch in the test module's directory, runs it under the
    RP2040 simulator, and returns lines printed to `Serial1` (aka uart0)."""

    sketch_dir = Path(request.path).parent
    output_dir = sketch_dir / "output.tmp"
    if output_dir.is_dir():
        shutil.rmtree(output_dir)

    compile_args = ["arduino-cli", "compile", "--output-dir=output.tmp"]
    subprocess.run(compile_args, check=True, cwd=str(sketch_dir))

    (uf2,) = output_dir.glob("*.uf2")
    emulate_args = [
        *("node", str(EMULATOR_PATH), str(uf2)),
        "--expect=END-TEST",  # TODO: remove these
        "--timeout=30",
    ]

    emulator = subprocess.Popen(emulate_args, stdout=subprocess.PIPE, text=True)
    try:
        out, _err = emulator.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        emulator.kill()
        out, _err = emulator.communicate()

    serial_text = sub.stdout_text()
    (output_dir / "serial_log.txt").write_text(serial_text)
    return serial_text


@pytest.fixture(scope="module")
def device_checks(emulated_output) -> dict:
    """Parses the firmware's END-TEST summary line."""
    match = re.search(
        r"^END-TEST checks=(\d+) failures=(\d+)$", emulated_output, re.M
    )
    assert match, f"no END-TEST summary in output:\n{emulated_output}"
    return {"checks": int(match[1]), "failures": int(match[2])}


@pytest.fixture(scope="module")
def device_info(emulated_output) -> dict[str, str]:
    """Collects `INFO key=value` lines the firmware reported."""
    return dict(re.findall(r"^INFO (\w+)=(.*)$", emulated_output, re.M))
