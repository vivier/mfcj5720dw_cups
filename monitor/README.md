# Printer status monitor

`mfcj5720dw-monitor` is a GTK 4 application with English and French interfaces.
It complements CUPS/GNOME with independent network polling, four separate ink
bars, the printer's display message, detailed faults and desktop notifications.
It reads local CUPS queue state and up to 20 recent jobs owned by the current
user. It performs no printing, maintenance operation or queue modification.

Build with `make -C monitor`, or build the whole project normally. Dependencies
are GTK 4 (4.12 or newer), GLib, Net-SNMP, libcups, gettext, a C compiler and make.
The runtime also needs iconv's HP Roman-8 conversion module (Fedora's
`glibc-gconv-extra`, required by the monitor RPM).
The RPM build produces an optional `mfcj5720dw-cups-monitor` subpackage. The core
driver does not acquire a GTK dependency. `make WITH_MONITOR=0` builds only the
driver. Source, manual and Makefile are together in this directory; native
artifacts go to `../build/monitor`.

Launch **Brother MFC-J5720DW Monitor**, choose a queue or enter the physical
printer's IP address, and press **Apply**. The settings section also controls
notifications, monitoring after closing the window, and optional login startup.
Reopen it from the application menu; **Quit monitor** stops polling.
Printer addresses from JetDirect, LPD and IPP/IPPS queues are recognized.
DNS-SD service URIs and USB queues require a network address override.
An IPP URI pointing to a print server also needs the physical printer's address.
The read community defaults to `public` and can be changed in the window.
`--once` provides a display-independent diagnostic report.

## Status interpretation

Each poll uses a Net-SNMP session with a 1.5-second timeout and no retry, followed
by localization and bounded alert-table requests if the printer responds. CUPS uses a separate
local connection with a timeout. All network operations run in a worker thread;
only one poll can run at once. Changing the selection discards any older result.
Only read requests are issued. SNMPv2c is unencrypted, including its community.

| Information | OID |
| --- | --- |
| Printer state | `1.3.6.1.2.1.25.3.5.1.1.1` |
| Device state | `1.3.6.1.2.1.25.3.2.1.5.1` |
| Error bits | `1.3.6.1.2.1.25.3.5.1.2.1` |
| Display text | `1.3.6.1.2.1.43.16.5.1.2.1.1` |
| Brother status | `1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.6.0` |
| Maintenance/ink data | `1.3.6.1.4.1.2435.2.3.9.4.2.1.5.5.8.0` |
| Alert severity/description | `1.3.6.1.2.1.43.18.1.1.{2,8}.1` |
| Console/general localization index | `1.3.6.1.2.1.43.5.1.1.{10,2}.1` |
| Localization character set | `1.3.6.1.2.1.43.7.1.1.4.1.<index>` |

Error-bit numbering follows [RFC 2790](https://www.rfc-editor.org/rfc/rfc2790.html):
bit zero is the most significant bit of the first octet. Alert interpretation
follows [RFC 3805](https://www.rfc-editor.org/rfc/rfc3805.html). Ink values reuse
the driver's validated decoder, in hundredths of a percent; a missing or invalid
reply must not become a zero level.

Brother status identifiers were investigated in the original macOS monitor:
`MonitorInfo.plist` assigns this model the `INK15` status family;
`StatusMessage.strings` associates numeric codes with states. The implementation
uses independently written labels and no copied vendor executable or resource.
For example, 10001 is ready, 40000 is sleep, 40021 is an open cover, 40022 a jam,
and 70500 a full ink absorber. Unrecognized codes are not reported as healthy.

On the tested MFC-J5720DW, sleep returns HR idle, error byte `00`, Brother code
40000 and display text `Veille`. Its alert table also contains that same text
with severity -1 and location 40000. This is a normal status row, not a fault;
the implementation does not classify every alert-table entry as an error.

Display and alert strings are converted from the charset announced for their
respective localization to UTF-8, using GLib/iconv. On the tested French printer,
both indices are 1 and the charset is IANA MIBenum 2004, **HP Roman-8**.
For example `50 72 C1 74` means `Prêt`; interpreting these bytes as UTF-8 or
ISO-8859-1 corrupts the accent. The supported identifiers are ASCII (3),
ISO-8859-1 (4), UTF-8 (106) and HP Roman-8 (2004), following the
[IANA character-set registry](https://www.iana.org/assignments/character-sets/).
Unknown or malformed encodings retain valid UTF-8 characters and replace
undecodable bytes; CUPS job titles already use UTF-8 and are not transcoded.

## Notifications and limits

New faults and recovery are reported on transitions. Connectivity alerts require
two unsuccessful polls. A CUPS job is reported delivered only after observing
its transition from an active state to the explicit completed state. Cancellation
and failure are distinct. Disappearance from history never implies success.
All displayed CUPS completion messages mean **sent to printer**, not verified
physical output. The initial history does not generate delivery notifications.

Maintenance-related display text is shown as returned by the printer. There is
no verified per-operation completion identifier, so the application does not
invent cleaning completion or head-alignment progress. USB-only monitoring and
SNMPv3 are not implemented. An unavailable query clears the displayed live ink
measurements; stale values are not presented as current.

The driver and its print protocol are unaffected by the GUI.
