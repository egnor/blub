#!/usr/bin/env python3

import argparse
import base64
import datetime
import paho.mqtt.client
import json
import logging
import os
from pathlib import Path
import signal
import time
import urllib.parse


urllib.parse.uses_relative.extend(["mqtt", "mqtts"])
urllib.parse.uses_netloc.extend(["mqtt", "mqtts"])


def parse_mqtt_url(arg):
    arg = f"mqtt://{arg}" if "//" not in arg else arg
    try:
        url = urllib.parse.urlsplit(arg)
    except ValueError as e:
        raise argparse.ArgumentTypeError(str(e))

    scheme_port = {"mqtt": 1883, "mqtts": 8883}.get(url.scheme)
    if not scheme_port:
        raise argparse.ArgumentTypeError(f"Bad MQTT URL scheme: {url.scheme}")
    if not url.hostname:
        raise argparse.ArgumentTypeError("Missing MQTT server hostname")
    if not url.port:
        url = url._replace(netloc=f"{url.netloc.rstrip(':')}:{scheme_port}")
    return url


def make_client(url):
    api_version = paho.mqtt.client.CallbackAPIVersion.VERSION2
    client = paho.mqtt.client.Client(callback_api_version=api_version)
    client.enable_logger(logging.getLogger("mqtt"))
    client.tls_set() if url.scheme.endswith("s") else None
    client.username_pw_set(url.username, url.password)
    client.connect_async(url.hostname, url.port)
    return client


class LogWriter:
    def __init__(self, dir):
        self.file = None
        self.dir = dir
        os.makedirs(dir, exist_ok=True)

    def log_message(self, client, userdata, message):
        dt = datetime.datetime.now()
        filedate = dt.strftime("%Y-%m-%d%z")
        filepath = str(self.dir / f"{filedate}.log")
        if not self.file or self.file.name != filepath:
            self.file.close() if self.file else None
            self.file = open(filepath, "a")

        log_json = {
            "ts": dt.isoformat(),
            "t": message.topic,
        }

        try:
            text = message.payload.decode("utf-8")
            try:
                payload_json = json.loads(text)
                if isinstance(payload_json, (dict, int, float)):
                    log_json["m"] = payload_json
                else:
                    log_json["m"] = text
            except json.JSONDecodeError:
                log_json["m"] = text

        except UnicodeDecodeError:
            log_json["b"] = base64.b64encode(message.payload).decode("ascii")
            text = None

        print(json.dumps(log_json), file=self.file)

    def flush(self):
        self.file.flush() if self.file else None


if __name__ == "__main__":
    signal.signal(signal.SIGINT, signal.SIG_DFL)  # sane ^C behavior
    logging.basicConfig(level=logging.DEBUG)
    parser = argparse.ArgumentParser(description="MQTT logger")
    parser.add_argument(
        "--server", type=parse_mqtt_url, required=True, help="MQTT URL"
    )
    parser.add_argument("--topic", nargs="+", default=["#"], help="Topic(s)")
    parser.add_argument("--qos", type=int, default=0, help="MQTT QoS level")
    parser.add_argument("--dir", type=Path, default=".", help="Log directory")
    args = parser.parse_args()
    writer = LogWriter(args.dir)

    topics = [(topic, args.qos) for topic in args.topic]
    client = make_client(args.server)
    client.on_message = writer.log_message
    client.on_connect = lambda *a: client.subscribe(topics)

    client.loop_start()
    while True:
        writer.flush()
        time.sleep(0.25)
