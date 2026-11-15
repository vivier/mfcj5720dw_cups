# SPDX-License-Identifier: GPL-3.0-or-later
"""BCM binary model for supported layouts; bcm_xml.py handles IccXML."""
import math
from pathlib import Path
import re
import struct
import tempfile
import os

FORMAT = "brother-bcm-1"
VERSION = "2.0"
VERSION_BYTES = b"\2\0\0\0"
MAX_BYTES = 64 * 1024 * 1024
MAX_TAGS = 4096
DESCRIPTION = int.from_bytes(b"desc", "big")
COPYRIGHT = int.from_bytes(b"cprt", "big")
COPYRIGHT_HEADER = b"text\0\0\0\0"
# Common cube/curve prefix; curves store their ink limit separately at byte 12.
TABLE_HEADER = struct.pack("<8I", 1, 0, 0xffff, 0, 0, 0, 0, 0)
# The density block's leading word is distinct from its editable signature.
DENSITY_PREFIX = struct.pack("<I", 1)
# Uninterpreted spans shared by all 28 original halftone blocks.
HALFTONE_DEFAULTS = (
    (0, struct.pack("<I", 1)),
    (10, b"\0" * 2),
    (14, b"\0" * 2),
    (18, b"\0" * 2),
    (22, struct.pack("<5H", 0, 4, 0, 3, 4)),
    (34, b"\0" * 94),
    # Each threshold/drop channel has six unused bytes before its error limit.
    *((offset, b"\0" * 6) for offset in range(136, 256, 16)),
)
# Uninterpreted spans shared by the three original correction blocks.
CORRECTION_DEFAULTS = (
    (0, struct.pack("<I", 1)),
    (10, b"\0" * 2),
    (14, b"\0" * 2),
    (40, b"\0" * 88),
)
BLOCK_CONSTANTS = {
    "cube": ((0, TABLE_HEADER),),
    "curves": ((0, TABLE_HEADER[:12]), (16, TABLE_HEADER[16:])),
    "halftone": HALFTONE_DEFAULTS,
    "correction": CORRECTION_DEFAULTS,
    "density": ((0, DENSITY_PREFIX), (16, b"\0" * 112)),
}
SIZES = {"cube": 39336, "curves": 4640, "halftone": 256,
         "correction": 128, "density": 128}
FAMILIES = {8: "cube", 12: "curves", 14: "halftone", 11: "correction", 5: "density"}
SIGNATURE_KINDS = {"halftone", "correction", "density"}
CURVES = ["cyan_transfer", "magenta_transfer", "yellow_transfer", "black_transfer",
          "cyan_offset", "magenta_offset", "yellow_offset", "black_offset", "monochrome_black"]
CHANNELS = ["cyan", "magenta", "yellow", "black"]
NOISE = {"noise_threshold": 8, "noise_offset": 12, "noise_floor": 16, "noise_shift": 20}
DROPS = {"large_drop": 0, "medium_drop": 2, "small_drop": 4, "black": 6, "error_limit": 14}
CORRECTION = {"column_span": (8, "H"), "row_span": (12, "H"),
              "page_rows": (16, "I"), "black_threshold": (20, "I")}


class FormatError(ValueError):
    """An invalid or unsupported BCM description or binary."""


def require(condition, message):
    if not condition:
        raise FormatError(message)


def keys(value, required, optional=()):
    require(type(value) is dict, "expected a table")
    missing = set(required) - value.keys()
    extra = value.keys() - set(required) - set(optional)
    require(not missing, "missing keys: " + ", ".join(sorted(missing)))
    require(not extra, "unknown keys: " + ", ".join(sorted(extra)))


def integer(value, bits=32, signed=False):
    low = -(1 << (bits - 1)) if signed else 0
    high = (1 << (bits - int(signed))) - 1
    require(type(value) is int and low <= value <= high,
            f"expected an integer in {low}..{high}, got {value!r}")
    return value


def sequence(value, count=None):
    require(type(value) is list, "expected an array")
    require(count is None or len(value) == count, f"expected {count} array entries")
    return value


