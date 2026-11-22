# Scan to PC

The MFC-J5720DW's front panel can request four host-side workflows: File, Image,
OCR and Email. The printer sends a button event to a registered computer; the
computer then acquires the pages from the scanner. It does not receive a PDF
upload from the printer. The monitor implements registration and reception and
uses Brother's separately installed 64-bit `brscan4` SANE backend for acquisition.

## Scanner setup

Install the **64-bit Scanner driver (brscan4)** from Brother's downloads for
this model: [RPM for Fedora](https://support.brother.com/g/b/downloadlist.aspx?c=us&lang=en&prod=mfcj5720dw_us_eu_as&os=127)
or [DEB for Debian](https://support.brother.com/g/b/downloadlist.aspx?c=us&lang=en&prod=mfcj5720dw_us_eu_as&os=128).
On Debian, install the downloaded `brscan4` DEB with `sudo apt install ./brscan4*.deb`.
The monitor package installs `scanimage`, Tesseract and its English/French language
packs, but does not bundle Brother's scanner library. The scanner driver is
optional for status monitoring and printing. The separate **Scan-key-tool**
(`brscan-skey`) is not needed: the monitor implements that function.

Configure a scanner with a unique name and the printer's literal IPv4 address.
For example, replace the address below with the printer's address:

```sh
sudo brsaneconfig4 -a name=MFC-J5720DW model=MFC-J5720DW ip=192.0.2.20
brsaneconfig4 -q
scanimage --formatted-device-list='%d %m%n'
```

If the scanner is already configured, check its name and IP instead of adding
a duplicate. Use `ip=`, not `nodename=`: the monitor currently matches the
literal IP address in Brother's configuration. It correlates the unique
configured name with the SANE device list, rather than assuming that `dev0`
or the first discovered scanner belongs to this printer.

## Registration and events

A temporary destination is an SNMPv1 SET to UDP 161, community `internal`, OID
`1.3.6.1.4.1.2435.2.3.9.2.11.1.1.0`, with an OCTET STRING value:

```text
TYPE=BR;BUTTON=SCAN;USER="Linux";FUNC=FILE;HOST=192.0.2.10:54925;APPNUM=5;DURATION=360;BRID=;
```

Four bindings to this OID register the four destinations in one request:

| FUNC | APPNUM | Result |
| --- | ---: | --- |
| IMAGE | 1 | JPEG page files; open the first image |
| EMAIL | 2 | PDF; open an email draft with an attachment |
| OCR | 3 | Searchable PDF only; open the PDF |
| FILE | 5 | Multipage PDF |

The advertised duration is 360 seconds; the monitor renews every 240 seconds.
The vendor tool renews after 270–330 seconds. On shutdown it stops renewing;
no network deregistration was found in its inspected shutdown/remove paths.
The monitor likewise lets the lease expire. It does not assume that duration
zero is a deletion command. Password-protected entries (`BRID`) are not exposed.
The UI limits names to the documented 15 safe ASCII characters; delimiters and
control characters are rejected. The analyzed Linux tool uses 16-byte legacy
names, which does not establish the firmware's supported display limit.

The printer sends an IPv4 UDP datagram to port 54925:

| Offset | Encoding |
| --- | --- |
| 0 | `02`, event family |
| 1–2 | Big-endian length (see the two observed conventions below) |
| 3 | `30`, event subtype |
| 4 onward | Semicolon-delimited fields |

The vendor's built-in example counts ASCII payload bytes. A physical
MFC-J5720DW request instead declared 123 for a 122-byte datagram containing
118 payload bytes: total datagram length plus one. No terminating NUL was
transmitted. The receiver accepts these two observed length conventions and
still validates every field and the actual packet bounds. Requiring the vendor
example's convention alone rejected real panel requests during initial testing.

The payload contains `TYPE=BR`, `BUTTON=SCAN`, the registered `USER`, `FUNC`,
`HOST`, `APPNUM`, plus numeric `REGID` and `SEQ`. Additional fields such as
`P1`–`P4` may appear. The vendor Linux tool ignores these parameters. The
monitor likewise does not derive scan options from these fields; it reads the
pending request's duplex settings separately over TCP, as described below. Unknown
well-formed fields do not become commands or file names. Neither the inspected
vendor receive path nor this implementation sends a UDP acknowledgement.

The receiver validates the actual source IPv4 address, complete frame length,
registration identity, function/application pairing and integer bounds. A
bounded two-minute cache of `(REGID, SEQ, FUNC)` suppresses repeated packets.
One acquisition runs at a time; extra requests while busy are discarded rather
than queued. A short two-second delay allows the panel to release the scanner.

## Acquisition and files

### Settings selected on the printer

Before acquisition, the monitor connects to TCP 54921, reads the scanner's
`+OK 200\r\n` greeting and sends `1b 4b 0a 80` (the scan-key settings query
found in `brscan4`). It closes this connection before SANE opens its own session.
This query does not start paper acquisition. At idle, the tested printer returns
a single `d0` byte; settings must be read after the panel sends a scan request.

A successful response begins with `30`, then a two-byte **little-endian**
payload length. The payload contains newline-separated `key=value` fields and
ends with `80`, included in that length. The monitor handles fragmented TCP
reads, bounds the payload to 1024 bytes, matches `F` to the requested destination,
rejects duplicate fields and stops on an unreadable or unknown duplex setting.
It never silently falls back to a locally forced duplex mode.

The following values were captured from this MFC-J5720DW after selecting each
option on its panel:

| Panel choice | D | E | Acquisition and output |
| --- | --- | --- | --- |
| Off | `SIN` | Ignored (`LON` in the capture) | One-sided acquisition; the printer selects the available input |
| Long edge | `DUP` | `LON` | Duplex feeder; retain the backend's page orientation |
| Short edge | `DUP` | `SHO` | Duplex feeder; rotate even-numbered pages 180 degrees |

The monitor has no local source or duplex selector. The printer selects the
physical input and its panel controls duplex. Old saved source preferences
are ignored and removed on the next settings save. Rotation happens on PNG
pages before PDF creation, OCR or opening
an image, so all destinations use the same orientation.

The inspected Brother backend's `EnableSKeyDuplex` option only detects `D=DUP`;
it does not interpret `E`. This implementation reads both fields without changing
the backend's global configuration. Other returned fields such as resolution,
color and paper size are not applied by this change: resolution and grayscale
still come from the monitor. The observed longer reply included
`F=FILE`, `D=DUP`, `E=SHO`, `R=200`, `M=CGRAY`, `P=A4`, `A=0`,
`T=PDF(Image)`, `W=0`, `G=0`; the simplex reply contained only `F`, `D` and `E`.

### SANE acquisition and output

Acquisition uses `scanimage` with the matching `brother4:net1;devN` device.
The monitor uses `Automatic Document Feeder(left aligned)` for one-sided scans
and `Automatic Document Feeder(left aligned,Duplex)` for two-sided scans,
according to the panel's choice. It uses `24bit Color[Fast]` or `True Gray`
and the locally selected resolution: 100, 150, 200, 300, 400, 600, 1200 or
2400 dpi, with 300 dpi as the default. The selector and settings validation
share one table so nonuniform values such as 150 and 1200 dpi are preserved.
The SANE feeder option allows batch
acquisition; on this model it does not force the physical input.

Direct PC-initiated WSD acquisition also worked during investigation, but
failed with an I/O error after a legacy Scan-to-PC panel request. The monitor
therefore uses the Brother backend for this workflow; `sane-airscan` is not
required.

The supported resolution list was read from `scanimage --help` with the
installed `brscan4` backend. It also advertises 4800 and 9600 dpi, which are
not offered here because they exceed this model's optical resolution.
Brother specifies 2400 × 2400 dpi on the glass and 2400 × 1200 dpi in the
feeder; a selectable value alone does not establish native optical detail
at that resolution for every input or successful physical acquisition.

`scanimage` acquires PNG pages. In two crossed physical tests with the former
source selector, a feeder request scanned the glass when the feeder was
empty, and a flatbed request scanned a document loaded in the feeder. Static
analysis of the backend's protocol-10 scan command found duplex and alignment
parameters, but no command that forces glass versus feeder. The log's
`Requested SANE source` line records the requested option, not a measured
physical source.

All jobs use batch acquisition until the scanner reports no more documents,
including a single page from the glass when the feeder is empty. The former
flatbed preference and its one-page limit are no longer used. A no-document
result after at least one acquired page marks the end of the batch; other
acquisition errors remain errors even if some pages were saved.
Jobs are limited to 500 pages; reaching that limit requires checking the
feeder and is not reported as an unqualified success. Each job has a 20-minute
timeout and can be cancelled in the window.

Cairo builds a PDF with physical page sizes derived from pixel dimensions and
resolution. Tesseract produces only a searchable PDF for OCR and the monitor
opens that PDF in the default viewer; no separate text file is saved. The default
languages are `fra+eng`. The monitor invokes programs with argument arrays,
without a shell. It opens the system mail composer for Email, with no recipient
and no automatic sending. A configured desktop email application is required.

Every job creates a new mode-0700 subdirectory in the selected output folder.
Its name combines the local timestamp with a random suffix, so existing scans
are never overwritten. Only final documents and a diagnostic `scan.log` are
retained. PNG pages are temporary working images: they are removed after success,
acquisition/conversion failure or cancellation, including incomplete pages and
rotation files. Quitting the monitor waits for the acquisition worker to finish
its cleanup. Image output saves one JPEG per page at quality 95 and opens the
first one. A failed or cancelled job is not reported as successful.
The **Open last scan folder** button provides access to final output and the log.
`scan.log` records the panel settings, requested SANE source and rotation.

## Network and session requirements

The proposed display name is the system hostname followed by the login name,
separated by ` de ` (for example `quad de laurent`). The complete string is
truncated to its first 15 characters, without shortening either part separately.
A saved name
overrides this default. Confirming an empty or spaces-only name restores,
displays and saves the default, without resetting other preferences.
Spaces, dots, hyphens and underscores are also accepted in manually chosen names.
Brother's [DeviceSelector instructions](https://support.brother.com/g/b/faqend.aspx?c=us_ot&faqid=faq00003187_003&lang=en&pfs=1&prod=mfcj5720dw_us_eu_as)
document a 15-character display-name maximum. The monitor follows that supported
limit; no physical test established the firmware's behavior with longer names.
The analyzed Linux scan-key tool truncates its legacy registration name to
16 bytes, which by itself does not establish the printer's supported maximum.

The monitor runs as the logged-in user. Enable **Receive scans started on the
printer**, apply the settings, and keep it running; hiding the window is enough
when background monitoring is enabled. Login startup remains optional.

Allow inbound UDP 54925 from the printer and outbound UDP 161 / TCP 54921 to it.
For example, with firewalld, replace the example address with the printer IP:

```sh
sudo firewall-cmd --permanent --add-rich-rule='rule family="ipv4" source address="192.0.2.20" port port="54925" protocol="udp" accept'
sudo firewall-cmd --reload
```

The package does not change firewall rules. A VM behind NAT needs explicit
forwarding or a bridged interface. The optional PC address overrides the
advertised address, but cannot configure a router. IPv6-only scan-button
registration and USB scan-button events are not implemented. An existing
`brscan-skey` service or a second user session using UDP 54925 must be stopped
before another receiver can start.

This legacy protocol is unencrypted and does not cryptographically authenticate
requests. Peer and identity checks protect against unrelated traffic but do not
provide protection against address spoofing on an untrusted local network.

## Evidence and validation

Registration fields, function IDs, lease values, framing and the absence of an
ACK were established by static analysis of Brother's official
[Scan-key-tool 0.3.6](https://download.brother.com/welcome/dlf006650/brscan-skey-0.3.6-0.x86_64.rpm).
The registration scheme is also implemented by
[brotherscankeyd2](https://github.com/FrankAbelbeck/brotherscankeyd2/blob/main/bin/brotherscankeyd2).
No vendor executable or implementation source is distributed with this monitor.
Device configuration and enumeration were checked against the installed
64-bit `brscan4` 0.4.11-1 backend. The printer accepted the four-destination
registration in a live SNMP test. A physical **File → MFC-test → Start** request
then produced a one-page PDF from the flatbed with this backend on 19 November
2026. The printer and receiving PC were on the same local network.

Automated VM tests exercise malformed frames, peer filtering, replay protection,
registration failure/retry, cancellation and shutdown; synthetic pages exercise
real Cairo PDF generation and Tesseract OCR, including all four destinations.
Scanner selection tests cover IP/name matching and ambiguous configurations.
The File/flatbed workflow was physically validated from the panel on the host.
A second panel File request, received in the NAT-connected VM, produced a
two-page PDF using the duplex feeder and `brscan4` 0.4.11-2. That test required
UDP forwarding, a matching libvirt forwarding exception and the host's LAN IP
in the monitor's optional PC address field. Subsequent simplex feeder tests in
the VM produced searchable one- and two-page OCR PDFs and a readable colour
JPEG from an Image request. No PNG working images remained after these jobs.
A physical Email panel request also produced a one-page PDF. The clean VM
initially had no mail application, so opening the composer failed. After
installing Thunderbird, assigning it to `x-scheme-handler/mailto` and completing
account setup, replaying the desktop handoff with that PDF opened the draft
and attachment successfully, as confirmed by the user. No email was sent.
A subsequent physical Email request validated the complete workflow from the
printer panel to Thunderbird: one PDF page was produced, no PNG remained,
and the user confirmed that the draft with its attachment opened successfully.

Panel-control tests capture all three duplex choices on the printer and exercise
fragmented TCP replies, malformed lengths, cancellation and refusal at idle.
Synthetic-page tests check every RGB pixel after short-edge rotation for all
four destinations and verify that an unreadable panel setting prevents scanning.
A subsequent physical File request with **Short edge** selected produced a
two-page PDF in the VM; its log records `D=DUP`, `E=SHO`, duplex acquisition and
a 180-degree back-page rotation. Reading orientation against a marked original
remains a visual check for the user.

Primary references:

- [Brother MFC-J5720DW scanner specifications](https://support.brother.com/g/b/spec.aspx?c=fr&lang=fr&prod=mfcj5720dw_us_eu_as)
- [Brother Scan Key Tool configuration](https://support.brother.com/g/b/faqend.aspx?c=us_ot&faqid=faq00100714_000&lang=en&prod=mfcj5720dw_us_eu_as)
- [Brother brscan4 64-bit download](https://support.brother.com/g/b/downloadhowto.aspx?c=us&lang=en&prod=mfcj5720dw_us_eu_as&os=127&dlid=dlf105203_000&flang=4&type3=564)
- [SANE scanimage interface](https://sane-project.gitlab.io/website/man/scanimage.1.html)
- [Tesseract command-line usage](https://tesseract-ocr.github.io/tessdoc/Command-Line-Usage.html)
