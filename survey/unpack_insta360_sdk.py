#!/usr/bin/env python3

import debian.debfile
import logging
import pathlib
import ok_logging_setup
import re
import shutil
import tarfile
import zipfile

ZIPFILE_RE = re.compile(r"^.*/Linux[^/]*MediaSDK.*[.]zip$")
TARFILE_RE = re.compile("^[^_.].*/MediaSDK[^/]*Linux[.]tar.*[.].*z")
MODELFILE_RE = re.compile("^[^_.].*/[^/]*[.]ins")
DEBFILE_RE = re.compile("^[^_.].*/[^/]*[.]deb")

SCRIPT_DIR = pathlib.Path(__file__).parent
SDK_DIR = SCRIPT_DIR / "insta360_sdk.tmp"
DEB_FILES_DIR = SDK_DIR / "deb_files" 

def extract_zip_tar():
    if SDK_DIR.is_dir():
        logging.info(f"📁 Using {SDK_DIR.name}/")
        return

    out_dir = SCRIPT_DIR / "insta360_extract.tmp"
    if out_dir.is_dir():
        logging.info(f"🗑️ Removing old {out_dir.name}/")
        shutil.rmtree(out_dir)

    logging.info(f"📁 Searching {SCRIPT_DIR.name}/")
    dir_files = list(SCRIPT_DIR.iterdir())
    dir_zips = [d for d in dir_files if ZIPFILE_RE.match(str(d))]
    if len(dir_zips) != 1:
        ok_logging_setup.exit("\n".join([
            f"Found {len(dir_zips)} matches for /{ZIPFILE_RE.pattern}/",
            *[f"    {z}" for z in dir_zips],
            f"from {len(dir_files)} files in {SCRIPT_DIR.name}",
            *[f"    {f}" for f in dir_files],
        ]))

    logging.info(f"📁 Creating {out_dir.name}/ ✨")
    out_dir.mkdir(parents=True)

    logging.info(f"🗜️ {dir_zips[0].name}")
    with zipfile.ZipFile(dir_zips[0]) as zipf:
        zip_tars = [f for f in zipf.namelist() if TARFILE_RE.match(f)]
        if len(zip_tars) != 1:
            ok_logging_setup.exit("\n".join([
                f"Found {len(zip_tars)} matches for /{TARFILE_RE.pattern}/",
                *[f"    {f}" for f in zip_tars],
                f"from {len(zipf.namelist())} files in {dir_zips[0].name}",
                *[f"    {f}" for f in zipf.namelist()],
            ]))

        logging.info(f"🛢️ {zip_tars[0].split("/")[-1]}")
        with zipf.open(zip_tars[0]) as tar_bin:
            with tarfile.open(name=zip_tars[0], fileobj=tar_bin) as tarf:
                for entry in tarf:
                    if MODELFILE_RE.match(entry.name):
                        out = out_dir / "models" / entry.name.split("/")[-1]
                        logging.info(f"  📷 {out.relative_to(out_dir)} ✨")
                    elif DEBFILE_RE.match(entry.name):
                        out = out_dir / entry.name.split("/")[-1]
                        logging.info(f"  📦 {out.relative_to(out_dir)} ✨")
                    else:
                        continue

                    out.parent.mkdir(parents=True, exist_ok=True)
                    out.write_bytes(tarf.extractfile(entry).read())

    if SDK_DIR.is_dir():
        logging.info(f"🗑️ Removing old {SDK_DIR.name}/")
        shutil.rmtree(SDK_DIR)
    out_dir.rename(SDK_DIR)


def extract_deb():
    if DEB_FILES_DIR.is_dir():
        logging.info(f"📁 Using {DEB_FILES_DIR.name}/")
        return

    dir_files = list(SDK_DIR.iterdir())
    dir_debs = [d for d in dir_files if DEBFILE_RE.match(str(d))]
    if len(dir_debs) != 1:
        ok_logging_setup.exit("\n".join([
            f"Found {len(dir_debs)} matches for /{DEBFILE_RE.pattern}/",
            *[f"    {z}" for z in dir_debs],
            f"from {len(dir_files)} files in {SDK_DIR.name}",
            *[f"    {f}" for f in dir_files],
        ]))

    out_dir = SDK_DIR / "deb_extract.tmp"
    if out_dir.is_dir():
        logging.info(f"🗑️ Removing old {out_dir.name}/")
        shutil.rmtree(out_dir)

    logging.info(f"📁 Creating {out_dir.name}/ ✨")
    out_dir.mkdir(parents=True)

    logging.info(f"📦 {dir_debs[0].name}")
    with debian.debfile.DebFile(dir_debs[0]) as debf:
        tarf = debf.data.tgz()
        for entry in tarf:
            parts = entry.name.split("/")
            while parts and parts[0] in ["", ".", "..", "usr"]: parts[:1] = []
            out = out_dir / "/".join(parts)
            data = tarf.extractfile(entry)
            if data:
                logging.info(f"  📄 {out.relative_to(out_dir)} ✨")
                out.parent.mkdir(parents=True, exist_ok=True)
                out.write_bytes(tarf.extractfile(entry).read())

    if DEB_FILES_DIR.is_dir():
        logging.info(f"🗑️ Removing old {DEB_FILES_DIR.name}/")
        shutil.rmtree(DEB_FILES_DIR)
    out_dir.rename(DEB_FILES_DIR)


def main():
    extract_zip_tar()
    extract_deb()


if __name__ == "__main__":
    ok_logging_setup.install()
    main()
