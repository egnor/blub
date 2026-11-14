#!/usr/bin/env python3
"""
Relays messages from embedded serial port output to Foxglove Studio.
"""

import argparse
import contextlib
import foxglove
import foxglove.channels
import foxglove.mcap
import foxglove.messages
import json
import logging
import msgspec
import ok_logging_setup
import ok_serial
import os
import pathlib
import re
import signal
import time


MAX_LINE_LENGTH = 65536

# $ topic | json
JSON_LINE_RX = re.compile(rb"\$(/[\w/]*)\|(\".*\"|{.*}|\[.*\]|[\w.-]+)")

# millis emoji ...
DEBUG_LINE_RX = re.compile(r"[\d.]+ 🕸️ ".encode())
ERROR_LINE_RX = re.compile(r"[\d.]+ ⚠️ ".encode())
FATAL_LINE_RX = re.compile(r"[\d.]+ 💥 ".encode())


class RelayStatus(msgspec.Struct):
    """Published status of serial connection"""

    host: str
    pid: int
    match: str = ""
    baud: int = 0
    connection_port: str | None = None
    connection_age: float | None = None
    disconnect_count: int = 0
    byte_count: int = 0
    line_count: int = 0
    message_count: dict[str, int] = {}


relay_status = RelayStatus(host=os.uname().nodename, pid=os.getpid())

relay_status_channel = foxglove.Channel(
    "/relay_status",
    message_encoding="json",
    schema=foxglove.Schema(
        name="RelayStatus",
        encoding="jsonschema",
        data=msgspec.json.encode(msgspec.json.schema(RelayStatus)),
    ),
)

relay_log_channel = foxglove.channels.LogChannel("/relay_log")

message_channels: dict[str, foxglove.Channel] = {}


def handle_line(
    fox_server: foxglove.WebSocketServer,
    line_bytes: bytes,
):
    relay_status.line_count += 1
    log_ts = foxglove.messages.Timestamp.from_epoch_secs(time.time())
    log_level: foxglove.messages.LogLevel | None = None

    line_bytes = line_bytes.strip()
    if match := JSON_LINE_RX.fullmatch(line_bytes):
        topic_bytes, json_bytes = match.groups()
        topic = topic_bytes.decode(errors="replace")
        try:
            json.loads(json_bytes)
        except ValueError as e:
            log_level = foxglove.messages.LogLevel.Error
            fox_server.publish_status(
                f"{e}: {line_bytes.decode(errors='replace')}",
                foxglove.websocket.StatusLevel.Error,
                id=f"message:{topic}",
            )

        counts = relay_status.message_count
        counts[topic] = counts.get(topic, 0) + 1
        if not (channel := message_channels.get(topic)):
            channel = foxglove.Channel(topic, message_encoding="json")
            message_channels[topic] = channel
        channel.log(json_bytes)
    elif FATAL_LINE_RX.match(line_bytes):
        log_level = foxglove.messages.LogLevel.Fatal
    elif ERROR_LINE_RX.match(line_bytes):
        log_level = foxglove.messages.LogLevel.Error
    elif DEBUG_LINE_RX.match(line_bytes):
        log_level = foxglove.messages.LogLevel.Debug
    else:
        log_level = foxglove.messages.LogLevel.Info

    if log_level is not None:
        t = line_bytes.decode(errors="replace")
        m = foxglove.messages.Log(timestamp=log_ts, level=log_level, message=t)
        relay_log_channel.log(m)


def run_connection(
    conn: ok_serial.SerialConnection,
    fox_server: foxglove.WebSocketServer,
    fox_writer: foxglove.mcap.MCAPWriter | None,
):
    connect_mono = time.monotonic()
    status_pub_mono = 0
    writer_flush_mono = 0
    relay_status.connection_port = conn.port_name
    relay_status.connection_age = 0.0
    logging.info("↔️ Relaying: %s @%d", conn.port_name, relay_status.baud)

    in_buffer = bytearray()
    while True:
        mono = time.monotonic()
        if mono >= status_pub_mono:
            status_pub_mono = mono + 0.5
            relay_status.connection_age = mono - connect_mono
            relay_status_channel.log(msgspec.json.encode(relay_status))

        if fox_writer and mono >= writer_flush_mono:
            writer_flush_mono = mono + 5.0
            fox_writer.flush()

        in_read = conn.read_sync(timeout=0.5)
        relay_status.byte_count += len(in_read)
        while (newline_pos := in_read.find(b"\n")) >= 0:
            handle_line(fox_server, in_buffer + in_read[:newline_pos])
            in_buffer, in_read = bytearray(), in_read[newline_pos + 1 :]
        in_buffer += in_read
        del in_buffer[:-MAX_LINE_LENGTH]


def run_relay(
    *,
    match: str,
    opts: ok_serial.SerialConnectionOptions,
    fox_server: foxglove.WebSocketServer,
    fox_writer: foxglove.mcap.MCAPWriter | None,
):
    relay_status.match = match

    relay_status_channel.log(msgspec.json.encode(relay_status))
    fox_server.publish_status(
        f"Searching for serial: [{match}]",
        foxglove.websocket.StatusLevel.Info,
        id="search",
    )

    serial_mon = ok_serial.SerialConnectionMonitor(match, copts=opts)
    while True:
        while not (conn := serial_mon.connect_sync(timeout=0.5)):
            relay_status_channel.log(msgspec.json.encode(relay_status))
            fox_server.publish_status(
                f"Serial not found: [{match}] (retrying)",
                foxglove.websocket.StatusLevel.Warning,
                id="search",
            )

        fox_server.remove_status(["search", "error"])
        try:
            run_connection(conn, fox_server, fox_writer)
        except ok_serial.SerialException as e:
            relay_status.connection_port = None
            relay_status.connection_age = None
            relay_status.disconnect_count += 1
            logging.error("Serial error: %s (retrying)", e)
            relay_status_channel.log(msgspec.json.encode(relay_status))
            fox_server.publish_status(
                f"Serial error: {e} (retrying)",
                foxglove.websocket.StatusLevel.Error,
                id="error",
            )
            fox_writer.flush() if fox_writer else None


def main():
    ok_logging_setup.install()
    ok_logging_setup.skip_traceback_for(KeyboardInterrupt)
    signal.signal(signal.SIGINT, signal.default_int_handler)

    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial", help="Serial port match string")
    parser.add_argument("--baud", type=int, default=115200, help="Serial bps")
    parser.add_argument("--log", type=pathlib.Path, help="MCAP file to write")
    parser.add_argument("--port", type=int, default=8765, help="Websocket port")
    parser.add_argument(
        "--public", action="store_true", help="Listen on all interfaces"
    )
    args = parser.parse_args()

    with contextlib.ExitStack() as exits:
        listen_host = "" if args.public else "localhost"
        fox_server = foxglove.start_server(host=listen_host, port=args.port)
        foxglove.start_sysinfo_publisher()
        logging.info("🦊 Foxglove: %s", fox_server.app_url())

        fox_writer: foxglove.mcap.MCAPWriter | None = None
        if args.log:
            logging.info("📝 Logging: %s", args.log)
            fox_writer = exits.enter_context(foxglove.open_mcap(args.log))

        SerOpt = ok_serial.SerialConnectionOptions
        run_relay(
            match=args.serial,
            opts=SerOpt(baud=args.baud, sharing="polite"),
            fox_server=fox_server,
            fox_writer=fox_writer,
        )


if __name__ == "__main__":
    main()
