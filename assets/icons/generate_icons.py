#!/usr/bin/env python3
"""Regenerate application icons using Python 3.9+ and rsvg-convert."""

import shutil
import struct
import subprocess
from pathlib import Path
from tempfile import TemporaryDirectory


SIZES = (16, 24, 32, 48, 64, 128, 256, 512, 1024)
ICNS_TYPES = (
    (b"icp4", 16),
    (b"icp5", 32),
    (b"icp6", 64),
    (b"ic07", 128),
    (b"ic08", 256),
    (b"ic09", 512),
    (b"ic10", 1024),
)


def make_ico(images: dict[int, bytes]) -> bytes:
    sizes = [size for size in SIZES if size <= 256]
    offset = 6 + 16 * len(sizes)
    entries = []
    for size in sizes:
        dimension = size if size < 256 else 0
        entries.append(
            struct.pack(
                "<BBBBHHII", dimension, dimension, 0, 0, 1, 32,
                len(images[size]), offset,
            )
        )
        offset += len(images[size])
    return (
        struct.pack("<HHH", 0, 1, len(sizes))
        + b"".join(entries)
        + b"".join(images[size] for size in sizes)
    )


def make_icns(images: dict[int, bytes]) -> bytes:
    chunks = b"".join(
        struct.pack(">4sI", kind, 8 + len(images[size])) + images[size]
        for kind, size in ICNS_TYPES
    )
    return struct.pack(">4sI", b"icns", 8 + len(chunks)) + chunks


def main() -> None:
    renderer = shutil.which("rsvg-convert")
    if renderer is None:
        raise SystemExit("rsvg-convert (librsvg) is required on PATH to regenerate icons.")
    directory = Path(__file__).resolve().parent
    images: dict[int, bytes] = {}
    with TemporaryDirectory(prefix="codecairn-icons-") as temporary:
        for size in SIZES:
            destination = Path(temporary) / f"codecairn-{size}.png"
            subprocess.run(
                [
                    renderer, "--width", str(size), "--height", str(size),
                    "--output", str(destination), str(directory / "codecairn.svg"),
                ],
                check=True,
            )
            images[size] = destination.read_bytes()
    ico = make_ico(images)
    icns = make_icns(images)
    for size, image in images.items():
        (directory / f"codecairn-{size}.png").write_bytes(image)
    (directory / "codecairn.ico").write_bytes(ico)
    (directory / "codecairn.icns").write_bytes(icns)
    print("Generated PNG, Windows ICO, and macOS ICNS icons.")


if __name__ == "__main__":
    main()