def octets(value, length=None):
    require(type(value) is str, "expected a hexadecimal byte string")
    require(re.fullmatch(r"(?:[0-9a-fA-F]{2}\s*)*", value.strip()) is not None,
            "expected hexadecimal byte pairs, optionally separated by whitespace")
    data = bytes.fromhex(value)
    require(length is None or len(data) == length, f"expected {length} bytes")
    return data


def latin1(value, length=None):
    require(type(value) is str, "expected a Latin-1 string")
    try:
        data = value.encode("latin1")
    except UnicodeEncodeError as error:
        raise FormatError("text must contain only U+0000..U+00FF") from error
    require(length is None or len(data) == length, f"expected {length} text bytes")
    return data


def tag_number(value):
    if type(value) is str:
        return int.from_bytes(latin1(value, 4), "big")
    return integer(value)


def family(tag):
    if tag == DESCRIPTION:
        return "description"
    if tag == COPYRIGHT:
        return "copyright"
    return FAMILIES.get(tag >> 12, "raw")


def color_expression(model, components):
    return model + "(" + ", ".join(map(str, components)) + ")"


def color_components(value, model):
    """Read decimal components without evaluating an expression as code."""
    require(type(value) is str, f"expected {model}(...) text")
    component = r"\s*(-?[0-9]+)\s*"
    match = re.fullmatch(model + r"\(" + ",".join([component] * len(model)) + r"\)", value)
    require(match is not None, f"expected {model} with {len(model)} decimal components")
    return [integer(int(n), 16) for n in match.groups()]


def creation_text(stamp):
    year, month, day, hour, minute, second = stamp
    return f"{day:02d}/{month:02d}/{year:04d} {hour:02d}:{minute:02d}:{second:02d}"


def creation_stamp(value):
    require(type(value) is str, "expected creation time as DD/MM/YYYY HH:MM:SS")
    match = re.fullmatch(r"([0-9]{2,5})/([0-9]{2,5})/([0-9]{4,5}) "
                         r"([0-9]{2,5}):([0-9]{2,5}):([0-9]{2,5})", value)
    require(match is not None, "expected creation time as DD/MM/YYYY HH:MM:SS")
    day, month, year, hour, minute, second = [integer(int(n), 16) for n in match.groups()]
    # Preserve all six stored words, including unusual dates; do not normalize.
    return year, month, day, hour, minute, second


def float_bytes(value):
    if type(value) is dict:
        keys(value, ["bits"])
        return struct.pack("<I", integer(value["bits"]))
    require(type(value) in (int, float), "expected a float32 number or an explicit bit pattern")
    try:
        require(math.isfinite(value), "non-finite float32 values require explicit bits")
        return struct.pack("<f", value)
    except (OverflowError, struct.error) as error:
        raise FormatError("value is outside the float32 range") from error


def read_float(data):
    number = struct.unpack("<f", data)[0]
    if not math.isfinite(number):
        return {"bits": struct.unpack("<I", data)[0]}
    return number


class Record:
    """Read or write disjoint fields and validate reserved bytes."""
    def __init__(self, size, data=None):
        self.data = bytearray(size) if data is None else bytearray(data)
        require(len(self.data) == size, f"expected a {size}-byte record")
        self.used = bytearray(size)

    def claim(self, offset, size):
        require(0 <= offset <= len(self.data) and 0 <= size <= len(self.data) - offset,
                "byte span extends outside its record")
        require(not any(self.used[offset:offset + size]), "overlapping fields")
        self.used[offset:offset + size] = b"\1" * size

    def read(self, offset, size):
        self.claim(offset, size)
        return bytes(self.data[offset:offset + size])

    def write(self, offset, data):
        self.claim(offset, len(data))
        self.data[offset:offset + len(data)] = data

    def number(self, offset, fmt):
        return struct.unpack("<" + fmt, self.read(offset, struct.calcsize("<" + fmt)))[0]

    def put_number(self, offset, fmt, value):
        bits = {"H": 16, "h": 16, "I": 32}[fmt]
        self.write(offset, struct.pack("<" + fmt, integer(value, bits, fmt == "h")))

    def check_reserved(self):
        for offset, (value, used) in enumerate(zip(self.data, self.used)):
            if not used:
                require(value == 0,
                        f"unsupported nonzero reserved byte at record offset 0x{offset:x}")


