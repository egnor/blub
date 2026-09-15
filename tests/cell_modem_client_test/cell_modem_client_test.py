async def test_cell_modem_client(run_emulator):
    # Waits out real reconnect backoffs: ~200s emulated, ~40s wall
    await run_emulator(timeout=120)
