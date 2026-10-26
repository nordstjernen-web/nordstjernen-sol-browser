// install.js: puts the Nordstjernen binary for this machine in place, from the notarised macOS release or a source build.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { Readable } = require('stream');
const { pipeline } = require('stream/promises');
const { version } = require('../package.json');

const REPO = 'nordstjernen-web/nordstjernen-sol-browser';
const RELEASE_URL = `https://github.com/${REPO}/releases/download/${version}`;
const SOURCE_URL = `https://github.com/${REPO}/archive/refs/tags/${version}.tar.gz`;
const STORE_URL = 'https://apps.microsoft.com/detail/9NW8T7W5Z4PL';

const BUILD_DEPENDENCIES = [
  'Nordstjernen is built from source on this system. Install the build dependencies first:',
  '',
  '  Debian/Ubuntu:',
  '    sudo apt install build-essential pkg-config meson ninja-build \\',
  '      libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev \\',
  '      libpsl-dev libsqlite3-dev libseccomp-dev libwebp-dev libsdl2-dev \\',
  '      libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev',
  '',
  '  Fedora/RHEL (ffmpeg-devel from RPM Fusion):',
  '    sudo dnf install gcc pkgconf meson ninja-build gtk4-devel libepoxy-devel libcurl-devel \\',
  '      openssl-devel uchardet-devel libpsl-devel sqlite-devel \\',
  '      libseccomp-devel libwebp-devel SDL2-devel ffmpeg-devel',
  '',
  '  openSUSE (ffmpeg-devel from Packman):',
  '    sudo zypper install gcc pkgconf meson ninja gtk4-devel libepoxy-devel libcurl-devel \\',
  '      libopenssl-devel libuchardet-devel libpsl-devel sqlite3-devel \\',
  '      libseccomp-devel libwebp-devel libSDL2-devel ffmpeg-devel',
  '',
  'Then reinstall: npm install -g nordstjernen-browser',
].join('\n');

class InstallError extends Error {}

function log(message) {
  process.stderr.write(`[nordstjernen-browser] ${message}\n`);
}

function cacheBase() {
  if (process.platform === 'darwin') return path.join(os.homedir(), 'Library', 'Caches');
  return process.env.XDG_CACHE_HOME || path.join(os.homedir(), '.cache');
}

function installRoots() {
  return [
    path.join(__dirname, '..', 'vendor', version),
    path.join(cacheBase(), 'nordstjernen-browser', version),
  ];
}

function buildFromSourceRequested() {
  return process.env.NORDSTJERNEN_BUILD_FROM_SOURCE === '1';
}

function installPlan() {
  if (process.platform === 'win32') return { kind: 'store' };
  if (process.platform === 'darwin' && process.arch === 'arm64' && !buildFromSourceRequested()) {
    return {
      kind: 'dmg',
      asset: `nordstjernen-${version}-macos-arm64.dmg`,
      exe: path.join('Nordstjernen.app', 'Contents', 'MacOS', 'Nordstjernen'),
    };
  }
  return { kind: 'source', exe: path.join('bin', 'nordstjernen') };
}

function storeMessage() {
  return [
    'On Windows, Nordstjernen is distributed through the Microsoft Store:',
    `  ${STORE_URL}`,
  ].join('\n');
}

function findInstalled(plan) {
  for (const root of installRoots()) {
    const exe = path.join(root, plan.exe);
    if (fs.existsSync(exe)) return exe;
  }
  return null;
}

function writableRoot() {
  for (const root of installRoots()) {
    try {
      fs.mkdirSync(path.dirname(root), { recursive: true });
      fs.accessSync(path.dirname(root), fs.constants.W_OK);
      return root;
    } catch {
      continue;
    }
  }
  throw new InstallError(`No writable install directory among: ${installRoots().join(', ')}`);
}

function run(command, args, options = {}) {
  const result = spawnSync(command, args, { stdio: 'inherit', ...options });
  if (result.error) throw new InstallError(`${command}: ${result.error.message}`);
  if (result.status !== 0) throw new InstallError(`${command} ${args.join(' ')} exited with ${result.status}`);
}

