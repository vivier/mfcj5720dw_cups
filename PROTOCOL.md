# Brother MFC-J5720DW print protocol specification

**Status:** independently derived engineering specification, revision 1.0,
2026-11-18. **Target:** Brother MFC-J5720DW, with the behavior observed on the
tested printer and in the reconstructed native encoder.

This document specifies the known outbound raster/JPEG command language,
maintenance framing and the relationship to discovery, transport and status
queries. It is intended for implementing a stream reader or a compatible
sender. It does not require access to proprietary drivers, private analysis
files or a particular CUPS implementation to read the wire descriptions.

This is not an official Brother specification or a claim of compatibility with
every product advertising HBP. Public specifications are used for the standard
parts and for comparison; the model-specific fields come from code analysis
and the captures described here. Unknown fields remain explicitly unknown.
In particular, this is not yet a complete independent specification of the
color-conversion and printhead-scheduling algorithms.

### Evidence and requirements

The following evidence classes distinguish a byte layout from its mechanical
interpretation. They can apply together, and are not interchangeable:

| Class | What it establishes |
| --- | --- |
| Public reference | A published definition, within that reference's stated models and scope |
| Code-derived | What the current encoder emits or computes; firmware acceptance is not implied |
| Capture-checked | The bytes occur in a saved stream and pass structural checks |
| Print-tested | An actual printed result was checked for the stated setting |
| Unresolved | Meaning, admissible values or device behavior still needs evidence |

Unless stated otherwise, a command's byte layout is **code-derived**. Section
10 identifies the capture-checked examples. Physical tests establish ordinary
printing, duplex sheet ordering and the reported A4 borderless placement;
they do not validate every parameter combination in this document.

Here, **must** states a requirement for an implementation following this
specification, and **should** states a conservative compatibility recommendation.
Neither word claims that the firmware rejects every alternative. An encoder's
chosen limit is identified separately from the range representable on the wire.

## Contents

