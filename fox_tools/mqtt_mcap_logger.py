#!/usr/bin/env python3

import argparse
import asyncio
import contextlib
import datetime
import foxglove.mcap
import logging
import os
import pathlib
import ssl
import timezone
import urllib.parse

import aiomqtt
import ok_logging_setup

urllib.parse.uses_relative.extend(["mqtt", "mqtts"])
urllib.parse.uses_netloc.extend(["mqtt", "mqtts"])


async def main() -> None:
    ok_logging_setup.install()
    parser = argparse.ArgumentParser(description="MQTT to MCAP logger")
    parser.add_argument(
        "--broker", type=parse_mqtt_url, required=True, help="MQTT URL"
    )
    parser.add_argument(
        "--prefix", type=pathlib.Path, required=True, help="Log file prefix"
    )
    parser.add_argument(
        "--topics", nargs="+", default=["#"], help="MQTT topics to subscribe"
    )
    args = parser.parse_args()

    with LogWriter(args.prefix) as log_writer:
        try:
            async with make_client(args.broker) as client:
                for t in args.topics:
                    await client.subscribe(t, max_qos=aiomqtt.QoS.AT_MOST_ONCE)
                async for message in client.messages():
                    print(message.topic, message.payload)

        except (
            OSError,
            aiomqtt.ConnectError,
            aiomqtt.ProtocolError,
            aiomqtt.NegativeAckError,
        ) as e:
            logging.error("Error (reconnecting): %s", str(e))
            await asyncio.sleep(1)


class LogWriter(contextlib.AbstractContextManager):
    def __init__(self, prefix: pathlib.Path):
        self.prefix = prefix
        self.context = foxglove.Context()
        self.mcap: foxglove.mcap.MCAPWriter | None = None
        self.mcap_path: pathlib.Path | None = None
        self.mcap_date: datetime.date | None = None

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def log_message(self, topic: str, payload: bytes):
        logtime = datetime.datetime.now(tz=timezone.utc)
        if self.mcap_date != logtime.date():
            self.close()
        if not self.mcap:
            date_name = self.prefix.name + logtime.strftime(".%Y-%m-%d")
            # TODO: look for existing .mcap* files, increment until clear
            # TODO: open MCAP file for writing with .mcap.active

    def close(self):
        if self.mcap:
            self.mcap.close()
            self.mcap = None
            # TODO: rename .mcap.active to .mcap


def make_client(url):
    use_tls = url.scheme.endswith("s")
    return aiomqtt.Client(
        hostname=url.hostname,
        port=url.port,
        ssl_context=ssl.create_default_context() if use_tls else None,
        identifier=f"mqtt_mcap_logger.{os.getpid()}@{os.uname().nodename}",
        logger=logging.getLogger("aiomqtt"),
        username=url.username,
        password=url.password.encode(),
    )


def parse_mqtt_url(arg: str) -> urllib.parse.SplitResult:
    arg = arg if "//" in arg else f"mqtt://{arg}"
    try:
        url = urllib.parse.urlsplit(arg, scheme="mqtt")
    except ValueError as e:
        raise argparse.ArgumentTypeError(str(e))

    scheme_port = {"mqtt": 1883, "mqtts": 8883}.get(url.scheme)
    if not scheme_port:
        raise argparse.ArgumentTypeError(f"Bad MQTT URL scheme: {url.scheme}")
    if not url.hostname:
        raise argparse.ArgumentTypeError("Missing MQTT broker hostname")
    if not url.port:
        url = url._replace(netloc=f"{url.netloc.rstrip(':')}:{scheme_port}")
    return url


if __name__ == "__main__":
    asyncio.run(main())
