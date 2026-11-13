#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile an IccXML description with BCM extensions into a Brother BCM file."""
import argparse
from pathlib import Path
from bcm_text import FormatError, encode, write_atomic

from bcm_xml import load_text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="editable .xml description")
    parser.add_argument("output", type=Path, help="destination .BCM profile")
    args = parser.parse_args()
    try:
        if args.input.resolve() == args.output.resolve():
            raise FormatError("input and output must be different files")
        write_atomic(args.output, encode(load_text(args.input)))
    except (OSError, ValueError) as error:
        parser.exit(1, f"{args.input}: {error}\n")


if __name__ == "__main__":
    main()
