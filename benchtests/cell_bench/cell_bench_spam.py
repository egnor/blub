#!/usr/bin/env python3
import aiomqtt
import asyncio
import logging
import ok_logging_setup
import ssl


async def run_spam():
    logging.info("Connecting to MQTT...")
    async with aiomqtt.Client(
        hostname="mqtt.eacs.io",
        port=8883,
        ssl_context=ssl.create_default_context(),
        identifier="cell_bench_spam",
        username="blub",
        password=b"blub",
        clean_start=True,
        reconnect=True,
    ) as client:
        counter = 0
        while True:
            logging.info("Publishing spam #%d", counter)
            await client.publish("cell_bench/sub", f"Spam #{counter}".encode())
            counter += 1
            await asyncio.sleep(1)


ok_logging_setup.install()
asyncio.run(run_spam())
