// dependencies.js: checks the build tools and libraries a source build needs, and names the packages that provide what is missing.
'use strict';

const fs = require('fs');
const { spawnSync } = require('child_process');

const SUPPORTED_DISTRIBUTIONS = 'Ubuntu 24.04, Debian 13, Fedora 40, openSUSE Tumbleweed, Arch Linux, Alpine 3.21 or newer';

const MANAGERS = {
  apt: { name: 'Debian/Ubuntu', install: 'sudo apt install' },
  dnf: { name: 'Fedora/RHEL', install: 'sudo dnf install', pkgconfig: (module) => `'pkgconfig(${module})'` },
  zypper: { name: 'openSUSE', install: 'sudo zypper install', pkgconfig: (module) => `'pkgconfig(${module})'` },
  pacman: { name: 'Arch Linux', install: 'sudo pacman -S --needed' },
  apk: { name: 'Alpine', install: 'sudo apk add', pkgconfig: (module) => `pc:${module}` },
  brew: { name: 'Homebrew', install: 'brew install' },
};

const TOOLS = [
  { label: 'meson', probe: ['meson'], minimum: '1.0',
    apt: 'meson', dnf: 'meson', zypper: 'meson', pacman: 'meson', apk: 'meson', brew: 'meson' },
  { label: 'ninja', probe: ['ninja', 'samu'],
    apt: 'ninja-build', dnf: 'ninja-build', zypper: 'ninja', pacman: 'ninja', apk: 'samurai', brew: 'ninja' },
  { label: 'pkg-config', probe: ['pkg-config', 'pkgconf'],
    apt: 'pkgconf', dnf: 'pkgconf', zypper: 'pkgconf', pacman: 'pkgconf', apk: 'pkgconf', brew: 'pkgconf' },
  { label: 'tar', probe: ['tar'],
    apt: 'tar', dnf: 'tar', zypper: 'tar', pacman: 'tar', apk: 'tar' },
  { label: 'a C compiler (gcc or clang)', probe: ['cc', 'gcc', 'clang'],
    apt: 'build-essential', dnf: 'gcc', zypper: 'gcc', pacman: 'base-devel', apk: 'build-base' },
  { label: 'a C++ compiler (g++ or clang++)', probe: ['c++', 'g++', 'clang++'],
    apt: 'build-essential', dnf: 'gcc-c++', zypper: 'gcc-c++', pacman: 'base-devel', apk: 'build-base' },
];

const REQUIRED = [
  { module: 'glib-2.0', minimum: '2.80', apt: 'libglib2.0-dev', pacman: 'glib2', brew: 'glib' },
  { module: 'gtk4', minimum: '4.14', apt: 'libgtk-4-dev', pacman: 'gtk4', brew: 'gtk4' },
  { module: 'epoxy', apt: 'libepoxy-dev', pacman: 'libepoxy', brew: 'libepoxy' },
  { module: 'libcurl', minimum: '8.5', apt: 'libcurl4-openssl-dev', pacman: 'curl', brew: 'curl' },
  { module: 'libcrypto', apt: 'libssl-dev', pacman: 'openssl', brew: 'openssl@3' },
  { module: 'libssl', apt: 'libssl-dev', pacman: 'openssl', brew: 'openssl@3' },
  { module: 'uchardet', apt: 'libuchardet-dev', pacman: 'uchardet', brew: 'uchardet' },
  { module: 'libpsl', apt: 'libpsl-dev', pacman: 'libpsl', brew: 'libpsl' },
  { module: 'sqlite3', apt: 'libsqlite3-dev', pacman: 'sqlite', brew: 'sqlite' },
  { module: 'libwebp', apt: 'libwebp-dev', pacman: 'libwebp', brew: 'webp' },
  { module: 'libwebpdemux', apt: 'libwebp-dev', pacman: 'libwebp', brew: 'webp' },
  { module: 'zlib', apt: 'zlib1g-dev', pacman: 'zlib', brew: 'zlib' },
  { module: 'fribidi', minimum: '1.0.6', apt: 'libfribidi-dev', pacman: 'fribidi', brew: 'fribidi' },
  { module: 'harfbuzz', minimum: '8.3.0', apt: 'libharfbuzz-dev', pacman: 'harfbuzz', brew: 'harfbuzz' },
  { module: 'fontconfig', minimum: '2.15.0', apt: 'libfontconfig-dev', pacman: 'fontconfig', brew: 'fontconfig' },
  { module: 'freetype2', apt: 'libfreetype-dev', pacman: 'freetype2', brew: 'freetype' },
  { module: 'cairo', minimum: '1.18.0', apt: 'libcairo2-dev', pacman: 'cairo', brew: 'cairo' },
  { module: 'cairo-ft', minimum: '1.18.0', apt: 'libcairo2-dev', pacman: 'cairo', brew: 'cairo' },
  { module: 'libseccomp', linux: true, apt: 'libseccomp-dev', pacman: 'libseccomp' },
  ...[
    ['libavformat', '60'],
    ['libavcodec', '60'],
    ['libavutil', '58'],
    ['libswscale', '7'],
    ['libswresample', '4'],
  ].map(([module, minimum]) => ({
    module, minimum, linux: true, apt: `${module}-dev`, pacman: 'ffmpeg', brew: 'ffmpeg',
    note: 'FFmpeg 6.0 or newer; on Fedora/RHEL from RPM Fusion, on openSUSE from Packman',
  })),
];

