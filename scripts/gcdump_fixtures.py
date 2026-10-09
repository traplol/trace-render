#!/usr/bin/env python3
"""Fetch the pinned Microsoft PerfView graph fixtures and independent XML oracles."""
import argparse
import hashlib
from pathlib import Path
import urllib.request


REVISION = "3d89ee8570a943290c4b517663f87493cfbaa35b"
FILES = {
    "test1.gcdump": "7e48001ef2aafd4a4a838b57097cdf01aa53aae000fc07addcb926796361324d",
    "test2.gcdump": "31122e0c12c48e80620d950e3d7e4e173eff1a8a1550f9f6fdaf0afc52855097",
    "test1_baseline.gcdump.xml": "3ebb2c055c917d76ddf5a147c22d9709a607b7bb783c294c5a26937ad1cbd3d5",
    "test2_baseline.gcdump.xml": "1b71ae657cefbb4c8dc69873a5768edf4b58cc6847e882d9a64a6aecc813a36d",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    args.directory.mkdir(parents=True, exist_ok=True)
    base = f"https://raw.githubusercontent.com/microsoft/perfview/{REVISION}/src/PerfView.Tests/GraphSerialization/inputs"
    for name, expected in FILES.items():
        path = args.directory / name
        data = path.read_bytes() if path.exists() else None
        if data is None or hashlib.sha256(data).hexdigest() != expected:
            with urllib.request.urlopen(f"{base}/{name}", timeout=60) as response:
                data = response.read()
            if hashlib.sha256(data).hexdigest() != expected:
                raise ValueError(f"Checksum mismatch: {name}")
            path.write_bytes(data)
        print(f"VERIFIED {name}")


if __name__ == "__main__":
    main()
