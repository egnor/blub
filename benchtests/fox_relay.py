#!/usr/bin/env python3
"""
Relays messages from embedded serial port output to Foxglove Studio.
"""

import argparse
import foxglove
import logging
import msgspec
import ok_logging_setup
import ok_serial
import os
import time


class SerialInfo(msgspec.Struct):
    """Published status of serial connection"""

    host: str
    pid: int
    match: str
    baud: int
    connect_port: str | None = None
    connect_time: float | None = None
    # TODO: bytes rx/rx, reconnections count, etc


def run_relay(
    *,
    serial_match: str,
    serial_opts: ok_serial.SerialConnectionOptions,
    fox_server: foxglove.WebSocketServer,
):
    serial_info = SerialInfo(
        host=os.uname().nodename,
        pid=os.getpid(),
        match=serial_match,
        baud=serial_opts.baud,
    )
    serial_mon = ok_serial.SerialConnectionMonitor(
        serial_match=serial_match, copts=serial_opts
    )
    while True:
        time.sleep(1.0)
        fox_server.publish_status(
            "Test Info",
            foxglove.websocket.StatusLevel.Info,
        )
        fox_server.publish_status(
            "Toast Info",
            foxglove.websocket.StatusLevel.Info,
        )


def main():
    ok_logging_setup.install()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("serial", help="Serial port match string")
    parser.add_argument(
        "--baud", type=int, default=115200, help="Serial port baud rate"
    )
    parser.add_argument(
        "--public",
        action=argparse.BooleanOptionalAction,
        help="Take connections from anywhere (not just localhost)",
    )
    parser.add_argument(
        "--port", type=int, default=8765, help="Foxglove websocket port"
    )
    args = parser.parse_args()

    fox_server = foxglove.start_server(
        host="" if args.public else "localhost", port=args.port
    )
    foxglove.start_sysinfo_publisher()
    logging.info("🦊 Foxglove server: %s", fox_server.app_url())

    opts = ok_serial.SerialConnectionOptions(baud=args.baud, sharing="polite")
    run_relay(serial_match=args.serial, serial_opts=opts, fox_server=fox_server)


if __name__ == "__main__":
    main()