1. [Scope and notation](#1-scope-and-notation)
2. [Job and page framing](#2-job-and-page-framing)
3. [Page settings](#3-page-settings)
4. [Raster swaths and rows](#4-raster-swaths-and-rows)
5. [Raster compression](#5-raster-compression)
6. [Symbol counters](#6-symbol-counters)
7. [JPEG data](#7-jpeg-data)
8. [Duplex printing](#8-duplex-printing)
9. [Additional commands](#9-additional-commands)
10. [Annotated captures](#10-annotated-captures)
11. [Coverage and open questions](#11-coverage-and-open-questions)
12. [Command reference index](#12-command-reference-index)
13. [Implementation and validation requirements](#13-implementation-and-validation-requirements)
14. [Public references](#14-public-references)

## 1. Scope and notation

The print-data language is separate from its transport. A transport adapter
delivers the byte stream below without modifying its octets. LPD control
records, IPP attributes and USB transactions are not part of that stream.

### Device identity and transport

The observed device identification includes:

```text
MFG:Brother;MDL:MFC-J5720DW;CMD:HBP,BRPJL,URF;
```

The order of these fields is immaterial. `HBP` identifies the host-based
printing capability; it is not a versioned guarantee of compatibility with
the old Brother laser HBP commands. `BRPJL` and `URF` are other advertised
capabilities, not instructions to insert an extra header into every raster job.

The observed DNS-SD IPP service advertises `rp=ipp/print`, `rfo=ipp/faxout`,
and these document formats:

```text
application/octet-stream
application/vnd.brother-hbp
image/pwg-raster
image/urf
image/jpeg
```

These are discovery observations, not proof that every listed format works
with every IPP operation or print setting. A sender using IPP must query the
selected endpoint's `document-format-supported` and use the appropriate
`document-format` attribute. The fax resource is distinct from the print
resource. The JPEG embedded in section 7 belongs to a Brother command stream;
it is not the same submission format as a standalone `image/jpeg` IPP document.

| Transport | Where the described stream belongs | Evidence and boundary |
| --- | --- | --- |
| Raw TCP, normally port 9100 | Connection's print-data bytes | Used for the project's physical print tests |
| LPD, normally port 515, queue `BINARY_P1` | LPD data file, with separate control-file and acknowledgement handling | Queue observed in discovery; outer framing is defined by [RFC 1179](https://www.rfc-editor.org/rfc/rfc1179.html) |
| IPP, observed resource `/ipp/print` | Document data after the IPP attributes | Endpoint and formats advertised; native Brother-stream submission over IPP is not established by the captures here |
| USB | Printer data endpoint, outside USB framing | No USB-specific transaction or status specification is supplied here |

[RFC 8010](https://www.rfc-editor.org/rfc/rfc8010.html) defines IPP encoding and
HTTP transport; [RFC 8011](https://www.rfc-editor.org/rfc/rfc8011.html) defines
operations and attributes. Their job/status semantics do not define the
Brother raster commands. A successful write or accepted job is not itself
proof that a sheet has finished printing.

### Rendering responsibilities

There are two image paths:

- **Raster:** the sender converts pixels to ink symbols, arranges them into
  printhead swaths and compresses the resulting channel rows.
- **JPEG:** the sender provides an encoded image together with Brother page
  settings and chunk framing.

Raster data represents printhead output, not an RGB image sent unchanged to
the printer. Halftoning, dot quantization, interlacing and feed scheduling are
part of preparing that data.

```text
Source image
  |-- color conversion, ink limits, halftoning, nozzle/pass scheduling
  |      -> packed ink rows -> row compression -> raster command stream
  |
  `-- image preparation -> baseline JPEG -> Brother JPEG command stream

Command stream -> chosen transport -> printer
```

BCM color cubes and curves are host-side processing data. They are not sent as
BCM files or ICC profiles in the command stream. Factory calibration read from
the printer can change raster thresholds and density without adding a new
page-header command. These responsibilities explain why reproducing only the
command headers is insufficient to reproduce print quality.

### Relationship to published command languages

The [Brother Laser Printer Technical Reference Guide, revision R, appendix
C](https://www.officestationery.co.uk/documents/BA69340.pdf#page=472) limits its
HBP chapter to HL-820/1020/1040. It enters that mode through PJL and uses commands
such as `@L` and `@G`. The MFC-J5720DW streams below enter raster or JPEG mode
with `ESC |` and use different framing. Do not apply that laser command table
to this inkjet merely because both advertise `HBP`.

The independent [HBP documentation project](https://sourceforge.net/projects/hbp-for-brother/)
also targets Brother laser printers, especially job-control options. It is
background material rather than a specification of this model.

The observed stream is a Brother-specific raster/JPEG dialect. Its command
structure resembles the published [Brother Raster Command Reference for
PT-E550W/P750W/P710BT](https://download.brother.com/welcome/docp100064/cv_pte550wp750wp710bt_eng_raster_102.pdf):
both use `ESC @` for initialization, `M` for compression selection, `G` for
raster data, `Z` for a blank row and `FF` for printing. This resemblance does
not establish binary compatibility. That reference selects raster mode with
`1b 69 61 01` and PackBits with `4d 02`; the MFC stream described here uses
`1b 7c 01 00 0d` and `4d 07`, respectively.

Epson's [ESC/POS initialization command](https://download4.epson.biz/sec_pubs/pos/reference_en/escpos/esc_atsign.html)
also uses `1b 40`, but a shared reset sequence does not identify the language.
ESC/POS [graphics commands](https://download4.epson.biz/sec_pubs/pos/reference_en/escpos/gs_lparen_cl.html)
use different framing. The MFC language selectors, page settings and JPEG
commands below must be treated as model-specific; ESC/POS parameters cannot
be substituted for them.

### Notation

| Notation | Meaning |
| --- | --- |
| `1b 40` | Two hexadecimal bytes |
| `'G'` | The single ASCII byte `47`, without a string terminator |
| `u8` | One unsigned byte |
| `u16le`, `u24le`, `u32le` | Unsigned integer, least significant byte first |
| `s16le` | Signed 16-bit little-endian integer, two's complement |
| `NUL` | One `00` byte |
| `[command]` in a sequence | A conditional or optional command |

Byte values and command codes are hexadecimal. Counts, dimensions and offsets
are decimal unless prefixed with `0x`. For example, `a3 01` represents 419,
and `fb ff` represents -5 when interpreted as `s16le`.

There is no uniform outer packet containing a global length, sequence number
or CRC. Each command defines its own framing. A data payload can contain any
command byte, including `0c`, `1b` and `47`. Consume the declared payload length
before parsing the next command. Transport packet boundaries do not delimit
commands.

The integer fields marked `le` below are little-endian. Encapsulated JPEG data
retains JPEG's own byte ordering and marker conventions.

For bytes `b0`, `b1`, `b2`, `b3`, unsigned lengths are decoded as:

```text
u16le = b0 + 256*b1
u24le = b0 + 256*b1 + 65536*b2
u32le = b0 + 256*b1 + 65536*b2 + 16777216*b3
s16le = u16le if u16le < 32768, otherwise u16le - 65536
```

All sizes count octets. A command length excludes its opcode and length field
unless explicitly stated otherwise. ASCII letters in command names denote
literal byte values; text such as `distance:u16le` denotes a binary field,
not decimal characters. No CR, LF, NUL, alignment padding or byte stuffing
may be added except where the command explicitly contains it.

| Quantity | Unit or representation | Status |
| --- | --- | --- |
| `R`, `q` | Enumerated mode bytes | Not literal dpi |
| `S` | Signed edges at 600 units/inch | Scale follows the encoder's geometry conversions |
| Custom `s ff` | Unsigned dimensions at 300 units/inch | Code-derived, not physically validated |
| `F` | Feed distance at 24,000 units/inch | Code-derived mechanical scale |
| JPEG `p` | Image pixels | Also described by the JPEG frame header |
| `P 90` | Nozzle/pass indices and row count | Not image pixels or millimetres |
| `g` | Counts of quantized symbols | Not ink-volume units |
| X/Y origins | Two-byte fields, zero in the supported sequences | Nonzero units unresolved |

## 2. Job and page framing

### Fixed sequences

| Bytes | Meaning |
| --- | --- |
| `00` repeated 4,608 times | Initialization padding |
| `1b 40` | Reset at job boundaries |
| `1b 7c 01 00 0d` | Conventional raster mode |
| `1b 7c 01 01 0d` | Raster mode used with range-coded swaths (`M 0c`) |
| `1b 7c 02 00 0d` | JPEG mode |
| `0c` | End of a raster page |
| `47 00 00 ff` | Special end-of-page marker for JPEG |

Initialization padding contains exactly 4,608 zero bytes (`0x1200`). Its exact
firmware purpose is unknown. The sequences below preserve its observed length
and placement.

### Raster job

```text
Job start:
    00 repeated 4608 times
    1b 7c 01 00 0d
    1b 40

For each page:
    1b 7c 01 00 0d             # use 1b 7c 01 01 0d for M 0c
    R, I, M, D
    N 04, N 05
    P 90
    T, s, S
    [2S, 2R]                   # duplex pages
    y
    Up, Uq, Ur
    59 00 00                   # Y
    58 00 00                   # X
    00 repeated 4608 times
    page data: I updates, feed commands and swaths
    0c

Job end:
    1b 40
```

`I` and `D` can be repeated during the page. A parser must retain their current
values rather than treating them as page-header-only commands. Final feed
commands may precede the page terminator.

The initial language selector before `ESC @` is deliberately repeated at the
start of the page. A sender should preserve that order and both padding
locations. The effect of eliminating the first selector or relying on settings
left by a previous job is not established.

### JPEG job

```text
Job start:
    00 repeated 4608 times
    1b 7c 02 00 0d
    1b 40

For each page:
    1b 7c 02 00 0d
    4Y identifier NUL
    R, q, p, I, D
    N 04, N 05
    T, s, S
    [2S, 2R]                   # code-derived duplex variant; see below
    h, y
    59 00 00
    58 00 00
    G chunks containing fragments of one JPEG image
    47 00 00 ff

Job end:
    1b 40
```

JPEG pages have no second 4,608-byte padding block after their header. They do
not use `M`, `P 90`, `Up`, `Uq`, `Ur` or conventional raster swaths in this
sequence. JPEG with duplex is not established by the captures described here;
the captures demonstrate simplex JPEG printing. The encoder can insert the
same duplex fields used in raster mode after `S`; that is a code-derived
extension, not a print-tested JPEG duplex profile.

### Stream state and page boundaries

A reader needs an explicit state, rather than searching for opcode bytes:

| State | Expected next items | State change |
| --- | --- | --- |
| Job preamble | 4,608 NULs, language selector, `ESC @` | Ready for a page header |
| Page header | Language selector and the ordered mode-specific settings above | Raster or JPEG page body |
| Conventional raster body | Feed, optional `I`/`D` updates, swath marker, counters, `G`/`Z` records | `0c` ends the page |
| Range-coded raster body | Swath marker, `b`/`j`, counters, then channel envelopes | `0c` ends the page |
| JPEG body | Length-delimited `G` fragments | `47 00 00 ff` ends the page |
| Between pages | Next page's language selector, or `ESC @` | Next page or job end |

This is a reader model of the known sequences, not a description of firmware
internals. Page bodies must be decoded using the current language and ink
mode. The initial reset does not mean that arbitrary mid-payload `1b 40`
bytes terminate the job. No mixed raster/JPEG job is established here; a
conservative sender keeps one encoding throughout a job.

## 3. Page settings

### `R`: resolution and print-mode codes

```text
52 code_1:u8 code_2:u8
```

The two parameters are mode codes, not literal dpi values. Known pairs are:

```text
00 00    11 00    02 00    11 01
01 01    02 01    02 02    03 03
02 03    02 04    03 04    03 06
```

Their selection depends on the printing mode as well as the requested quality.
A complete mapping from these codes to physical horizontal and vertical
resolution has not been established. In the captures below, the A6 raster
examples use `52 02 01`; the A6 JPEG example uses `52 03 04`.

### `I`: channel and symbol representation

```text
49 mode:u8
```

For raster data:

| Monochrome command | Color command | Bits per symbol | Quantization levels |
| --- | --- | ---: | ---: |
| `49 00` | `49 10` | 1 | 2 |
| `49 30` | `49 31` | 2 | 4 |
| `49 40` | `49 41` | 4 | 5 |

Symbols represent quantized ink output, not RGB intensity bytes. Four bits per
symbol do not imply that sixteen ink levels are used. Symbol layout, masking
and row ordering also depend on the printhead mode.

In JPEG mode, `49 30` selects monochrome output and `49 31` selects color
output. The JPEG payload must be decoded according to its own image headers,
not as packed raster symbols.

### `M`: raster compression

```text
4d method:u8
```

| Command | Encoding |
| --- | --- |
| `4d 08` | Zero runs followed by literal bytes |
| `4d 06` | Repeated patterns of one, two or four bytes |
| `4d 07` | PackBits-style byte runs |
| `4d 0c` | Range-coded swaths, compressed separately by channel |

`M 08` is used by all conventional raster captures in this document. The other
methods are described separately with their validation limits.

### `D`: bidirectional printing control

```text
44 mode:u8
```

The known values are `00` and `ff`. At page start, `00` permits bidirectional
printing and `ff` indicates a disabled or constrained bidirectional mode.
Media, quality and swath corrections can affect the choice. A `D` update may
appear immediately before a swath.

These values should not be interpreted as unconditional physical
left-to-right and right-to-left movements; that mechanical correspondence has
not been established.

### `N 04`: paper thickness

```text
4e 04 thickness:u8
```

| Value | Setting |
| --- | --- |
| `00` | Regular |
| `02` | Thick |
| `04` | Envelope |

### `N 05`: media type

```text
4e 05 media:u8
```

| Value | Known setting or interpretation |
| --- | --- |
| `00` | Plain paper, including plain duplex paper |
| `01` | Inkjet paper or Brother BP60 matte paper |
| `06` | Other glossy/photo paper |
| `07` | Brother BP71 photo paper |
| `0c` | Hagaki address side |
| `08` | Hagaki reverse side |
| `04`, `05`, `0d`, `0e` | Additional media selections; public setting correspondence unresolved |

Media selection can be constrained by duplex and quality settings. These are
wire values, not numeric option indices from a printer configuration interface.

### `T`: paper source

```text
54 source:u8
```

| Value | Source |
| --- | --- |
| `03` | Automatic selection |
| `01` | Tray 1 |
| `02` | Tray 2 |
| `06` | Multipurpose tray |
| `ff` | Manual feed selection; not exercised by the captures below |

### `s`: paper size

Predefined sizes use two bytes:

```text
73 size:u8
```

The following subset of size codes is known. The alternative codes correspond
to a rotated paper configuration; they should not be selected from an
application's Portrait/Landscape setting alone.

| Paper | Normal code | Rotated code, when distinct |
| --- | --- | --- |
| Letter | `00` | `2b` |
| A4 | `01` | `2a` |
| Legal | `02` | Same |
| Executive | `03` | `2c` |
| JIS B5 | `04` | `2d` |
| A5 | `07` | Same |
| A6 | `0b` | Same |
| A3 | `1e` | Same |
| Tabloid | `1d` | Same |
| JIS B4 | `1f` | Same |
| Folio | `21` | Same |
| 4 x 6 inch photo | `14` | Same |
| 5 x 8 inch index card | `13` | Same |
| Photo L | `18` | Same |
| Photo 2L (nominal 5 x 7 inch) | `19` | Same |
| Hagaki postcard | `11` | Same |
| Double Hagaki postcard | `1a` | Same |
| Com-10 envelope | `0c` | Same |
| DL envelope | `0a` | Same |
| C5 envelope | `06` | Same |
| Monarch envelope | `0d` | Same |
| You4 envelope | `15` | Same |

These associations are code-derived; the capture table exercises A6 and the
physical tests also exercise A4. Additional retained mappings are ISO B5
`05`, JIS B6 `08`, C4 envelope `12`, Chou3 `26`, Chou4 `27` and Kaku2 `28`.
Their presence in the encoder is not a statement that the MFC-J5720DW supports
every source/media/size combination. Retained disc and unnamed paper codes
are excluded from the supported model profile.

Borderless printing does not necessarily select a different `s` code. Other
page and swath parameters can change as well.

Custom sizes use six bytes:

```text
73 ff dimension_1:u16le dimension_2:u16le
```

The inferred unit is **1/300 inch**. In the known encoding, the unrotated order
is page height followed by page width; rotation exchanges the dimensions.
Intermediate dimension conversions can introduce rounding differences. Custom
sizes and their orientation conventions have not been validated by the
captures below. A bare `73 ff` does not provide a complete custom-size command.

For positive dimensions supplied in tenths of a millimetre, the known
conversion is `floor(dimension * 300 / 254)`. For example, height 2,970 and
width 2,100 produce 3,507 and 2,480 units:

```text
73 ff b3 0d b0 09
```

This is a serialization example, not a print-tested substitute for the
predefined A4 command. Some retained fallback paths emit a bare `73 ff` for
an unknown paper identifier; a new sender must reject that input rather than
treating the incomplete fallback as a valid custom-size command.

### `P 90`: nozzle range

```text
50 90 first_row:u16le row_count:u16le
```

These fields configure the nozzle/pass range. The A6 raster examples use
`50 90 01 00 a3 01`: first row 1 and a count of 419. The values are specific to
the printhead mode, not the height of the source image.

Brother's [published model specifications](https://support.brother.com/g/b/spec.aspx?c=gb&lang=en&prod=mfcj5720dw_us_eu_as)
list 420 nozzles for each of the four ink heads. This supports the scale of the
observed count, but does not establish that `first_row` is a physical nozzle
address or justify replacing the observed `1, 419` with `0, 420`.

### `S`: image-edge parameters

```text
53 left:s16le right:s16le top:s16le bottom:s16le
```

The inferred scale is **1/600 inch**. The four fields describe image edges or
margins, not the image's pixel width and height. Rotation, mirroring and row
alignment can change their values.

A special encoding uses `left=-5`, with the other three fields zero:

```text
53 fb ff 00 00 00 00 00 00
```

Its full firmware meaning is unresolved. It should not be treated as an
ordinary unsigned margin value.

Another borderless placement, used with an enlarged input image, subtracts
106 units (at 600 dpi) from all four calculated edges when the selected
nozzle/interlace mode enables borderless bounds. The resulting values can
all be negative; each is transmitted as a two's-complement 16-bit pattern.
This placement is distinct from the special `-5, 0, 0, 0` form above.
The input image is enlarged before encoding, while the paper-size command
continues to identify the physical sheet. These are host-side observations;
A4 paper tests confirmed full coverage and centered placement with an
additional host-side vertical correction; clipping on other stock sizes has
not been measured.

### `y`, `Y` and `X`

| Format | Known use |
| --- | --- |
| `79 flag:u8` | Paper-family flag; known values `00` and `01` |
| `59 00 00` | Y initialization |
| `58 00 00` | X initialization |

The page header writes Y before X. Only zero-valued X/Y initialization is
documented here; nonzero coordinate units and behavior are not established.
The exact firmware effect of the paper-family flag is also unresolved.

### `Up`, `Uq` and `Ur`: checksummed flags

| Format | Meaning |
| --- | --- |
| `55 70 02 00 flag check` | `Up`: color output flag, 1 for color, 0 otherwise |
| `55 71 02 00 flag check` | `Uq`: flag associated with single-pass operation |
| `55 72 02 00 flag check` | `Ur`: paper-control flag; precise meaning unresolved |

Each command is six bytes. The little-endian length `02 00` counts the flag and
checksum, excluding the four-byte header. The checksum is additive:

```text
check = -sum(all preceding bytes of this command) modulo 256
sum(all bytes including check) modulo 256 = 0
```

For example, color output enabled is `55 70 02 00 01 38`.
The firmware scope of the single-pass flag remains uncertain; it should not
be interpreted as a promise that the whole page requires one head movement.

## 4. Raster swaths and rows

A *swath* is a group of channel rows arranged for a printhead pass. Nozzle
positions, interlacing, masks and feed distances determine how source image
rows become swath rows.

A typical conventional swath sequence is:

```text
F feed_distance
[D direction_control]
69 76 54 32 01                 # ASCII "ivT2" followed by 01
[g symbol counters...]
[Z leading blank channel rows...]
G encoded channel rows and Z blank channel rows...
[Z trailing blank channel rows...]
```

An empty swath may be represented by feed commands alone. The fixed
`69 76 54 32 01` sequence introduces useful swath data; the meaning of each
individual byte within that sequence is not independently established.

### `F`: vertical feed

```text
46 distance:u16le
```

The inferred feed unit is **1/24,000 inch**. Large distances are split into
chunks of 32,767 units (`46 ff 7f`), followed by the remainder. A zero remainder
can still be sent as `46 00 00`.

This feed scale is distinct from the scale used for image-edge parameters,
custom paper sizes and source-image pixels.

### `G`: conventional raster row

```text
47 compressed_length:u16le payload[compressed_length]
```

The three-byte header is followed by exactly the declared number of bytes.
The payload uses the current `M` compression method.

The current sender encodes the full prepared byte width for a nonblank row.
Its trailing-zero scan detects empty rows and useful bounds; it does not
shorten the ordinary nonblank `G` payload before compression. A reader must
not infer the decoded row width from the compressed length. Acceptance of
arbitrary shortened nonblank rows is not established.

The prepared width depends on horizontal expansion and dot depth, and is
rounded to whole storage groups. Mirroring and printhead corrections must be
accounted for before assigning these symbols a physical position.

### `Z`: blank channel row

`5a` represents one blank channel row and has no arguments. It participates in
the swath row count just like a `G` record.

Color swaths normally have four channel records per nozzle-row position;
monochrome swaths have one. Leading, trailing and completely blank channels
all count. A `G` or `Z` record carries no explicit channel identifier: its
position in the swath determines its channel.

The code-derived logical plane order is **cyan, magenta, yellow, black**:
both halftone paths store planes in that order and both row schedules retain
it. Black-only rows come from the fourth logical plane. This describes the
sender's channel convention; an independent isolated-nozzle/color capture
has not established every physical head mapping. A grayscale document can
still use four ink planes, depending on its media and rendering mode. Follow
`I`, rather than the document's appearance, to determine the record count.

In particular, these records are not four interleaved RGB bytes. One swath
row is not necessarily one row of the original image.

### Packed symbols and swath accounting

Within each byte, symbols occupy the most significant positions first:

| Dot depth | Symbols in one byte | Example |
| --- | --- | --- |
| 1 bit | bits 7, 6, ..., 0 | `a0` represents `1,0,1,0,0,0,0,0` |
| 2 bits | bits 7..6, 5..4, 3..2, 1..0 | `1b` represents `0,1,2,3` |
| 4 bits | high nibble, then low nibble | `24` represents `2,4` |

Unused packed positions are zero. Five-level data uses symbols 0 through 4;
values 5 through 15 have no meaning in this profile. For mirrored output, the
zero-run path reverses symbol order, including byte order, without reversing
the bits inside a symbol. Thus the four two-bit symbols in `1b` become `e4`,
not a bitwise reversal to `d8`. The mechanical direction still depends on the
pass schedule and geometry.

For an ordinary nonempty swath, the known sender writes `P.row_count` groups
of one or four records, including leading and trailing `Z` records. The
swath marker has no length field and there is no separate end-of-swath byte;
record counts and page mode define the boundary. An all-blank pass can omit
the marker and rows entirely while still advancing paper with `F`.

## 5. Raster compression

### `M 08`: zero runs and literals

Each group starts with a control byte `h`:

```text
zero_count    = h & 0x1f       # 0 to 31
literal_count = h >> 5         # 0 to 7
```

Decode the group by appending `zero_count` zero bytes, then copying the next
`literal_count` bytes from the compressed payload. Repeat until the `G`
payload is exhausted. There is no additional end-of-row marker in the payload.

A decoder must reject a literal count that extends beyond the payload and
enforce a maximum decoded row size.

Worked example:

```text
Uncompressed row:  00 00 12 34 00 56
Compressed groups: 42 12 34 21 56
Complete command:  47 05 00 42 12 34 21 56

42: two zeros, then two literal bytes 12 34
21: one zero, then one literal byte 56
```

`1f` represents 31 zeros with no literals. `e0` introduces seven literals
with no preceding zeros. Blank ink symbols are zero, even though white source
pixels in an RGB image are `ff ff ff`.

For exact reproduction of the current sender, end a group immediately when
its zero count reaches 31; otherwise append at most seven consecutive nonzero
literals. It does not emit the no-output group `00`. A decoder following the
bit-field grammar can consume `00` as an empty group, but this is not evidence
that sending redundant empty groups to the printer is supported.

### `M 07`: PackBits-style encoding

| Control byte `h` | Decoding |
| --- | --- |
| `00` through `7f` | Copy the next `h + 1` bytes |
| `81` through `ff` | Repeat the next byte `257 - h` times |
| `80` | Behavior not established here |

The encoder uses the Gutenprint TIFF PackBits routine for this format and
does not emit `80`. The outer `G` uses a 16-bit compressed payload length;
there is no TIFF container or TIFF tag directory. This method is not exercised
by the printer-stream captures in section 10. PackBits conformance of the
host codec does not establish firmware behavior for unused control values.

### `M 06`: repeated patterns

| Control byte `h` | Decoding |
| --- | --- |
| `h < 0x80` | Copy the next `h + 1` bytes |
| `h >= 0x80`, `(h & 3) == 0` | Repeat the next one-byte pattern `((h >> 2) & 31) + 1` times |
| `h >= 0x80`, `(h & 3) == 1` | Repeat the next two-byte pattern the same number of times |
| `h >= 0x80`, `(h & 3) == 3` | Repeat the next four-byte pattern the same number of times |

The case `h >= 0x80` with `(h & 3) == 2` is unspecified here. Patterns are byte
sequences, not integers to byte-swap. Long repetitions are divided into groups.
The outer `G` again uses a 16-bit compressed payload length.

This grammar is derived from encoding behavior; it has not been validated by
the captures in section 10.

### `M 0c`: range-coded swaths

This method uses page-mode selection `1b 7c 01 01 0d`. It has different framing
from conventional raster rows.

Rows are padded to the required nozzle count, and their byte sizes are aligned
to 32-byte boundaries. Channels are compressed separately. The coding model
depends on the pixel count, row count, symbol depth and number of quantization
levels.

After the swath marker, the known envelope is:

| Format | Meaning |
| --- | --- |
| `62 position:u16le` (`b`) | Initial useful horizontal boundary, expressed in symbols |
| `6a position:u16le` (`j`) | Final useful horizontal boundary, expressed in symbols |
| `67 ...` (`g`) | Symbol counters |
| `65 framed_size:u32le raw_size:u32le` (`e`) | Header for each channel |
| `47 length:u24le payload[length]` | Compressed channel chunks, up to 4,092 payload bytes each |

`framed_size` includes the four-byte header of every following `G` chunk for
that channel. `raw_size` is the row count multiplied by the aligned row size
for one channel. The `b`/`j` boundaries account for mirroring. There is one
channel in monochrome and four in color.

The complete channel payload is reconstructed by concatenating its `G`
payloads, without their four-byte headers. Its code-derived prefix is:

| Offset | Field |
| ---: | --- |
| 0 | `L:u8`, the number of ordinary dot levels (2, 4 or 5) |
| 1 | `pixels:u32le`, the padded symbol count per row |
| 5 | `frequency[0..L]`, each `u32le` |
| `5 + 4*(L+1)` | `cumulative_end[0..L]`, each `u32le` |
| `5 + 8*(L+1)` | Arithmetic-coded data and finalization bytes |

Ordinary symbols are `0..L-1`. Symbol `L` replaces an entire all-zero row;
it is not an extra ink level. The model is static for one channel. Each
nonblank row contributes all its symbols to the frequency table; each blank
row contributes one occurrence of symbol `L`. The cumulative entries are
inclusive sums of those frequencies. The separately transmitted `g` counts
still count the actual zero dots in blank rows, so they differ from this
compression-model histogram.

The arithmetic engine starts with `range=0xffffffff` and `low=0`, subdivides
using integer frequency ratios, renormalizes below `0x01000000`, and finishes
with four bytes of `low`, **most significant byte first**, after pending carry
bytes. Thus the prefix integers are little-endian while the arithmetic tail
must not be byte-swapped as a sequence of little-endian words.

This specifies the envelope and model prefix, **not a complete interoperable
range-code decoder**: carry propagation, finalization and a decoder need
independent validation. The current implementation bounds its row count to
432; that is an implementation constraint, not a documented firmware limit.
This mode is not exercised by the printer-stream captures in section 10 and
is outside the recommended baseline sender profile.

## 6. Symbol counters

`g` records contain seven bytes:

```text
67 channel:u8 level:u8 count:u32le
```

Color counter groups use channel codes in the order **2, 3, 1, 0**. The sender's
logical mapping is:

| Raster plane position | Logical ink | Counter channel code |
| ---: | --- | --- |
| 0 | Cyan | `02` |
| 1 | Magenta | `03` |
| 2 | Yellow | `01` |
| 3 | Black | `00` |

Monochrome uses code `00`. This mapping follows the encoder and has the same
physical-validation limitation as the raster plane order in section 4.

For each channel, the level codes are:

| Symbol representation | Level codes |
| --- | --- |
| Two levels | `00`, `03` |
| Four levels | `00`, `01`, `02`, `03` |
| Five levels | `00`, `01`, `02`, `03`, `04` |

Level-zero counts include positions without ink and blank-row padding. These
are symbol counts, not simply the number of colored source-image pixels.
Whether counters appear in a conventional raster swath depends on its mode;
the current conventional-row path emits them for five-level output, giving
20 records for color and five for monochrome. The range-coded path emits them
before its channel envelopes for its selected symbol depth. They are not
present in every raster capture.

These are commands **sent to the printer**, not cartridge-level responses.
They do not report remaining ink percentages. Ink-supply queries use a
separate [SNMP query](#status-and-factory-calibration-channels), outside this
print-data stream.

## 7. JPEG data

### JPEG-specific settings

| Format | Meaning |
| --- | --- |
| `34 59 identifier NUL` (`4Y`) | Job identifier, up to 20 identifier bytes in the known encoding |
| `71 code_1:u8 code_2:u8` (`q`) | JPEG resolution-class codes |
| `70 width:u16le height:u16le` (`p`) | JPEG image dimensions in pixels |
| `49 30` or `49 31` (`I`) | Monochrome or color output selection |
| `68 flags:u8` (`h`) | Color-processing flags |

An empty identifier is `34 59 00`. The identifier's character encoding beyond
ASCII is not established.

Known `q` pairs are `00 00`, `01 01` and `02 02`. They are codes rather than
literal dpi values. The JPEG capture below uses `71 02 02` and carries a
600-dpi JFIF density value, but that example alone does not define all mappings.

The known `h` encoding is a bit field:

| Mask | Condition used by the sender |
| --- | --- |
| `01` | Slow-drying setting enabled |
| `02` | Vivid color matching selected |
| `04` | Color matching disabled |

Vivid and disabled matching are alternative selections, so the sender does
not combine bits `02` and `04`. The drying bit is independent, giving emitted
values `00`, `01`, `02`, `03`, `04` and `05`. All other bits are zero in the
known encoder. These associations do not specify the firmware's actual
color transformation. The capture below uses `68 02`.

### JPEG image profile

The standard image coding syntax is JPEG, identified by
[ITU-T T.81](https://www.itu.int/rec/T-REC-T.81-199209-I/en). The current sender
uses this more specific, code-derived profile:

| Property | Emitted profile |
| --- | --- |
| Frame coding | Baseline sequential DCT, 8-bit samples, Huffman coding |
| Components | Y, Cb, Cr, converted from RGB input |
| Sampling | 4:2:2: Y uses 2 x 1, Cb and Cr each use 1 x 1 |
| Quantization | Default luminance/chrominance tables scaled by 50 percent with baseline-compatible entries |
| Huffman tables | Default tables; no optimized entropy-table generation |
| Restart interval | Zero |
| Application marker | JFIF 1.00, density unit 1 (dots per inch) |
| Density | X and Y both use the selected rendering dpi |
| Adobe APP14 marker | Not emitted |

Monochrome selection still carries a three-component JPEG; `I 30` requests
monochrome output from the printer. A sender must keep `p` dimensions consistent
with the JPEG frame dimensions. Padding of partial JPEG blocks is part of the
JPEG codec and must not enlarge the dimensions declared in `p`.

This profile describes emitted images; it is not a list of all variants the
printer may accept. Progressive JPEG, arithmetic-coded JPEG, other sampling
ratios and grayscale JPEG payloads have not been established here. Different
JPEG implementations can produce different valid bytes for the same image;
byte-identical compressed output is not a requirement of this specification.

### Chunk framing

```text
47 length:u24le jpeg_fragment[length]
```

The current sender limits each fragment to **4,096 bytes**, an implementation
choice rather than a discovered maximum firmware capacity. Smaller fragments
occur in the captures; the chunks are not individually complete images.
Concatenating their payloads yields the page image from JPEG start marker
`ff d8` to JPEG end marker `ff d9`. A JPEG marker can straddle a chunk boundary.

Captured payloads include a JFIF header and decode with an independent JPEG
decoder after the Brother chunk headers are removed. This does not establish
support for every JPEG variant, color space or encoder configuration.

After the last image fragment, the page ends with:

```text
47 00 00 ff
```

Recognize this special marker **before accepting the length as a payload
size**. It does not request `0xff0000` bytes of JPEG data. It is distinct from
the `0c` terminator of a conventional raster page.

### The three meanings of `G`

| Context | Header | Payload |
| --- | --- | --- |
| Conventional raster | `47 length:u16le` | One encoded channel row |
| Range-coded raster (`M 0c`) | `47 length:u24le` | A fragment of a compressed channel |
| JPEG | `47 length:u24le` | A fragment of the page's JPEG image |

The current image mode determines the header width. Searching for `47` bytes
without tracking that mode and consuming each payload will misparse the stream.

## 8. Duplex printing

Two commands are added to each raster duplex page header:

| Format | Meaning |
| --- | --- |
| `32 53 mode:u8` (`2S`) | Duplex drying/quality mode |
| `32 52 side:u8` (`2R`) | Side selection |

The known `2S` values are:

| Value | Setting |
| --- | --- |
| `00` | Normal |
| `01` | Duplex 1, slower printing |
| `02` | Duplex 2, slower printing with reduced ink use |

The parameter is a mode code, not a time in seconds.

In ordinary automatic duplex, `2R` alternates `01`, `02`: odd-numbered pages
in the transmitted stream use `01`, even-numbered pages use `02`. These numbers
refer to stream order, not original document page numbers.

Both long-edge and short-edge binding use that alternating sequence. `2R` does
not directly encode the binding edge; image orientation and page geometry
also determine the result. Additional side values `03`, `04` and `ff` are
known from encoding analysis, but their use is not established for ordinary
automatic duplex and is not validated by the captures here.

### Reverse order and an odd number of pages

When the output stack requires reverse page order, pair document sides before
reversing them:

```text
Original document:      1, 2, 3
Pad the final sheet:    1, 2, 3, blank
Transmit in reverse:   blank, 3, 2, 1
2R side values:         01,    02,01,02
Sheet pairs:            (blank, 3), (2, 1)
```

Appending the blank after reversal would produce `3, 2, 1, blank`, pairing
`(3, 2)` and `(1, blank)`. That places the blank beside the wrong document page.
Page pairing is a sender responsibility; changing the `2R` alternation alone
does not repair the document order.

## 9. Additional commands

The following formats were identified during earlier encoding analysis, but
are not part of the normal captured job sequences. The user/title/job/document
tags and duplex-delay setting are not emitted by the current native sender.
Their firmware effects and required conditions have not been validated here;
they are recorded as extensions, not required baseline commands.

| Purpose | Format |
| --- | --- |
| User tag | `55 60 length:u16le text NUL check` |
| Title tag | `55 61 length:u16le text NUL check` |
| Job number | `55 62 05 00` + four ASCII digits + `check` |
| Document tag | `55 63 length:u16le text NUL check` |
| Duplex delay setting | `32 44 f4 01` |
| Feed-completion-related marker | `74` (`t`) |

For text tags, `length` includes the NUL terminator and checksum, but excludes
the four-byte header. The checksum follows the same additive rule as
`Up`/`Uq`/`Ur`. Non-ASCII character encoding and maximum accepted text lengths
are not established.

The delay bytes `f4 01` represent 500 as a little-endian value; the time unit
is unknown. The `t` marker does not establish a per-chunk acknowledgement
exchange. No response or completion-wait protocol is specified here.
The current sender disables the feed-completion feature, so a new baseline
implementation need not send `t` or wait for an undocumented reply to it.

### Inkjet maintenance jobs

Head cleaning and the internal print-quality sheet use a separate PJL stream.
Static analysis of the macOS CUPS command filter and status utility for this
model establishes this framing:

```text
00 repeated 4096 times
1b 25 2d 31 32 33 34 35 58       UEL: ESC %-12345X
one ASCII command ending in CR LF
1b 25 2d 31 32 33 34 35 58       UEL: ESC %-12345X
```

The displayed line breaks separate components; they are not additional bytes.
There is no extra PJL greeting and no trailing NUL in the CUPS filter's stream.

| Operation | ASCII command before CR LF |
| --- | --- |
| Internal nozzle / print-quality sheet | `@PJL EXECUTE PINCHKPRINT` |
| Normal head cleaning | `@PJL EXECUTE NORMALPURGE=ALL` |
| Normal black-head cleaning | `@PJL EXECUTE NORMALPURGE=BLACK` |
| Normal color-head cleaning | `@PJL EXECUTE NORMALPURGE=COLOR` |
| Strong head cleaning | `@PJL EXECUTE POWERPURGE=<inks>` |
| Strongest head cleaning | `@PJL EXECUTE SUPERPOWERPURGE=<inks>` |

The vendor's CUPS cleaning path uses `ALL`. Its separate maintenance UI also
selects `BLACK` and `COLOR`, and offers stronger `POWERPURGE` and
`SUPERPOWERPURGE` commands with the same ink selections. `<inks>` is exactly
`ALL`, `BLACK` or `COLOR`. All three strengths use the same framing. The maintenance
jobs do not contain raster or BCM data.

If several maintenance operations are requested, each has its own complete
4,096-zero/UEL/command/UEL envelope. These padding bytes differ from the 4,608
zeros of an image job. Preserve case, spaces and the final `0d 0a` in the PJL
line. Sending these commands can start a physical cleaning or print operation;
they are not capability queries.

No remote alignment cycle was established from the inspected Windows and macOS
drivers. The Windows vertical-line controls modify local printing preferences
grouped by paper size. They do not establish a command to print alignment
patterns or write calibration results to the printer. Those remote operations
and their parameters remain unknown.

These bytes were recovered from vendor code, not a physical maintenance
capture. Successful stream delivery does not by itself indicate that firmware
has completed the requested operation. Ink estimates use the independent
read-only query described below.

### Status and factory-calibration channels

Supply levels and factory calibration are retrieved through separate SNMP
GET operations. They are not replies to the outbound `G`, `g` or `U` commands.
The following summary is code-derived and backed by recorded query replies.
It describes the relevant objects and value formats, rather than a complete
specification of SNMP transport or calibration-table traversal.

| Data | SNMP object | Value encoding |
| --- | --- | --- |
| Ink estimates | `1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.8.0` | Binary OCTET STRING: seven-byte records, then `ff` |
| Factory calibration | Subtree `1.3.6.1.4.1.2435.2.3.9.4.2.1.5.1.2.67` | Integer indices/counts and OCTET STRING chunks containing ASCII hexadecimal text |

In an ink record, `field:u8 01 04 value:u32be`, fields `6f`, `70`, `71` and
`72` represent black, cyan, magenta and yellow, respectively. Values are
hundredths of a percent in the range 0..10,000. The `ff` terminator is at a
record boundary; it must not terminate parsing when encountered inside a
four-byte value. Missing or malformed fields mean unavailable information,
not zero remaining ink.

For each calibration chunk, decode ASCII hexadecimal to bytes, verify that
the final byte equals the sum of the preceding decoded bytes modulo 256,
and remove that byte before concatenation. This **positive** checksum is
different from the additive inverse used in `U` commands. The known complete
snapshot is 6,033 bytes: 5,052 nozzle-sample bytes, 893 parameter bytes and
88 density bytes. The full interpretation of these calibration arrays is
outside this command-stream specification. A calibration read does not start
alignment or write new calibration to the device.

## 10. Annotated captures

These examples are captured outbound print-data streams. They illustrate byte
framing and parameter values, not printer responses. Offsets are decimal and
start at the first byte of the job.

These are retained reference captures. Their offsets and compressed image
bytes describe those captures, not a promise that every current encoder or
JPEG library will reproduce the same chunk boundaries. New encoders must
preserve the framing and decoded meaning, rather than copying payloads or
absolute offsets from these examples.

### A6 color raster, four levels

| Offset | Bytes | Interpretation |
| ---: | --- | --- |
| 0 | `00` repeated 4,608 times | Initial padding |
| 4608 | `1b 7c 01 00 0d` | Raster mode |
| 4613 | `1b 40` | Reset |
| 4615 | `1b 7c 01 00 0d` | Raster page header |
| 4620 | `52 02 01` | Resolution/print-mode codes |
| 4623 | `49 31` | Color, four levels |
| 4625 | `4d 08` | Zero-run compression |
| 4627 | `44 00` | Bidirectional printing permitted at page start |
| 4629 | `4e 04 00 4e 05 00` | Regular thickness, plain paper |
| 4635 | `50 90 01 00 a3 01` | First row 1, 419 pass rows |
| 4641 | `54 03 73 0b` | Automatic source, A6 |
| 4645 | `53 4a 00 4c 00 4b 00 4b 00` | Edge parameters 74, 76, 75, 75 |
| 4654 | `79 00` | Paper-family flag zero |
| 4656 | `55 70 02 00 01 38` | Color enabled, checksum `38` |
| 4662 | `55 71 02 00 00 38` | `Uq=0` |
| 4668 | `55 72 02 00 00 37` | `Ur=0` |
| 4674 | `59 00 00 58 00 00` | Y, then X |
| 4680 | `00` repeated 4,608 times | Page padding |
| 9288 | `49 31` | Color mode repeated |
| 9290 | `46 ff 7f 46 f1 1b` | Feed 32,767 + 7,153 = 39,920 units |
| 9296 | `69 76 54 32 01` | Swath marker |
| 9301 | `47 b9 00` + 185 payload bytes | First compressed channel row |

The first payload begins `1f e9 44 51 44 51 44 51 44`: 31 zeros, followed by
nine zeros and seven literal bytes. The complete stream contains 320 `G`
rows and 1,356 `Z` rows, totaling 1,676 records = 419 rows x four channels.
It ends with final feed commands followed by `0c 1b 40`.

### A6 color JPEG

| Offset | Bytes | Interpretation |
| ---: | --- | --- |
| 0 | `00` repeated 4,608 times | Initial padding |
| 4608 | `1b 7c 02 00 0d 1b 40` | JPEG mode and reset |
| 4615 | `1b 7c 02 00 0d` | JPEG page header |
| 4620 | `34 59 00` | Empty identifier |
| 4623 | `52 03 04 71 02 02` | `R` and JPEG class `q` |
| 4629 | `70 14 09 16 0d` | Image dimensions 2,324 x 3,350 pixels |
| 4634 | `49 31 44 00` | Color output and direction control |
| 4638 | `4e 04 00 4e 05 07` | Regular thickness, BP71 media |
| 4644 | `54 03 73 0b` | Automatic source, A6 |
| 4648 | `53 4a 00 4c 00 4b 00 4b 00` | Image-edge parameters |
| 4657 | `68 02 79 00` | Color processing and paper-family flag |
| 4661 | `59 00 00 58 00 00` | Y, then X |
| 4667 | `47 01 03 00` | JPEG chunk with 769 payload bytes |
| 4671 | `ff d8 ff e0 00 10 4a 46 49 46 ...` | JPEG start and JFIF header |

The stream contains 202 chunks, with payload sizes from 299 to 769 bytes.
Concatenated payloads total 154,868 bytes and decode as an RGB image measuring
2,324 x 3,350 pixels. The stream ends with:

```text
ff d9    47 00 00 ff    1b 40
JPEG end page end       job reset
```

### Capture coverage

Four streams were checked for command boundaries, lengths, padding and `U`
checksums. Zero-run payloads were expanded within their row boundaries, and
the JPEG image was decoded independently.

| Stream | Total bytes | Structural observations |
| --- | ---: | --- |
| A6 color raster, four levels | 67,773 | One page, 320 `G` rows, 1,356 `Z` rows, one swath |
| A6 color raster, five levels | 20,738 | One page, 84 `G` rows, 1,592 `Z` rows, 20 `g` counters |
| A6 color JPEG | 160,349 | One page, 202 JPEG chunks |
| Short-edge duplex raster | 259,102 | Two pages, `2R 01` followed by `2R 02` |

These checks establish consistency of the documented framing for those
streams. They do not establish physical-printer behavior for every command
or combination of settings.

## 11. Coverage and open questions

The most directly supported paths are conventional raster with `M 08` and
chunked JPEG with the settings shown in the captures. The following points
remain outside that capture validation:

- `M 06`, `M 07`, range-coded `M 0c`, custom sizes and additional metadata
  commands need separate validation.
- The exact firmware meanings of `ivT2`, `Ur`, `y`, some color-processing
  flags and special image-edge values remain incomplete.
- A complete resolution-code mapping, independently measured nozzle/color
  mapping and independent swath-scheduling specification are not provided.
- The effect of removing padding, reordering commands or omitting symbol
  counters is not established.
- Device replies, acknowledgement timing, error recovery, cancellation and
  cartridge-status queries are not specified by this outbound language.

Command framing alone does not reproduce raster rendering. Color conversion,
halftoning, symbol packing, printhead masks and swath scheduling must also
produce data appropriate to the selected mode. A new implementation should
validate complete streams and page geometry, as well as the framing of
individual commands.

## 12. Command reference index

Sizes below include the command bytes and fields shown, before any external
transport framing. `n` is the payload/text size stated in that row. **R** means
conventional raster, **A** means range-coded raster and **J** means JPEG.
**Extension** denotes retained analysis outside the baseline sender profile.

| Command | Size in bytes | Context | Reference |
| --- | ---: | --- | --- |
| `1b 40` (`ESC @`) | 2 | Job boundaries | Section 2 |
| `1b 7c language variant 0d` | 5 | R/A/J selection | Section 2 |
| `52 a b` (`R`) | 3 | R/A/J header | Section 3 |
| `49 mode` (`I`) | 2 | R/A/J header; raster updates | Section 3 |
| `4d method` (`M`) | 2 | R/A header | Sections 3 and 5 |
| `44 mode` (`D`) | 2 | R/A/J header; raster updates | Section 3 |
| `4e 04 thickness` | 3 | R/A/J header | Section 3 |
| `4e 05 media` | 3 | R/A/J header | Section 3 |
| `50 90 first:u16le rows:u16le` | 6 | R/A header | Section 3 |
| `54 source` (`T`) | 2 | R/A/J header | Section 3 |
| `73 predefined_size` (`s`) | 2 | R/A/J header; size must differ from `ff` | Section 3 |
| `73 ff first:u16le second:u16le` | 6 | Custom-size extension | Section 3 |
| `53 left:s16le right:s16le top:s16le bottom:s16le` | 9 | R/A/J header | Section 3 |
| `79 flag` (`y`) | 2 | R/A/J header | Section 3 |
| `59 value:u16le` (`Y`) | 3 | R/A/J header, value zero | Section 3 |
| `58 value:u16le` (`X`) | 3 | R/A/J header, value zero | Section 3 |
| `55 selector 02 00 flag check` (`Up/Uq/Ur`) | 6 | R/A header, selectors `70`/`71`/`72` | Section 3 |
| `46 distance:u16le` (`F`) | 3 | Raster feed | Section 4 |
| `69 76 54 32 01` (`ivT2` + `01`) | 5 | Raster swath marker | Section 4 |
| `47 n:u16le payload[n]` (`G`) | `3+n` | R row | Sections 4 and 5 |
| `5a` (`Z`) | 1 | R blank channel row | Section 4 |
| `62 position:u16le` (`b`) | 3 | A swath | Section 5 |
| `6a position:u16le` (`j`) | 3 | A swath | Section 5 |
| `65 framed:u32le raw:u32le` (`e`) | 9 | A channel header | Section 5 |
| `47 n:u24le payload[n]` (`G`) | `4+n` | A channel fragment or J image fragment | Sections 5 and 7 |
| `67 channel level count:u32le` (`g`) | 7 | Raster symbol counter | Section 6 |
| `34 59 text[n] 00` (`4Y`) | `3+n` | J header; `n <= 20` in the sender | Section 7 |
| `71 a b` (`q`) | 3 | J header | Section 7 |
| `70 width:u16le height:u16le` (`p`) | 5 | J header | Section 7 |
| `68 flags` (`h`) | 2 | J header | Section 7 |
| `47 00 00 ff` | 4 | J page end, not a data fragment | Section 7 |
| `32 53 mode` (`2S`) | 3 | Duplex header | Section 8 |
| `32 52 side` (`2R`) | 3 | Duplex header | Section 8 |
| `0c` (`FF`) | 1 | R/A page end | Sections 2 and 8 |
| `55 tag length:u16le text NUL check` | `4+length` | Extension; tags `60`/`61`/`63` | Section 9 |
| `55 62 05 00 digits[4] check` | 9 | Extension | Section 9 |
| `32 44 f4 01` | 4 | Extension | Section 9 |
| `74` (`t`) | 1 | Extension, disabled in the current sender | Section 9 |

Initialization NUL blocks and the separate PJL maintenance envelopes are
described in sections 2 and 9. They are not general-purpose alignment padding.

## 13. Implementation and validation requirements

### Baseline sender profile

An implementation targeting the established behavior should begin with:

1. A homogeneous raster job with the complete section 2 preamble and page
   headers, `M 08`, known paper/media settings and a matching pass schedule.
2. A separately selected simplex JPEG path using section 7's image profile
   and the Brother JPEG header, fragmentation and terminator.
3. Ordinary raster duplex using alternating `2R 01`/`2R 02`, complete sheet
   pairing and the appropriate host-side image orientation.

The sender must preserve blank channel rows and complete a page with the
correct mode-specific terminator. It should explicitly emit the settings for
each page and preserve unknown but observed fields from a verified mode.
There is no public firmware-version range for which all these settings have
been established. Unsupported media/mode combinations should be rejected or
mapped to an already verified combination, not constructed by combining
unrelated observed bytes.

M 06, M 07, M 0c, custom paper, alternate duplex sides, nonzero X/Y origins and
additional tags require separate device validation before extending this
baseline. The declarations here do not enable those modes automatically.

### Length and resource checks

| Field | Wire range | Known sender restriction or validation |
| --- | --- | --- |
| Conventional `G` length | 16-bit unsigned | Must equal the encoded row payload; decoded width is checked separately |
| JPEG `G` length | 24-bit unsigned | Up to 4,096 payload bytes emitted; `00 00 ff` is reserved for page end in this context |
| Range `G` length | 24-bit unsigned | Up to 4,092 payload bytes emitted |
| Range `e.framed` | 32-bit unsigned | Must equal the sum of that channel's `4+n` chunk sizes |
| Range `e.raw` | 32-bit unsigned | Padded row bytes times nozzle rows for one channel |
| `F` distance | 16-bit unsigned | Sender splits at 32,767 and emits a final remainder, even zero |
| `4Y` text | NUL-terminated | Sender writes at most 20 bytes before the NUL |
| JPEG `p` dimensions | 16-bit unsigned each | Nonzero and equal to JPEG frame dimensions; codec may impose a lower maximum |
| `S` edges | 16-bit two's-complement patterns | Negative bounds are meaningful; do not clamp them to zero |
| `U` payload length | 16-bit unsigned | Includes the checksum and any NUL terminator, excludes the four-byte header |

A reader must validate fixed fields and remaining input before every read.
It must bound decompressed output independently of compressed input size and
check arithmetic before allocations. A short header, short payload, invalid
checksum or unsupported compression value must produce an explicit diagnostic;
continuing at the next apparent opcode is unsafe because payloads can contain
all opcode bytes.

For strict interpretation of the documented profile, an unknown opcode or
subcommand ends parsing with its byte offset. There is no uniform length
prefix that would allow every unknown command to be skipped. Retaining an
opaque remainder for analysis is different from successfully decoding it.
Likewise, an unfinished job must not be repaired by blindly appending `FF` or
`ESC @`: the device may still be consuming a declared payload.

### Serialization examples

These are command-level examples, **not standalone printable jobs**. They are
useful for independently checking integer order, payload lengths and checksums.

| Purpose | Hexadecimal bytes | Expected interpretation |
| --- | --- | --- |
| Color flag enabled | `55 70 02 00 01 38` | Six bytes whose sum modulo 256 is zero |
| Color flag disabled | `55 70 02 00 00 39` | Same format, flag zero |
| Signed special bounds | `53 fb ff 00 00 00 00 00 00` | `(-5, 0, 0, 0)` |
| Nozzle range | `50 90 01 00 a3 01` | First 1, count 419 |
| Feed of exactly 32,767 units | `46 ff 7f 46 00 00` | Full chunk followed by the sender's zero remainder |
| Zero-run row | `47 05 00 42 12 34 21 56` | Decodes to `00 00 12 34 00 56` under `M 08` |
| Zero-run payload containing opcode bytes | `47 04 00 60 0c 47 1b` | Decodes to the three data bytes `0c 47 1b`; none is a command |
| PackBits two-byte run | `47 02 00 ff aa` | Decodes to `aa aa` under `M 07` |
| Two repetitions of a two-byte pattern | `47 03 00 85 12 34` | Decodes to `12 34 12 34` under `M 06` |
| JPEG page end | `47 00 00 ff` | Terminator in J mode; not a 16,711,680-byte fragment |

An independent reader should also reject a `G` header with a missing payload,
an `M 08` group declaring unavailable literal bytes, and a `U` command with a
changed checksum. Splitting a valid input at every possible transport-buffer
boundary must not change the decoded commands or concatenate adjacent pages'
JPEG images.

### Validation levels

Byte serialization checks, independent decompression and actual print tests
answer different questions. A compatible implementation should verify:

- Header order, fixed padding, lengths, endianness and `U` checksums.
- Row decompression, symbol ranges and swath channel/row accounting.
- JPEG dimensions, component sampling and complete SOI-to-EOI reconstruction.
- Document pairing for an odd duplex job, including the final blank side.
- Printed position, scale, full-sheet coverage and color-channel meaning.

The first four saved streams in section 10 provide framing evidence, not a
complete test suite for all hardware behavior. For example, A4 borderless
centering required a measured host-side correction; identical header syntax
alone did not establish correct physical placement.

## 14. Public references

The references below establish the stated standard concepts or comparison
points. None is treated as an official command specification for the
MFC-J5720DW's private raster/JPEG dialect. Descriptions above are independently
written; documents for other models do not extend this model's supported
command set.

| Reference | Used for |
| --- | --- |
| [Brother MFC-J5720DW specifications](https://support.brother.com/g/b/spec.aspx?c=gb&lang=en&prod=mfcj5720dw_us_eu_as) | Target hardware and 420-nozzle heads; not command-byte definitions |
| [Brother Laser Printer Technical Reference Guide, revision R, appendix C](https://www.officestationery.co.uk/documents/BA69340.pdf#page=472) (copy of the Brother publication) | Scope and syntax of old laser HBP, to distinguish it from this inkjet dialect |
| [Brother PT-E550W/P750W/P710BT Raster Command Reference, version 1.02](https://download.brother.com/welcome/docp100064/cv_pte550wp750wp710bt_eng_raster_102.pdf) | Related raster-command vocabulary; model-specific selectors are not reused |
| [Independent Brother HBP documentation project](https://sourceforge.net/projects/hbp-for-brother/) | Background on laser host-based printing; not a source of inkjet parameters |
| [RFC 1179](https://www.rfc-editor.org/rfc/rfc1179.html) | Outer LPD job/control protocol |
| [RFC 8010](https://www.rfc-editor.org/rfc/rfc8010.html) and [RFC 8011](https://www.rfc-editor.org/rfc/rfc8011.html) | Outer IPP encoding, document-format negotiation and job semantics |
| [ITU-T T.81](https://www.itu.int/rec/T-REC-T.81-199209-I/en) | Standard JPEG coding referenced by the encapsulated image profile |
| [Epson ESC/POS initialization](https://download4.epson.biz/sec_pubs/pos/reference_en/escpos/esc_atsign.html) and [graphics commands](https://download4.epson.biz/sec_pubs/pos/reference_en/escpos/gs_lparen_cl.html) | Comparison showing why a shared `ESC @` byte sequence is insufficient to identify a language |
