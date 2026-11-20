# Preserve the current encoder's symbols and compiler settings.
%global debug_package %{nil}
%global _build_id_links none
%global __brp_strip %{nil}
%global __brp_strip_comment_note %{nil}
%global __brp_strip_lto %{nil}
%global cupsfilterdir %{_prefix}/lib/cups/filter
%global ppddir %{_datadir}/cups/model/Brother
%global driverdatadir %{_datadir}/mfcj5720dw

Name:           mfcj5720dw-cups
Version:        0.3.0
Release:        1%{?dist}
Summary:        Direct CUPS Raster driver for the Brother MFC-J5720DW
License:        GPL-3.0-or-later AND GPL-2.0-or-later AND LicenseRef-Brother-Data
URL:            https://github.com/vivier/mfcj5720dw_cups
Source0:        %{name}-%{version}.tar.gz
# The encoder currently retains x86 floating-point and data-layout assumptions.
ExclusiveArch:  x86_64 %{ix86}
BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  cups
BuildRequires:  cups-devel
BuildRequires:  libjpeg-turbo-devel
BuildRequires:  binutils
BuildRequires:  file
BuildRequires:  groff-base
BuildRequires:  python3 >= 3.11
BuildRequires:  grep
BuildRequires:  coreutils
BuildRequires:  gtk4-devel >= 4.12
BuildRequires:  libdbusmenu-devel
BuildRequires:  net-snmp-devel
BuildRequires:  gettext
BuildRequires:  desktop-file-utils
Requires:       cups
Requires:       cups-filesystem
Requires:       cups-filters
Requires:       ghostscript
Requires(preun): cups
Requires(preun): grep
Requires(preun): coreutils

%description
Native CUPS Raster filter for the Brother MFC-J5720DW, with a generated PPD,
color profiles, ink-level reporting through the CUPS side channel, three
head-cleaning strengths and an internal nozzle-check page.
The encoder is linked directly into the filter; no Brother LPR executable
or wrapper package is needed. This is an independent, experimental driver.

Select Brother MFC-J5720DW CUPS when adding the printer.
Final package removal deletes local queues using its filters. Upgrades and
reinstalls preserve existing queues and settings.

%package monitor
Summary:        Desktop status monitor for the Brother MFC-J5720DW
License:        GPL-3.0-or-later
Requires:       %{name}%{?_isa} = %{version}-%{release}
# iconv loads HP Roman-8 dynamically, so ELF dependency scanning cannot see it.
Requires:       glibc-gconv-extra%{?_isa}
Requires:       sane-backends
# Brother's separately installed brscan4 backend is needed for scan-to-PC.
# Keep it optional so status monitoring also works without the external RPM.
Recommends:     brscan4
# GNOME needs a StatusNotifier host; other desktops provide their own tray.
Recommends:     (gnome-shell-extension-appindicator if gnome-shell)
Requires:       tesseract
Requires:       tesseract-langpack-eng
Requires:       tesseract-langpack-fra
Requires:       xdg-utils

%description monitor
GTK application displaying physical printer status, detailed faults and ink
levels. Includes a system tray icon, desktop notifications and
optional login startup. A maintenance window offers head cleaning at three
strengths for black, color or all heads, and an internal nozzle-check sheet.
CUPS ink levels are synchronized automatically through the selected queue.
Network monitoring uses SNMP queries independently of print jobs. Optional
scan-to-PC support registers this computer on the printer and receives its
scan-button requests. Documents are acquired using the separately installed
Brother brscan4 SANE backend, with PDF, JPEG, Tesseract OCR and email-composer
destinations.

%prep
%setup -q

%build
# Keep the Makefile's validated compiler and linker settings, including x87
# arithmetic. Distribution optimization/LTO flags are not used by this encoder.
%make_build LDFLAGS=""

%install
%make_install LDFLAGS="" PREFIX=%{_prefix} CUPSFILTERDIR=%{cupsfilterdir} \
    PPDDIR=%{ppddir} DATADIR=%{driverdatadir}