const OPTIONAL = [
  { module: 'sdl2', feature: 'audio playback', apt: 'libsdl2-dev', pacman: 'sdl2', brew: 'sdl2' },
  { module: 'gstreamer-1.0', minimum: '1.20', unix: true, feature: 'H.264 video and AAC audio',
    apt: 'libgstreamer1.0-dev', pacman: 'gstreamer' },
  { module: 'gstreamer-app-1.0', minimum: '1.20', unix: true, feature: 'H.264 video and AAC audio',
    apt: 'libgstreamer-plugins-base1.0-dev', pacman: 'gst-plugins-base-libs' },
  { module: 'gstreamer-video-1.0', minimum: '1.20', unix: true, feature: 'H.264 video and AAC audio',
    apt: 'libgstreamer-plugins-base1.0-dev', pacman: 'gst-plugins-base-libs' },
  { module: 'poppler-glib', feature: 'viewing PDF files', apt: 'libpoppler-glib-dev', pacman: 'poppler-glib', brew: 'poppler' },
  { module: 'libbrotlidec', feature: 'WOFF2 web fonts', apt: 'libbrotli-dev', pacman: 'brotli', brew: 'brotli' },
  { module: 'enchant-2', feature: 'spell-checking', apt: 'libenchant-2-dev', pacman: 'enchant', brew: 'enchant' },
  { module: 'libavif', feature: 'AVIF images', apt: 'libavif-dev', pacman: 'libavif', brew: 'libavif' },
  { module: 'libjxl', feature: 'JPEG XL images', apt: 'libjxl-dev', pacman: 'libjxl', brew: 'jpeg-xl' },
  { module: 'libavcodec', minimum: '60', mac: true, feature: 'WebM video', brew: 'ffmpeg' },
];

function capture(command, args, env = process.env) {
  const result = spawnSync(command, args, { encoding: 'utf8', env });
  if (result.error || result.status !== 0) return null;
  return result.stdout.trim();
}

function versionAtLeast(found, minimum) {
  const a = found.split(/[.+~-]/).map((part) => parseInt(part, 10) || 0);
  const b = minimum.split('.').map((part) => parseInt(part, 10) || 0);
  for (let i = 0; i < b.length; i++) {
    if ((a[i] || 0) !== b[i]) return (a[i] || 0) > b[i];
  }
  return true;
}

function osRelease() {
  try {
    const text = fs.readFileSync('/etc/os-release', 'utf8');
    const field = (name) => (text.match(new RegExp(`^${name}="?([^"\\n]*)`, 'm')) || [])[1] || '';
    return { ids: `${field('ID')} ${field('ID_LIKE')}`.toLowerCase(), name: field('PRETTY_NAME') };
  } catch {
    return { ids: '', name: '' };
  }
}

function packageManager() {
  if (process.platform === 'darwin') return 'brew';
  const { ids } = osRelease();
  if (/debian|ubuntu/.test(ids)) return 'apt';
  if (/fedora|rhel|centos/.test(ids)) return 'dnf';
  if (/suse/.test(ids)) return 'zypper';
  if (/arch/.test(ids)) return 'pacman';
  if (/alpine/.test(ids)) return 'apk';
  return null;
}

function applies(entry) {
  if (entry.linux) return process.platform === 'linux';
  if (entry.unix) return process.platform !== 'darwin';
  if (entry.mac) return process.platform === 'darwin';
  return true;
}

function packageFor(entry, manager) {
  if (!manager) return null;
  if (entry.module && MANAGERS[manager].pkgconfig) return MANAGERS[manager].pkgconfig(entry.module);
  return entry[manager] || null;
}

function installCommand(entries, manager) {
  const packages = [...new Set(entries.map((entry) => packageFor(entry, manager)).filter(Boolean))];
  if (!packages.length) return [];
  const lines = [];
  let line = `  ${MANAGERS[manager].install}`;
  for (const name of packages) {
    if (line.length + name.length > 76) {
      lines.push(`${line} \\`);
      line = '     ';
    }
    line += ` ${name}`;
  }
  lines.push(line);
  return lines;
}

function checkTools(env) {
  const missing = [];
  const outdated = [];
  for (const tool of TOOLS) {
    const found = tool.probe.map((command) => capture(command, ['--version'], env)).find((out) => out !== null);
    if (found === undefined) missing.push(tool);
    else if (tool.minimum) {
      const version = found.split('\n')[0].trim();
      if (!versionAtLeast(version, tool.minimum)) outdated.push({ ...tool, found: version });
    }
  }
  return { missing, outdated };
}