function hasCommand(command) {
  const result = spawnSync(command, ['--version'], { stdio: 'ignore' });
  return !result.error && result.status === 0;
}

async function download(url, destination) {
  log(`downloading ${url}`);
  const response = await fetch(url);
  if (!response.ok) throw new InstallError(`GET ${url}: HTTP ${response.status}`);
  await pipeline(Readable.fromWeb(response.body), fs.createWriteStream(destination));
}

async function expectedChecksum(asset) {
  const response = await fetch(`${RELEASE_URL}/SHA256SUMS`);
  if (!response.ok) throw new InstallError(`GET ${RELEASE_URL}/SHA256SUMS: HTTP ${response.status}`);
  for (const line of (await response.text()).split('\n')) {
    const [sum, name] = line.trim().split(/\s+\*?/);
    if (name === asset) return sum.toLowerCase();
  }
  throw new InstallError(`${asset} is not listed in the ${version} SHA256SUMS`);
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

async function installDmg(plan, staging, work) {
  const dmg = path.join(work, plan.asset);
  await download(`${RELEASE_URL}/${plan.asset}`, dmg);
  const expected = await expectedChecksum(plan.asset);
  if (sha256(dmg) !== expected) throw new InstallError(`${plan.asset} does not match its SHA256SUMS entry`);
  const mountpoint = fs.mkdtempSync(path.join(work, 'mount-'));
  run('hdiutil', ['attach', '-nobrowse', '-readonly', '-quiet', '-mountpoint', mountpoint, dmg]);
  try {
    run('ditto', [path.join(mountpoint, 'Nordstjernen.app'), path.join(staging, 'Nordstjernen.app')]);
  } finally {
    run('hdiutil', ['detach', '-quiet', mountpoint]);
  }
}

async function installSource(staging, work) {
  const missing = ['meson', 'ninja', 'pkg-config', 'tar'].filter((tool) => !hasCommand(tool));
  if (!['cc', 'gcc', 'clang'].some(hasCommand)) missing.push('a C compiler');
  if (missing.length) throw new InstallError(`Missing ${missing.join(', ')}.\n\n${BUILD_DEPENDENCIES}`);

  const tarball = path.join(work, `nordstjernen-${version}.tar.gz`);
  await download(SOURCE_URL, tarball);
  run('tar', ['-xzf', tarball, '-C', work]);
  const source = path.join(work, `nordstjernen-sol-browser-${version}`);
  const build = path.join(work, 'build');

  log(`building Nordstjernen ${version} from source; this takes several minutes`);
  try {
    run('meson', ['setup', build, source, '--buildtype=release', `--prefix=${staging}`]);
  } catch (error) {
    throw new InstallError(`${error.message}\n\n${BUILD_DEPENDENCIES}`);
  }
  run('meson', ['compile', '-C', build]);
  run('meson', ['install', '-C', build, '--quiet']);
}

async function install() {
  const plan = installPlan();
  if (plan.kind === 'store') throw new InstallError(storeMessage());
  const existing = findInstalled(plan);
  if (existing) return existing;

  const root = writableRoot();
  const staging = `${root}.partial-${process.pid}`;
  const work = fs.mkdtempSync(path.join(os.tmpdir(), 'nordstjernen-browser-'));
  fs.rmSync(staging, { recursive: true, force: true });
  fs.mkdirSync(staging, { recursive: true });
  try {
    if (plan.kind === 'dmg') await installDmg(plan, staging, work);
    else await installSource(staging, work);
    if (!fs.existsSync(path.join(staging, plan.exe))) {
      throw new InstallError(`the install did not produce ${plan.exe}`);
    }
    fs.rmSync(root, { recursive: true, force: true });
    fs.renameSync(staging, root);
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
    fs.rmSync(work, { recursive: true, force: true });
  }
  log(`installed Nordstjernen ${version} in ${root}`);
  return path.join(root, plan.exe);
}

module.exports = { install, installPlan, storeMessage, STORE_URL, InstallError, log };
