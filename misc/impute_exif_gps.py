#!/usr/bin/env python3

import argparse
import bisect
import pathlib
import piexif

from datetime import datetime

EXIF_TIME_FORMAT = "%Y:%m:%d %H:%M:%S"

parser = argparse.ArgumentParser()
parser.add_argument("--offset", default=10.0, help="Max timestamp offset")
parser.add_argument("dir", nargs="+", help="Directory to scan for images")
args = parser.parse_args()

timestamp_gps = {}
nogps_timestamp = {}

print("READING")
for dir_name in args.dir:
    print(f"📁 {dir_name}")
    for file_path in sorted(pathlib.Path(dir_name).iterdir()):
        if file_path.is_dir() or file_path.suffix.lower() in ('.dng', ):
            continue

        try:
            exif = piexif.load(str(file_path))
        except ValueError:
            print(f"  🚫 {file_path.name}")
            continue

        # https://github.com/hMatoba/Piexif/issues/86
        exif["0th"].pop(piexif.ImageIFD.AsShotNeutral, None)
        exif_time = exif["0th"].get(piexif.ImageIFD.DateTime, b"").decode()
        pic_time = exif_time and datetime.strptime(exif_time, EXIF_TIME_FORMAT)
        pic_stamp = pic_time and pic_time.timestamp()

        gps_data = {}
        for coord in ("Latitude", "Longitude", "Altitude"):
            ref = exif["GPS"].get(getattr(piexif.GPSIFD, f"GPS{coord}Ref"))
            fracs = exif["GPS"].get(getattr(piexif.GPSIFD, f"GPS{coord}"), [])
            if len(fracs) == 2 and all(isinstance(f, int) for f in fracs):
                fracs = [fracs]
            vals = [ref, *((n / d) for n, d in fracs)]
            if ref and vals and any(vals[1:]):
                gps_data[coord] = (ref, vals)

        if not ("Latitude" in gps_data and "Longitude" in gps_data):
            gps_data = {}

        gps_emoji = "🌎" if gps_data else "  "
        when = datetime.fromtimestamp(pic_stamp) if pic_stamp else "[no time]"
        print(f"  🖼️ {gps_emoji} {when} {file_path.name}")

        if pic_stamp:
            if gps_data:
                timestamp_gps[pic_stamp] = exif["GPS"]
            else:
                nogps_timestamp[file_path] = pic_stamp

    print()

if not timestamp_gps:
    print("💀 NO GPS-TAGGED IMAGES")
elif not nogps_timestamp:
    print("✨ NO NON-GPS-TAGGED IMAGES")
else:
    print("UPDATING")
    timestamps = list(sorted(timestamp_gps.keys()))
    for nogps_path, nogps_stamp in sorted(nogps_timestamp.items()):
        index = bisect.bisect_left(timestamps, nogps_stamp)
        match = timestamps[max(index - 1, 0)]
        if index < len(timestamps):
            if abs(nogps_stamp - timestamps[index]) < abs(nogps_stamp - match):
                match = timestamps[index]
        offset = abs(nogps_stamp - match)
        if offset > args.offset:
            print(f"⌛ no match ({offset:.1f}s) {nogps_path}")
            continue

        print(f"🌎 ({offset:.1f}s) {nogps_path}")
        exif = piexif.load(str(nogps_path))
        exif["GPS"] = timestamp_gps[match]
        print(exif)
        piexif.insert(piexif.dump(exif), str(nogps_path))
