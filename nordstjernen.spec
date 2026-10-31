#
# spec file for package nordstjernen
#
# Copyright 2026 Andreas Røsdal
#
# Nordstjernen is dual-licensed under the Nordstjernen Source License v1.0
# (NSL-1.0) or the GNU General Public License version 3 or later, at the
# recipient's option. The GPL option is a free, OSI-approved license, so the package
# can be distributed under it, including in openSUSE:Factory.
#


Name:           nordstjernen
Version:        1.0.39
Release:        0
Summary:        Small, hand-written GTK web browser
License:        LicenseRef-NSL-1.0 OR GPL-3.0-or-later
Group:          Productivity/Networking/Web/Browsers
URL:            https://nordstjernen.org
ExcludeArch:    %ix86 %arm

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  hicolor-icon-theme
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  pkgconfig(enchant-2)
BuildRequires:  pkgconfig(gstreamer-1.0) >= 1.20
BuildRequires:  pkgconfig(gstreamer-app-1.0) >= 1.20
BuildRequires:  pkgconfig(gstreamer-video-1.0) >= 1.20
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(libcurl) >= 7.85
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(libavif)
BuildRequires:  pkgconfig(libjxl)
BuildRequires:  pkgconfig(libavcodec) >= 60
BuildRequires:  pkgconfig(libavformat) >= 60
BuildRequires:  pkgconfig(libavutil) >= 58
BuildRequires:  pkgconfig(libswresample) >= 4
BuildRequires:  pkgconfig(libswscale) >= 7
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(simde)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(uchardet)
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pangoft2)
Recommends:     mpv
Recommends:     myspell-en_US
Requires(post): update-alternatives
Requires(postun): update-alternatives

%description
Nordstjernen is a clean-room web browser written from scratch in C, with a
GTK 4 user interface and a libcurl network stack. It is built to be small,
secure, and readable by a single person end to end.

  * A from-scratch HTML5, CSS, and JavaScript engine — no forked browser engine.
  * Each tab's engine runs in its own sandboxed process (seccomp + Landlock on
    Linux) behind an IPC and shared-memory framebuffer boundary.
  * No JIT, which keeps the JavaScript attack surface small.
  * WebGL 1/2 over OpenGL ES, switchable off in Settings.
  * No telemetry: it does not phone home and does not track the user.

%prep
%setup -q -c -T
top=$(find "%{_sourcedir}" -name meson.build 2>/dev/null \
      | awk '{ print length, $0 }' | sort -n | head -1 | cut -d' ' -f2-)
echo "DIAG: top meson.build = ${top:-<none>}"
echo "DIAG: %{_sourcedir} ="; ls -la "%{_sourcedir}"
if [ -z "$top" ]; then
    echo "DIAG: no meson.build under SOURCES; listing %{_builddir}:"
    ls -laR "%{_builddir}" | head -80
    exit 1
fi
cp -a "$(dirname "$top")"/. .
test -f meson.build

%build
%meson \
    -Dwebgpu=disabled
%meson_build

%install
%meson_install
mkdir -p %{buildroot}%{_sysconfdir}/alternatives
ln -sf %{_sysconfdir}/alternatives/open-browser-nordstjernen %{buildroot}%{_bindir}/open-browser-nordstjernen
touch %{buildroot}%{_sysconfdir}/alternatives/open-browser-nordstjernen

# The GTK browser statically compiles the engine; the embedding shared
# library and its development header are only needed by external embedders,
# not the browser app. Drop them so the package is a clean application,
# not a -devel library.
rm -f %{buildroot}%{_libdir}/libnordstjernen.so
rm -f %{buildroot}%{_includedir}/nordstjernen/libnordstjernen.h
rmdir %{buildroot}%{_includedir}/nordstjernen 2>/dev/null || :

%post
%{_sbindir}/update-alternatives --remove internet-browser %{_bindir}/nordstjernen \
    >/dev/null 2>&1 || :
%{_sbindir}/update-alternatives --install %{_bindir}/open-browser-nordstjernen \
    open-browser-nordstjernen %{_bindir}/nordstjernen 60

%postun
if [ ! -f %{_bindir}/nordstjernen ]; then
    %{_sbindir}/update-alternatives --remove open-browser-nordstjernen %{_bindir}/nordstjernen
fi

%files
%doc README.md
%license %{_datadir}/nordstjernen/License.md
%license %{_datadir}/nordstjernen/COPYING
%{_bindir}/nordstjernen
%{_bindir}/open-browser-nordstjernen
%ghost %{_sysconfdir}/alternatives/open-browser-nordstjernen
%{_bindir}/nordstjernen-renderer
%{_bindir}/nordstjernen-audio
%{_datadir}/applications/org.nordstjernen.WebBrowser.desktop
%{_datadir}/metainfo/org.nordstjernen.WebBrowser.metainfo.xml
%{_datadir}/nordstjernen/
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.gif
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen*.svg

%changelog