def decode_header(data):
    record = Record(128, data)
    record.read(0, 4)  # The size is derived when writing.
    version = record.read(8, 4)
    require(version == VERSION_BYTES, "unsupported BCM version marker")
    result = {"version": VERSION,
            "created": creation_text(struct.unpack(">6H", record.read(24, 12))),
            "manufacturer": record.read(48, 4).decode("latin1"),
            "model": record.read(52, 4).decode("latin1")}
    record.check_reserved()
    return result


def encode_header(header):
    keys(header, ["version", "created", "manufacturer", "model"])
    record = Record(128)
    record.write(0, b"\0" * 4)
    require(header["version"] == VERSION, 'supported BCM version is "2.0"')
    record.write(8, VERSION_BYTES)
    stamp = creation_stamp(header["created"])
    record.write(24, struct.pack(">6H", *stamp))
    record.write(48, latin1(header["manufacturer"], 4))
    record.write(52, latin1(header["model"], 4))
    return bytes(record.data)


def decode_block(kind, data):
    if kind == "raw":
        return {"data": data.hex(" ")}
    if kind in ("description", "copyright"):
        signature = b"desc" if kind == "description" else b"text"
        start = 12 if kind == "description" else 8
        require(len(data) >= start and data[:4] == signature, "invalid text block signature/size")
        end = start + struct.unpack_from(">I", data, 8)[0] if start == 12 else len(data)
        require(start <= end <= len(data), "description text count extends outside its block")
        text = data[start:end]
        nul = text.find(b"\0")
        nul = len(text) if nul < 0 else nul
        result = {"text": text[:nul].decode("latin1"), "padding": text[nul:].hex(" ")}
        if start == 12:
            result["reserved"] = data[4:8].hex(" ")
            result["trailing"] = data[end:].hex(" ")
        else:
            require(data[:8] == COPYRIGHT_HEADER, "unsupported nonzero copyright reserved bytes")
        return result
    record = Record(SIZES[kind], data)
    for offset, constant in BLOCK_CONSTANTS[kind]:
        require(record.read(offset, len(constant)) == constant,
                f"unsupported fixed bytes at block offset 0x{offset:x}")
    result = {}
    if kind in SIGNATURE_KINDS:
        result["signature"] = record.read(4, 4).decode("latin1")
    if kind == "cube":
        samples = struct.unpack("<19652H", record.read(32, 39304))
        result["nodes"] = {color_expression("RGB", (i // 289, i // 17 % 17, i % 17)):
                           color_expression("CMYK", samples[i * 4:i * 4 + 4])
                           for i in range(4913)}
    elif kind == "curves":
        result["ink_limit"] = record.number(12, "I")
        result["curves"] = {name: list(struct.unpack("<256" + ("h" if 4 <= i < 8 else "H"),
                            record.read(32 + i * 512, 512))) for i, name in enumerate(CURVES)}
    elif kind == "halftone":
        result.update({name: record.number(offset, "H") for name, offset in NOISE.items()})
        result["head_mode"] = record.number(32, "h")
        for group, start in [("thresholds", 128), ("drops", 192)]:
            result[group] = [{name: record.number(start + i * 16 + offset, "H")
                              for name, offset in DROPS.items()} for i in range(4)]
    elif kind == "correction":
        result.update({name: record.number(offset, fmt) for name, (offset, fmt) in CORRECTION.items()})
        result["factors"] = {name: read_float(record.read(24 + i * 4, 4))
                             for i, name in enumerate(CHANNELS)}
    elif kind == "density":
        result["lower_percent"] = record.number(8, "I")
        result["upper_percent"] = record.number(12, "I")
    record.check_reserved()
    return result


def text_padding(text, unit_bytes=1):
    """Add a NUL code unit, then align the encoded text to four bytes."""
    require(unit_bytes in (1, 2) and len(text) % unit_bytes == 0,
            "text must contain complete one- or two-byte code units")
    return b"\0" * (unit_bytes + (-(len(text) + unit_bytes) % 4))


def encode_block(block):
    require(type(block) is dict, "expected a block table")
    kind = block.get("kind")
    require(type(kind) is str and kind in [*SIZES, "description", "copyright", "raw"],
            "unknown block kind")
    common = ["name", "kind"]
    optional = ["after"]
    if kind == "raw":
        keys(block, common + ["data"], optional)
        return octets(block["data"])
    if kind == "copyright":
        keys(block, common + ["text"], optional + ["padding"])
        text = latin1(block["text"])
        padding = octets(block["padding"]) if "padding" in block else text_padding(text)
        return COPYRIGHT_HEADER + text + padding
    if kind == "description":
        keys(block, common + ["reserved", "text", "trailing"], optional + ["padding"])
        text = latin1(block["text"])
        text += octets(block["padding"]) if "padding" in block else text_padding(text)
        reserved = octets(block["reserved"], 4)
        return b"desc" + reserved + struct.pack(">I", len(text)) + text + octets(block["trailing"])
    required = {"cube": ["nodes"], "curves": ["ink_limit", "curves"],
                "halftone": [*NOISE, "head_mode", "thresholds", "drops"],
                "correction": [*CORRECTION, "factors"], "density": ["lower_percent", "upper_percent"]}
    signature_fields = ["signature"] if kind in SIGNATURE_KINDS else []
    keys(block, common + required[kind] + signature_fields, optional)
    record = Record(SIZES[kind])
    for offset, constant in BLOCK_CONSTANTS[kind]:
        record.write(offset, constant)
    if kind in SIGNATURE_KINDS:
        record.write(4, latin1(block["signature"], 4))
    if kind == "cube":
        nodes = block["nodes"]
        require(type(nodes) is dict and len(nodes) == 4913, "expected a table of 4913 RGB-to-CMYK nodes")
        seen = set()
        for rgb, cmyk in nodes.items():
            r, g, b = color_components(rgb, "RGB")
            require(max(r, g, b) <= 16, "cube coordinates must lie in 0..16")
            index = (r * 17 + g) * 17 + b
            require(index not in seen, f"duplicate cube node ({r}, {g}, {b})")
            seen.add(index)
            record.write(32 + index * 8, struct.pack("<4H", *color_components(cmyk, "CMYK")))
    elif kind == "curves":
        record.put_number(12, "I", block["ink_limit"])
        keys(block["curves"], CURVES)
        for i, name in enumerate(CURVES):
            signed = 4 <= i < 8
            values = [integer(n, 16, signed) for n in sequence(block["curves"][name], 256)]
            record.write(32 + i * 512, struct.pack("<256" + ("h" if signed else "H"), *values))
    elif kind == "halftone":
        for name, offset in NOISE.items():
            record.put_number(offset, "H", block[name])
        record.put_number(32, "h", block["head_mode"])
        for group, start in [("thresholds", 128), ("drops", 192)]:
            for i, channel in enumerate(sequence(block[group], 4)):
                keys(channel, DROPS)
                for name, offset in DROPS.items():
                    record.put_number(start + i * 16 + offset, "H", channel[name])
    elif kind == "correction":
        for name, (offset, fmt) in CORRECTION.items():
            record.put_number(offset, fmt, block[name])
        keys(block["factors"], CHANNELS)
        for i, name in enumerate(CHANNELS):
            record.write(24 + i * 4, float_bytes(block["factors"][name]))
    elif kind == "density":
        record.put_number(8, "I", block["lower_percent"])
        record.put_number(12, "I", block["upper_percent"])
    return bytes(record.data)


def decode(data):
    require(132 <= len(data) <= MAX_BYTES, "BCM size must be between 132 bytes and 64 MiB")
    require(struct.unpack_from(">I", data)[0] == len(data), "declared BCM size does not match file")
    header = decode_header(data[:128])
    count = struct.unpack_from(">I", data, 128)[0]
    end = 132 + count * 12
    require(count <= MAX_TAGS and end <= len(data), "invalid tag directory size")
    entries = [struct.unpack_from(">III", data, 132 + i * 12) for i in range(count)]
    require(len({t for t, _, _ in entries}) == count, "duplicate directory tag")
    blocks = {}
    for tag, offset, length in entries:
        require(end <= offset <= len(data) and 0 < length <= len(data) - offset,
                "tag block lies outside the file or overlaps the directory")
        require(offset % 4 == 0, "unaligned block offset")
        blocks.setdefault((offset, length), []).append(tag)
    ordered = sorted(blocks)
    names = {}
    output = []
    for index, (offset, length) in enumerate(ordered):
        tags = blocks[offset, length]
        kinds = {family(t) for t in tags} - {"raw"}
        require(len(kinds) <= 1, "aliased tags have incompatible block families")
        kind = next(iter(kinds)) if kinds else "raw"
        name = f"{kind}_{tags[0]:04x}"
        names[offset, length] = name
        next_offset = ordered[index + 1][0] if index + 1 < len(ordered) else len(data)
        require(offset + length <= next_offset, "overlapping tag blocks")
        block = {"name": name, "kind": kind, "after": data[offset + length:next_offset].hex(" ")}
        try:
            block.update(decode_block(kind, data[offset:offset + length]))
        except FormatError as error:
            raise FormatError(f"{name}: {error}") from error
        output.append(block)
    return {"format": FORMAT, "header": header,
            "directory_padding": data[end:ordered[0][0] if ordered else len(data)].hex(" "),
            "tags": [{"id": tag, "block": names[offset, length]} for tag, offset, length in entries],
            "blocks": output}


def encode(document):
    keys(document, ["format", "header", "tags", "blocks"], ["directory_padding"])
    require(document["format"] == FORMAT, "unsupported description format")
    tags = sequence(document["tags"])
    blocks = sequence(document["blocks"])
    require(len(tags) <= MAX_TAGS and len(blocks) <= len(tags), "too many tags or blocks")
    names = {}
    for block in blocks:
        require(type(block) is dict, "expected a block table")
        name = block.get("name")
        require(type(name) is str and re.fullmatch(r"[A-Za-z][A-Za-z0-9_-]*", name), "invalid block name")
        require(name not in names, "duplicate block name: " + name)
        names[name] = block
    seen = set()
    referenced = set()
    for tag in tags:
        keys(tag, ["id", "block"])
        number = tag_number(tag["id"])
        require(number not in seen, "duplicate directory tag")
        seen.add(number)
        require(type(tag["block"]) is str and tag["block"] in names, "unknown block reference")
        referenced.add(tag["block"])
        expected = family(number)
        require(expected == "raw" or expected == names[tag["block"]].get("kind"),
                f"tag 0x{number:08x} has the wrong block kind")
    require(referenced == names.keys(), "unreferenced block")
    data = bytearray(encode_header(document["header"]))
    data.extend(struct.pack(">I", len(tags)))
    data.extend(b"\0" * (12 * len(tags)))
    data.extend(octets(document.get("directory_padding", "")))
    offsets = {}
    for name, block in names.items():
        try:
            payload = encode_block(block)
            require(len(payload) > 0, "empty block")
            after = octets(block.get("after", ""))
            data.extend(b"\0" * (-len(data) % 4))
            offsets[name] = (len(data), len(payload))
            require(len(data) + len(payload) + len(after) <= MAX_BYTES, "BCM exceeds 64 MiB")
            data.extend(payload)
            data.extend(after)
        except FormatError as error:
            raise FormatError(f"{name}: {error}") from error
    require(len(data) <= MAX_BYTES, "BCM exceeds 64 MiB")
    for i, tag in enumerate(tags):
        struct.pack_into(">III", data, 132 + i * 12, tag_number(tag["id"]), *offsets[tag["block"]])
    struct.pack_into(">I", data, 0, len(data))
    return bytes(data)


def read_limited(path):
    with Path(path).open("rb") as stream:
        data = stream.read(MAX_BYTES + 1)
    require(len(data) <= MAX_BYTES, "input exceeds 64 MiB")
    return data


def write_atomic(path, data):
    """Keep a previously generated output intact if conversion fails."""
    path = Path(path)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix="." + path.name + ".", delete=False) as stream:
        temporary = Path(stream.name)
        try:
            stream.write(data)
            stream.close()
            temporary.chmod(0o644)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
