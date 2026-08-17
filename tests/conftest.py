import asyncio
import pytest_asyncio
import shutil
from asyncio.subprocess import PIPE
from pathlib import Path

EMULATOR_PATH = Path(__file__).parent / "emulator" / "emulate_rp2040.js"


@pytest_asyncio.fixture(scope="module")
async def emulated_test_output(request, timeout=30.0) -> list[str]:
    """Builds the sketch in the test module's directory, runs it under the
    RP2040 simulator, and returns lines printed to `Serial1` (aka uart0)."""

    sketch_dir = Path(request.path).parent
    if (output_dir := sketch_dir / "output.tmp").is_dir():
        shutil.rmtree(output_dir)

    print("\n🏗️ Building:", sketch_dir.name)
    args = ["arduino-cli", "compile", "--output-dir=output.tmp"]
    proc = await asyncio.create_subprocess_exec(*args, cwd=str(sketch_dir))
    assert (await proc.wait()) == 0, "arduino-cli compile failed"

    (uf2,) = output_dir.glob("*.uf2")
    print("\n▶️ Emulating:", uf2.name)
    args = (EMULATOR_PATH, str(uf2))
    proc = await asyncio.create_subprocess_exec(*args, stdout=PIPE)
    started, ended = False, False
    try:
        lines: list[str] = []
        failures: list[str] = []
        async with asyncio.TaskGroup() as tasks:
            tasks.create_task(asyncio.wait_for(proc.wait(), timeout=timeout))
            with open(output_dir / "serial_log.txt", "wb", buffering=0) as log:
                async for line in proc.stdout:
                    log.write(line)
                    print(text := line.decode().rstrip())
                    assert "#ABORT-TESTS#" not in text, text
                    if "#BEGIN-TESTS#" in text:
                        assert not started, "Extra #BEGIN-TESTS#: {text}"
                        started = True
                    if started and not ended:
                        lines.append(text)
                    if "#TEST-FAIL#" in text:
                        failures.append(text)
                    if "#END-TESTS#" in text:
                        assert started, f"No #BEGIN-TESTS#: {text}"
                        assert not ended, f"Extra #END-TESTS#: {text}"
                        ended = True
                        print("🏁 Test done, stopping emulator")
                        proc.terminate()
    finally:
        (proc.returncode is None) and proc.kill()
        await proc.wait()
        print("⏹️ Emulator stopped")

    assert not failures, f"Test failed:\n  {'\n  '.join(failures)}"
    return lines
