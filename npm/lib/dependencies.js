// dependencies.js: checks the build tools and libraries a source build needs, and names the packages that provide what is missing.
'use strict';

const fs = require('fs');
const { capture, locateMsys2 } = require('./system');

const MANAGERS = {
  apt: { install: 'sudo apt install' },
  dnf: { install: 'sudo dnf install', pkgconfig: (module) => `'pkgconfig(${module})'` },
  zypper: { install: 'sudo zypper install', pkgconfig: (module) => `'pkgconfig(${module})'` },
  pacman: { install: 'sudo pacman -S --needed' },
  apk: { install: 'sudo apk add', pkgconfig: (module) => `pc:${module}` },
  brew: { install: 'brew install' },
  msys: { install: 'pacman -S --needed' },
};

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

function systemName() {
  if (process.platform === 'win32') {
    const msys2 = locateMsys2();
    return msys2 ? `MSYS2 ${msys2.msystem} (${msys2.root})` : '';
  }
  return osRelease().name;
}

function packageManager() {
  if (process.platform === 'win32') return 'msys';
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
  if (entry.platforms) return entry.platforms.includes(process.platform);
  if (entry.linux) return process.platform === 'linux';
  if (entry.unix) return process.platform !== 'darwin' && process.platform !== 'win32';
  if (entry.mac) return process.platform === 'darwin';
  if (entry.windows) return process.platform === 'win32';
  return true;
}

