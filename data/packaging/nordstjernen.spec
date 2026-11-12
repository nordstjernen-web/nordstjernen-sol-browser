Name:           nordstjernen
Version:        1.0.29
Release:        1%{?dist}
Summary:        Clean-room, hardened web browser written from scratch in C

License:        LicenseRef-NSL-1.0 OR GPL-3.0-or-later
URL:            https://github.com/nordstjernen-web/nordstjernen-sol-browser
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  rust >= 1.91
BuildRequires:  meson >= 1.0
%if 0%{?fedora} || 0%{?rhel}
BuildRequires:  ninja-build
%else
BuildRequires:  ninja
%endif
BuildRequires:  pkgconfig
BuildRequires:  cmake
BuildRequires:  pkgconfig(gtk4) >= 4.6
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  pkgconfig(libcurl) >= 7.85
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(uchardet)
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  glslang-devel
BuildRequires:  pkgconfig(libavcodec) >= 60
BuildRequires:  pkgconfig(libavformat) >= 60
BuildRequires:  pkgconfig(libavutil) >= 58
BuildRequires:  pkgconfig(libswresample) >= 4
BuildRequires:  pkgconfig(libswscale) >= 7
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pangoft2)

Recommends:     mpv

ExclusiveOS:    linux

%description
Nordstjernen is an independent, lightweight web browser built entirely
from scratch in C, using GTK 4 for the UI and libcurl for networking.
It is a clean-room implementation with no upstream browser engine: the
HTML parser (its own, in safe Rust), the JavaScript interpreter (QuickJS),
and the image decoder (Wuffs) are all integrated in-tree. The engine is a
hardened, zero-JIT HTML/CSS renderer aimed at secure general web
browsing, document reading, embedded systems, and embedding in other
applications. It does not phone home and does not telemeter the user.

%prep
%autosetup -n %{name}-%{version}

%build
# wgpu-native is not packaged.
%meson \
    -Dwebgpu=disabled
%meson_build

%install
%meson_install

# The browser statically compiles the engine; the embedding shared library
# and its header serve external embedders only, so this stays an application
# package rather than shipping a -devel surface.
rm -f %{buildroot}%{_libdir}/libnordstjernen.so
rm -f %{buildroot}%{_includedir}/nordstjernen/libnordstjernen.h
rmdir %{buildroot}%{_includedir}/nordstjernen 2>/dev/null || :

%files
%license %{_datadir}/nordstjernen/License.md
%license %{_datadir}/nordstjernen/COPYING
%doc README.md
%{_bindir}/nordstjernen
%{_bindir}/nordstjernen-renderer
%{_bindir}/nordstjernen-audio
%{_datadir}/applications/org.nordstjernen.WebBrowser.desktop
%{_datadir}/metainfo/org.nordstjernen.WebBrowser.metainfo.xml
%{_datadir}/nordstjernen/
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.gif
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen*.svg

%changelog
* Mon Oct 05 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.29-1
- Release 1.0.29.

* Sun Oct 04 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.28-1
- Release 1.0.28.

* Fri Oct 02 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.27-1
- Release 1.0.27.

* Sat Sep 26 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.26-1
- Release 1.0.26.

* Sun Sep 20 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.25-1
- Release 1.0.25.

* Wed Sep 16 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.24-1
- Release 1.0.24.

* Sat Aug 08 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.23-1
- Release 1.0.23.

* Fri Jul 31 2026 Andreas Røsdal <andreas.rosdal@gmail.com> - 1.0.22-1
- Release 1.0.22.