%check
# Inspect build artifacts without running the printing filters.
make check-static LDFLAGS=""
cmp build/rastertomfcj5720dw %{buildroot}%{cupsfilterdir}/rastertomfcj5720dw
cmp build/mfcj5720dw_ink %{buildroot}%{cupsfilterdir}/mfcj5720dw_ink
cmp build/commandtomfcj5720dw %{buildroot}%{cupsfilterdir}/commandtomfcj5720dw
cmp ppd/mfcj5720dw.ppd %{buildroot}%{ppddir}/mfcj5720dw.ppd

%preun
# Only final removal deletes queues; an upgrade or reinstall keeps them.
if [ "$1" -eq 0 ]; then
    for queue_ppd in /etc/cups/ppd/*.ppd; do
        [ -f "$queue_ppd" ] || continue

        # cupsFilter2 overrides all cupsFilter entries when present.
        filter_attribute=cupsFilter
        if LC_ALL=C grep -q '^\*cupsFilter2:' "$queue_ppd"; then
            filter_attribute=cupsFilter2
        fi
        filter_program='(%{cupsfilterdir}/)?(rastertomfcj5720dw|mfcj5720dw_ink|commandtomfcj5720dw)'
        if ! LC_ALL=C grep -Eq "^\*$filter_attribute:[[:space:]]*\"[^\"]*[[:space:]]$filter_program[[:space:]]*\"[[:space:]]*$" "$queue_ppd"; then
            continue
        fi

        queue_name=${queue_ppd##*/}
        queue_name=${queue_name%%.ppd}
        # Pin the local socket so client configuration cannot select a remote server.
        if ! timeout 30 /usr/sbin/lpadmin -h /run/cups/cups.sock -U root -x "$queue_name" < /dev/null; then
            printf 'Cannot remove CUPS queue %s; start local CUPS and retry removing %{name}.\n' "$queue_name" >&2
            exit 1
        fi
        printf 'Removed CUPS queue %s (Brother MFC-J5720DW CUPS).\n' "$queue_name"
    done
fi
exit 0

%files
%license LICENSE COPYRIGHT PROVENANCE.md
%{cupsfilterdir}/rastertomfcj5720dw
%{cupsfilterdir}/mfcj5720dw_ink
%{cupsfilterdir}/commandtomfcj5720dw
%dir %{ppddir}
%{ppddir}/mfcj5720dw.ppd
%dir %{driverdatadir}
%dir %{driverdatadir}/lut
%{driverdatadir}/lut/*.BCM
%{_mandir}/man1/rastertomfcj5720dw.1*
%{_mandir}/man1/mfcj5720dw_ink.1*
%{_mandir}/man1/commandtomfcj5720dw.1*

%files monitor
%{_bindir}/mfcj5720dw-monitor
%{_datadir}/applications/io.github.vivier.mfcj5720dw.monitor.desktop
%{_datadir}/locale/fr/LC_MESSAGES/mfcj5720dw-monitor.mo
%{_mandir}/man1/mfcj5720dw-monitor.1*

%changelog
* Thu Nov 19 2026 Laurent Vivier <laurent@vivier.eu> - 0.3.0-1
- Add an optional GTK status monitor with notifications and French translation.
- Package the monitor separately and decode printer-localized text, including HP Roman-8.
- Build the monitor as PIE for standalone RPM rebuilds with Fedora linker flags.
- Support custom paper sizes with validated geometry and tray/duplex constraints.
- Declare HBP support for driver matching and publish the printer protocol specification.

* Mon Nov 16 2026 Laurent Vivier <laurent@vivier.eu> - 0.2.0-1
- Add three head-cleaning strengths and the internal nozzle-check page as CUPS commands.
- Include French option, paper-size and ink-color labels in the generated PPD.
- Retrieve and cache per-printer factory calibration through the CUPS backend.
- Apply complete density banks and nozzle correction with per-page initialization.

* Wed Nov 11 2026 Laurent Vivier <laurent@vivier.eu> - 0.1.0-2
- Package the standalone native CUPS Raster driver and ink reporting filter.
- Include the generated PPD, color profiles, manuals and retained data notices.
- Remove dependent local queues on final removal and preserve them on upgrades.
- Use libjpeg-turbo for JPEG compression with Brother transport framing.