function checkLibraries(entries, env) {
  const missing = [];
  const outdated = [];
  for (const entry of entries.filter(applies)) {
    const found = capture('pkg-config', ['--modversion', entry.module], env);
    if (found === null) missing.push(entry);
    else if (entry.minimum && !versionAtLeast(found, entry.minimum)) outdated.push({ ...entry, found });
  }
  return { missing, outdated };
}

function describe(entry) {
  const name = entry.module || entry.label;
  const wanted = entry.minimum ? ` ${entry.minimum} or newer` : '';
  return entry.found ? `${name} ${entry.found} (${name}${wanted} is needed)` : `${name}${wanted}`;
}

function report(tools, libraries, manager) {
  const lines = ['Nordstjernen is built from source on this system, and the build cannot start yet.'];
  const missing = [...tools.missing, ...libraries.missing];
  const system = osRelease().name;

  if (missing.length) {
    lines.push('', 'Not installed:');
    for (const entry of missing) {
      const provider = packageFor(entry, manager);
      lines.push(`  - ${describe(entry)}${provider ? `  (package ${provider.replace(/'/g, '')})` : ''}`);
    }
    const notes = [...new Set(missing.map((entry) => entry.note).filter(Boolean))];
    for (const note of notes) lines.push(`    ${note}`);
    if (manager) {
      lines.push('', `Install them${system ? ` on ${system}` : ''} with:`, '', ...installCommand(missing, manager));
    } else {
      lines.push('', 'Install the packages that provide these pkg-config modules and programs with your system\'s package manager.');
    }
  }

  if (tools.outdated.length) {
    lines.push('', 'Older than Nordstjernen needs:', ...tools.outdated.map((entry) => `  - ${describe(entry)}`));
    lines.push('', 'A newer meson can be installed for your user with:', '', '  pipx install meson', '', 'or: python3 -m pip install --user meson');
  }

  if (libraries.outdated.length) {
    lines.push('', `Libraries older than Nordstjernen needs${system ? ` on ${system}` : ''}:`);
    lines.push(...libraries.outdated.map((entry) => `  - ${describe(entry)}`));
    if (process.platform === 'darwin') {
      lines.push('', 'Update them with: brew update && brew upgrade');
    } else {
      lines.push(
        '',
        'These versions come with your distribution release, so installing packages will not fix them.',
        `Nordstjernen builds on ${SUPPORTED_DISTRIBUTIONS}.`,
      );
    }
  }

  if (missing.length || tools.outdated.length || process.platform === 'darwin') {
    lines.push('', 'Then reinstall: npm install -g nordstjernen-browser');
  }
  return lines.join('\n');
}

function optionalReport(libraries, manager) {
  const absent = [...libraries.missing, ...libraries.outdated];
  if (!absent.length) return null;
  const features = [...new Set(absent.map((entry) => entry.feature))];
  const lines = [
    `Building without ${features.join(', ')}: ${absent.map((entry) => entry.module).join(', ')} not found.`,
  ];
  const command = manager ? installCommand(absent, manager) : [];
  if (command.length) lines.push('To include them, install these first and reinstall:', ...command);
  return lines.join('\n');
}

function uncheckedReport(tools, manager) {
  const lines = [
    'Nordstjernen is built from source on this system, and the build cannot start yet.',
    '',
    'Not installed:',
    ...tools.missing.map((entry) => `  - ${describe(entry)}`),
    '',
    'Without pkg-config the libraries cannot be checked.',
  ];
  if (manager) {
    lines.push(
      `This installs the build tools and every library the build needs${osRelease().name ? ` on ${osRelease().name}` : ''}:`,
      '',
      ...installCommand([...tools.missing, ...REQUIRED.filter(applies)], manager),
    );
  }
  lines.push('', 'Then reinstall: npm install -g nordstjernen-browser');
  return lines.join('\n');
}

function preflight(env, { onWarning, onError }) {
  const manager = packageManager();
  const tools = checkTools(env);
  if (process.platform === 'darwin' && tools.missing.some((tool) => / compiler /.test(tool.label))) {
    onError('Nordstjernen is built from source on this Mac and needs a C and C++ compiler. Install the Xcode command line tools:\n\n  xcode-select --install\n\nThen reinstall: npm install -g nordstjernen-browser');
    return;
  }
  if (tools.missing.some((tool) => tool.label === 'pkg-config')) {
    onError(uncheckedReport(tools, manager));
    return;
  }
  const libraries = checkLibraries(REQUIRED, env);
  if (tools.missing.length || tools.outdated.length || libraries.missing.length || libraries.outdated.length) {
    onError(report(tools, libraries, manager));
    return;
  }
  const optional = optionalReport(checkLibraries(OPTIONAL, env), manager);
  if (optional) onWarning(optional);
}

module.exports = { preflight, capture };
