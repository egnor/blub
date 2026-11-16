#!/usr/bin/env python3

import argparse
import asyncio
import contextlib
import datetime
import fcntl
import json
import logging
import os
import pathlib
import signal
import ssl
import typing
import urllib.parse

import aiomqtt
import foxglove.mcap
import ok_logging_setup

urllib.parse.uses_relative.extend(["mqtt", "mqtts"])
urllib.parse.uses_netloc.extend(["mqtt", "mqtts"])


async def main() -> None:
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
    if not args.prefix.name:
        ok_logging_setup.exit("--prefix must include a filename")

    async def log_task(writer: LogWriter):
        async for message in mqtt_message_iter(args.broker, args.topics):
            log_writer.log_message(message.topic, message.payload)

    async def flush_task(writer: LogWriter):
        while True:
            await asyncio.sleep(10)
            writer.flush()

    with LogWriter(args.prefix) as log_writer:
        async with asyncio.TaskGroup() as tasks:
            tasks.create_task(log_task(log_writer))
            tasks.create_task(flush_task(log_writer))


class LogWriter(contextlib.AbstractContextManager):
    def __init__(self, prefix: pathlib.Path):
        self.prefix = prefix
        self.channels: dict[str, foxglove.Channel] = {}
        self.open_file: typing.BinaryIO | None = None
        self.open_writer: foxglove.mcap.MCAPWriter | None = None
        self.open_path: pathlib.Path | None = None
        self.open_date: datetime.date | None = None

    def __exit__(self, exc_type, exc_value, traceback):
        self.ensure_closed()

    def log_message(self, topic: str, payload: bytes):
        log_time = datetime.datetime.now(tz=datetime.UTC)
        try:
            json.loads(payload)
        except ValueError:
            logging.warning("Bad JSON (%s), skipping: %s", topic, payload)
            return

        self.ensure_open(log_time.date())
        if not (channel := self.channels.get(topic)):
            channel = foxglove.Channel(topic, message_encoding="json")
            self.channels[topic] = channel

        channel.log(payload, log_time=int(log_time.timestamp() * 1e9))

    def flush(self):
        if self.open_writer:
            self.open_writer.flush()

    def ensure_open(self, date: datetime.date):
        if self.open_date != date:
            self.ensure_closed()
        if not self.open_writer:
            dir = self.prefix.parent
            date_name = self.prefix.name + date.strftime(".%Y-%m-%d")
            for count in range(1000):
                count_name = date_name + (f".{count:03d}" if count else "")
                if (dir / (count_name + ".mcap")).exists():
                    continue
                try:
                    self.open_path = dir / (count_name + ".mcap.active")
                    self.open_path.parent.mkdir(parents=True, exist_ok=True)
                    self.open_file = open(self.open_path, "xb")  # noqa: SIM115
                    fcntl.flock(self.open_file, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except FileExistsError:
                    continue
                logging.info("📝 Writing: %s", self.open_path)
                self.open_writer = foxglove.open_mcap(self.open_file)
                self.open_date = date
                return
            raise FileExistsError(f"Too many logs: {dir / date_name}*")

    def ensure_closed(self):
        if self.open_writer:
            self.open_writer.close()
            self.open_writer = None
        if self.open_file:
            self.open_file.close()
            self.open_file = None
        if self.open_path and self.open_path.exists():
            final_path = self.open_path.with_suffix("")
            self.open_path.rename(final_path)
            logging.info("✅ Finished: %s", final_path)
        self.open_path = None
        self.open_date = None


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


async def mqtt_message_iter(
    broker: urllib.parse.SplitResult, topics: list[str]
) -> typing.AsyncIterator[aiomqtt.PublishPacket]:
    client_id = f"mqtt_mcap_logger.{os.getpid()}@{os.uname().nodename}"
    sslc = ssl.create_default_context() if broker.scheme.endswith("s") else None
    while True:
        try:
            logging.info("📡 Connecting: %s", broker.geturl())
            async with aiomqtt.Client(
                hostname=broker.hostname,
                port=broker.port or 1883,
                ssl_context=sslc,
                identifier=client_id,
                logger=logging.getLogger("aiomqtt"),
                username=broker.username,
                password=broker.password.encode() if broker.password else None,
            ) as client:
                for t in topics:
                    logging.info("🗨️ Subscribing: %s", t)
                    await client.subscribe(t, max_qos=aiomqtt.QoS.AT_MOST_ONCE)
                async for message in client.messages():
                    if isinstance(message, aiomqtt.PublishPacket):
                        yield message

        except (
            OSError,
            aiomqtt.ConnectError,
            aiomqtt.ProtocolError,
            aiomqtt.NegativeAckError,
        ) as e:
            logging.error("MQTT error (reconnecting): %s", str(e))
            await asyncio.sleep(1)


def on_sigint(*args):
    raise KeyboardInterrupt()


def on_sigterm(*args):
    raise SystemExit()


if __name__ == "__main__":
    ok_logging_setup.install()
    signal.signal(signal.SIGINT, on_sigint)
    signal.signal(signal.SIGTERM, on_sigterm)
    asyncio.run(main())