function packageFor(entry, manager) {
  if (!manager) return null;
  if (manager === 'msys') {
    if (entry.msysBase) return entry.msysBase;
    const msys2 = locateMsys2();
    return entry.msys && msys2 ? `${msys2.packagePrefix}${entry.msys}` : null;
  }
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

function installInstructions(entries, manager, purpose) {
  const system = systemName();
  if (manager === 'msys') {
    return ['', `${purpose}, open the MSYS2 ${locateMsys2().msystem} terminal${system ? ` of ${locateMsys2().root}` : ''} and run:`, '', ...installCommand(entries, manager)];
  }
  if (!manager) {
    return ['', 'Install the packages that provide these pkg-config modules and programs with your system\'s package manager.'];
  }
  return ['', `${purpose}${system ? ` on ${system}` : ''}, run:`, '', ...installCommand(entries, manager)];
}

function reportedVersion(output) {
  const match = output.match(/\d+\.\d+(\.\d+)?/);
  return match ? match[0] : output.split('\n')[0].trim();
}

function checkTools(tools, env) {
  const missing = [];
  const outdated = [];
  for (const tool of tools.filter(applies)) {
    if ((tool.satisfiedBy || []).some((command) => capture(command, ['--version'], env) !== null)) continue;
    const found = tool.probe.map((command) => capture(command, ['--version'], env)).find((out) => out !== null);
    if (found === undefined) missing.push(tool);
    else if (tool.minimum) {
      const version = reportedVersion(found);
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

function heading(product) {
  return `${product.name} is built from source on this system, and the build cannot start yet.`;
}

function reinstall(product) {
  return ['', `Then reinstall: npm install -g ${product.packageName}`];
}

function report(product, tools, libraries, manager) {
  const lines = [heading(product)];
  const missing = [...tools.missing, ...libraries.missing];

  if (missing.length) {
    lines.push('', 'Not installed:');
    for (const entry of missing) {
      const provider = packageFor(entry, manager);
      lines.push(`  - ${describe(entry)}${provider ? `  (package ${provider.replace(/'/g, '')})` : ''}`);
    }
    const notes = [...new Set(missing.map((entry) => entry.note).filter(Boolean))];
    for (const note of notes) lines.push(`    ${note}`);
    lines.push(...installInstructions(missing, manager, 'To install them'));
  }

  if (tools.outdated.length) {
    lines.push('', 'Older than the build needs:', ...tools.outdated.map((entry) => `  - ${describe(entry)}`));
    if (manager === 'msys' || manager === 'brew') {
      lines.push('', manager === 'msys' ? 'Update MSYS2 with: pacman -Syu' : 'Update them with: brew update && brew upgrade');
    } else {
      for (const hint of new Set(tools.outdated.map((entry) => entry.upgrade).filter(Boolean))) lines.push('', ...hint);
    }
  }

  if (libraries.outdated.length) {
    const system = systemName();
    lines.push('', `Libraries older than the build needs${system ? ` on ${system}` : ''}:`);
    lines.push(...libraries.outdated.map((entry) => `  - ${describe(entry)}`));
    if (manager === 'msys') {
      lines.push('', 'Update MSYS2 with: pacman -Syu');
    } else if (manager === 'brew') {
      lines.push('', 'Update them with: brew update && brew upgrade');
    } else {
      lines.push(
        '',
        'These versions come with your distribution release, so installing packages will not fix them.',
        `${product.name} builds on ${product.supportedDistributions}.`,
      );
    }
  }

  if (missing.length || tools.outdated.length || manager === 'brew' || manager === 'msys') {
    lines.push(...reinstall(product));
  }
  return lines.join('\n');
}

function missingMsys2Report(product) {
  const prefix = 'mingw-w64-ucrt-x86_64-';
  const packages = [...product.tools, ...product.required]
    .filter(applies)
    .map((entry) => entry.msysBase || (entry.msys && `${prefix}${entry.msys}`))
    .filter(Boolean);
  const lines = [
    `On Windows, ${product.name} is built from source with MSYS2, which is not installed.`,
    '',
    'Install MSYS2 from https://www.msys2.org (the default location is C:\\msys64; set',
    'MSYS2_ROOT if you choose another), then open its UCRT64 terminal and run:',
    '',
  ];
  let line = '  pacman -S --needed';
  for (const name of [...new Set(packages)]) {
    if (line.length + name.length > 76) {
      lines.push(`${line} \\`);
      line = '     ';
    }
    line += ` ${name}`;
  }
  lines.push(line, ...reinstall(product));
  return lines.join('\n');
}

function uncheckedReport(product, tools, manager) {
  const lines = [heading(product), '', 'Not installed:', ...tools.missing.map((entry) => `  - ${describe(entry)}`)];
  lines.push('', 'Without pkg-config the libraries cannot be checked.');
  if (manager) {
    lines.push(...installInstructions(
      [...tools.missing, ...product.required.filter(applies)],
      manager,
      'To install the build tools and every library the build needs',
    ));
  }
  lines.push(...reinstall(product));
  return lines.join('\n');
}

function optionalReport(libraries, manager) {
  const absent = [...libraries.missing, ...libraries.outdated];
  if (!absent.length) return null;
  const features = [...new Set(absent.map((entry) => entry.feature))];
  const lines = [`Building without ${features.join(', ')}: ${absent.map((entry) => entry.module).join(', ')} not found.`];
  const command = manager ? installCommand(absent, manager) : [];
  if (command.length) lines.push('To include them, install these first and reinstall:', ...command);
  return lines.join('\n');
}

function preflight(product, env, { onWarning, onError }) {
  if (process.platform === 'win32' && !locateMsys2()) {
    onError(missingMsys2Report(product));
    return;
  }
  const manager = packageManager();
  const tools = checkTools(product.tools, env);
  if (process.platform === 'darwin' && tools.missing.some((tool) => tool.compiler)) {
    onError(`${product.name} is built from source on this Mac and needs a C and C++ compiler. Install the Xcode command line tools:\n\n  xcode-select --install\n${reinstall(product).join('\n')}`);
    return;
  }
  if (tools.missing.some((tool) => tool.label === 'pkg-config')) {
    onError(uncheckedReport(product, tools, manager));
    return;
  }
  const libraries = checkLibraries(product.required, env);
  if (tools.missing.length || tools.outdated.length || libraries.missing.length || libraries.outdated.length) {
    onError(report(product, tools, libraries, manager));
    return;
  }
  const optional = optionalReport(checkLibraries(product.optional, env), manager);
  if (optional) onWarning(optional);
}

module.exports = { preflight };
