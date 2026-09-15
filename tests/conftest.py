import pytest
from asyncio import create_subprocess_exec, TaskGroup, wait_for
from asyncio.subprocess import PIPE
from pathlib import Path

EMULATOR_PATH = Path(__file__).parent / "emulator" / "emulate_rp2040.js"


@pytest.fixture
def run_emulator(request):
    """Returns an async function which builds the sketch in the test module's
    directory, runs it under the RP2040 emulator until #END-TESTS# (or
    `timeout` wall seconds), fails on any #TEST-FAIL#, and returns lines
    printed to `Serial1` (aka uart0)."""

    sketch_dir = Path(request.path).parent
    module_name = request.module.__name__.split(".")[-1]
    build_dir = sketch_dir / f"{module_name}.tmp"

    async def run(timeout=30.0) -> list[str]:
        print("\n🏗️ Building:", sketch_dir.name)
        compile = await create_subprocess_exec(
            "arduino-cli",
            "compile",
            f"--build-path={build_dir}/work",
            f"--output-dir={build_dir}",
            cwd=str(sketch_dir),
        )
        assert (await compile.wait()) == 0, "arduino-cli compile failed"

        (uf2,) = build_dir.glob("*.uf2")
        print("\n▶️ Emulating:", uf2.name)
        emu = await create_subprocess_exec(EMULATOR_PATH, str(uf2), stdout=PIPE)
        started, ended = False, False
        try:
            lines: list[str] = []
            failures: list[str] = []
            async with TaskGroup() as tasks:
                tasks.create_task(wait_for(emu.wait(), timeout=timeout))
                with open(build_dir / "output.txt", "wb", buffering=0) as log:
                    async for line in emu.stdout:
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
                            emu.terminate()
        finally:
            (emu.returncode is None) and emu.kill()
            await emu.wait()
            print("⏹️ Emulator stopped")

        assert not failures, f"Tests failed\n  {'\n  '.join(failures)}"
        return lines

    return run
