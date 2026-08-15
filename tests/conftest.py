import asyncio
import pytest
import pytest_asyncio
import shutil
from asyncio.subprocess import PIPE
from pathlib import Path

EMULATOR_PATH = Path(__file__).parent / "emulator" / "emulate_rp2040.js"


@pytest_asyncio.fixture(scope="module")
async def emulated_output_lines(request, timeout=30.0) -> list[str]:
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
    try:
        lines: list[str] = []
        async with asyncio.TaskGroup() as tasks:
            tasks.create_task(asyncio.wait_for(proc.wait(), timeout=timeout))
            with open(output_dir / "serial_log.txt", "wb", buffering=0) as log:
                async for line in proc.stdout:
                    lines.append(line)
                    log.write(line)
                    print(line.decode().rstrip())
                    assert not line.startswith(b"ABORT-TEST"), line
                    if line.startswith(b"END-TEST"):
                        print("🏁 Test done, terminating emulator")
                        proc.terminate()
    finally:
        (proc.returncode is None) and proc.kill()
        await proc.wait()
        print("⏹️ Emulator stopped")

    return [t.decode().rstrip() for t in lines]


@pytest.fixture(scope="function")
def check_emulated_output(emulated_output_lines, subtests):
    lines = emulated_output_lines
    assert "BEGIN-TEST" in lines
    assert "END-TEST" in lines
    begin_i, end_i = lines.index("BEGIN-TEST"), lines.index("END-TEST")
    for i in range(begin_i, end_i):
        if failure := lines[i].split("VERIFY-FAIL:", 1)[1:]:
            with subtests.test(failure[0].strip()):
                context = "\n  ".join(lines[i : i + 5])
                assert "VERIFY-FAIL" not in lines[i], context
