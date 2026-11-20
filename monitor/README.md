# Printer status monitor

`mfcj5720dw-monitor` is a GTK 4 application with English and French interfaces.
It complements CUPS/GNOME with independent network polling, four vertical ink
bars, the printer's display message, detailed faults and desktop notifications.
It reads local CUPS queue state and up to 20 recent jobs owned by the current
user for notifications. View and manage the print queue in GNOME's printer
settings. It also supports scans started from the printer's **Scan to PC** menu.
Its Maintenance window submits head-cleaning and nozzle-check jobs through the
selected local CUPS queue. Ink-level synchronization runs automatically when
needed. The monitor preserves queue configuration and defaults.

Build with `make -C monitor`, or build the whole project normally. Dependencies
are GTK 4 (4.12 or newer), GLib, libdbusmenu-glib, Net-SNMP, libcups, gettext,
a C compiler and make.
The runtime also needs iconv's HP Roman-8 conversion module (Fedora's
`glibc-gconv-extra`, required by the monitor RPM).
Scanning uses `scanimage` with Brother's separately installed 64-bit `brscan4`
backend, configured for the printer's IPv4 address. Follow the
[scanner setup instructions](SCANNING.md#scanner-setup); `brscan-skey` is not
needed. Tesseract with the selected language packs provides OCR; `xdg-email`
opens the default mail composer.
The RPM build produces an optional `mfcj5720dw-cups-monitor` subpackage. The core
driver does not acquire a GTK dependency. `make WITH_MONITOR=0` builds only the
driver. Source, manual and Makefile are together in this directory; native
artifacts go to `../build/monitor`.

Launch **Brother MFC-J5720DW Monitor**, choose a queue or enter the physical
printer's IP address, and press **Apply**. The settings section also controls
notifications, monitoring after closing the window, and optional login startup.
An icon remains in the system tray while the monitor runs, including with
the window hidden or when started with `--background`. Activate the icon or
choose **Show monitor** from its menu to reopen the existing window.
**Quit monitor**, available in both the window and the icon's menu, stops
polling and scan reception. The icon disappears when the monitor exits.

The desktop must provide a StatusNotifierItem host. On GNOME, install and
enable the **AppIndicator and KStatusNotifierItem Support** extension:

```sh
sudo dnf install gnome-shell-extension-appindicator
gnome-extensions enable appindicatorsupport@rgcjonas.gmail.com
```

If GNOME does not recognize a newly installed extension, log out and back in
before enabling it. With this extension, a single click opens the menu and a
double-click opens the monitor directly. Other desktops may use a single
click for activation. Without a tray host, monitoring still works; reopen the
window from the application menu. The monitor RPM recommends the extension
when GNOME Shell is installed, but does not enable extensions automatically.

Printer addresses from JetDirect, LPD and IPP/IPPS queues are recognized.
DNS-SD service URIs and USB queues require a network address override.
An IPP URI pointing to a print server also needs the physical printer's address.
The read community defaults to `public` and can be changed in the window.
`--once` provides a display-independent diagnostic report.

Expand **Scan to this PC**, enable reception, choose the PC name, output folder,
resolution and OCR languages, then press **Apply**. Choose that PC from
the printer's File, Image, OCR or Email menu. File produces a multipage PDF;
Image saves JPEG pages and opens the first one; OCR saves only a searchable PDF
and opens it; Email opens a draft with a PDF attachment. No mail
is sent automatically. Output goes into a new private subdirectory for each job,
with the final documents and `scan.log` for diagnostics. PNG working images
are deleted after every job, including failure and cancellation. Image output
uses JPEG at quality 95. Quitting waits for acquisition cleanup to finish.

Click the current output folder to open GTK's folder chooser, select a folder,
then press **Apply** to save the choice. Cancelling the chooser keeps the
previous folder. The full path is available in the button's tooltip.

The proposed PC name combines the system hostname and login name with ` de `,
for example `quad de laurent`. The complete string is truncated to its first
15 characters, and unsupported characters become underscores. A saved name
takes precedence. Clear the name and press **Apply** to restore the default;
a name containing only spaces has the same effect. The restored name appears
in the field and is saved without changing the other scan settings.
You can edit it freely using ASCII letters, digits, spaces, dots, hyphens and
underscores.

Choose **Off**, **Long edge** or **Short edge** on the printer's duplex menu.
The monitor reads that choice for each request; both duplex modes select the
feeder, and short-edge scans rotate the back pages before PDF/OCR processing.
For one-sided scans, the printer automatically uses the feeder when loaded,
otherwise the glass. Acquisition continues until the available pages are
exhausted; a glass scan produces one page. Old source preferences are ignored
and removed when settings are saved. Resolution and grayscale still come from
the monitor. Unreadable panel settings stop acquisition instead of silently
choosing a mode.

Available resolutions are 100, 150, 200, 300, 400, 600, 1200 and 2400 dpi;
300 dpi remains the default. These values are exposed by the Brother backend.
The [optical limit](https://support.brother.com/g/b/spec.aspx?c=fr&lang=fr&prod=mfcj5720dw_us_eu_as)
depends on the physical input: 2400 × 2400 dpi on the glass
and 2400 × 1200 dpi in the feeder. The backend's 4800 and 9600 dpi choices
exceed both optical limits and are not offered by the monitor.

Scanning continues when the window is hidden if background
monitoring is enabled. **Quit monitor** stops reception and cancels acquisition.
The printer may retain the PC entry until its six-minute registration expires.

Allow incoming **UDP 54925 from the printer** in the PC firewall. Status-only
monitoring does not need this rule. Only one scan-button receiver can bind this
port: stop `brscan-skey` or another receiver before enabling this feature.
Launching the monitor again in the same desktop session opens the existing
instance. Different users can each monitor status, but only one can receive
scans on the PC at a time. Including the login name identifies the recipient;
it does not enable concurrent scan receivers.
A VM behind NAT needs a port forward or a bridged network; supplying a public
PC address alone does not create that forwarding rule. The address override is
for such routed setups. See [SCANNING.md](SCANNING.md) for the wire protocol,
security boundary and validation scope.

## Maintenance

Click **Maintenance** in the main window to open the maintenance window and
use the operations advertised by the selected print queue.
Choose **All print heads**, **Black** or **Colors**, then **Normal
cleaning**, **Strong cleaning** or **Strongest cleaning**. **Clean print heads…**
is on the same row as the head and strength selectors and shows the queue,
strength and ink group before confirmation. Stronger cleaning
uses more ink; the default confirmation button is Cancel.

**Print nozzle check** prints one internal print-quality sheet.
Printer status and ink bars refresh automatically every five seconds; no
refresh buttons are needed. The monitor compares the printer's ink readings
with CUPS supply information and submits a paperless `ReportLevels` job only
when the CUPS levels differ or are unavailable. Percentages use the same
rounding as the driver. The selected queue must be idle, accept jobs and support
this command. The printer must respond with valid ink levels, and its monitored
address must match the address derived from the queue.

At most one ink update is tracked at a time, with at least five minutes between
attempts for the selected queue, including after failure or a lost reply.
Ordinary five-second polls create no jobs when the levels already match.
Automatic supply queries do not generate document-delivery notifications.
One last-update time covers the displayed printer information, including ink
levels. Ink synchronization shows a temporary progress message, cleared on
confirmed completion, or an error if the job fails, is cancelled or cannot be
confirmed. The monitor queries that job directly, independently of the bounded
job history.

Under **Printer and notifications**, select a local CUPS queue using the
Brother MFC-J5720DW CUPS driver and press **Apply**. An IP address alone is
not sufficient for maintenance. Commands go to that queue's configured device,
including when a different address is used for status monitoring. Controls are
disabled when CUPS is unavailable, the queue rejects jobs or a command is not
advertised. Capabilities are checked again immediately before submission.

Each action creates one `application/vnd.cups-command` job, with one copy and
no change to printing defaults. Cleaning and nozzle-check submissions display
their job ID; follow them in the CUPS job list.
Acceptance or delivery does not mean the physical cleaning has finished; use
the printer state and its panel to follow that operation. A paused queue can
retain the job until resumed. Cleaning and nozzle-check submissions are never
retried automatically: check CUPS before sending an unconfirmed operation again.

The UI stays responsive while CUPS is contacted. Closing the maintenance window
keeps the monitor running; opening it again retains the cleaning selections.
Hiding the main window also hides maintenance. Quitting cancels an open confirmation dialog
or waits for an in-flight CUPS submission; it does not cancel a job already
accepted by CUPS or wait for the printer's physical operation to finish.
Alignment remains available on the printer's own maintenance menu.

## Status interpretation

Each poll uses a Net-SNMP session with a 1.5-second timeout and no retry, followed
by localization and bounded alert-table requests if the printer responds. CUPS uses a separate
local connection with a timeout. All network operations run in a worker thread;
only one poll can run at once. Changing the selection discards any older result.
Status polling issues only read requests; the GUI separately submits paperless
CUPS jobs when supply information needs updating. SNMPv2c is unencrypted,
including its community. Enabling scan reception separately performs SNMPv1 SET requests for
temporary scan destinations, using Brother's `internal` registration community.

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
CUPS completion notifications mean **sent to printer**, not verified
physical output. The initial history does not generate delivery notifications.

Maintenance-related display text is shown as returned by the printer. There is
no verified per-operation completion identifier, so the application does not
invent cleaning completion or head-alignment progress. USB-only monitoring and
SNMPv3 are not implemented. An unavailable query clears the displayed live ink
measurements; stale values are not presented as current.

The driver and its print protocol are unaffected by the GUI.
