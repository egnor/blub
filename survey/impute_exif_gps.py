#!/usr/bin/env python3

import argparse
import ok_logging_setup
import pathlib
import piexif


def read_exifs(paths):
    for image_path in paths:
        exif = piexif.load(str(image_path))
        print(image_path, exif)


def impute_exifs(path_exif):
    pass


def write_exifs(path_exif):
    pass


if __name__ == "__main__":
    ok_logging_setup.install()

    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="+", type=pathlib.Path)
    args = parser.parse_args()

    path_exif = read_exifs(args.image)
    impute_exifs(path_exif)
    write_exifs(path_exif)
