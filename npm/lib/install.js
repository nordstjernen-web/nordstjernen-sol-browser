// install.js: unpacks the release AppImage or notarised macOS build where the system can run it, otherwise builds the release from source with meson, and installs it inside the package.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { Readable, Transform } = require('stream');
const { pipeline } = require('stream/promises');
const product = require('./product');
const { preflight, versionAtLeast } = require('./dependencies');
const { write, locateMsys2, capture, buildEnvironment, runtimeEnvironment, buildPath, runLogged } = require('./system');

const RELEASE_URL = `https://github.com/${product.repo}/releases/download/${product.version}`;
const SOURCE_URL = `https://github.com/${product.repo}/archive/refs/tags/${product.version}.tar.gz`;
const SOURCE_CHECKSUM_FILE = path.join(__dirname, 'source.json');
const RUNTIME_FILE = 'npm-runtime.json';
const EXECUTABLE_DIRECTORIES = ['bin', 'libexec'];

class InstallError extends Error {}
class AppImageUnusable extends InstallError {}
class DownloadFailed extends InstallError {}

function log(message) {
  write(`[${product.packageName}] ${message}\n`);
}

function cacheBase() {
  if (process.platform === 'win32') return process.env.LOCALAPPDATA || path.join(os.homedir(), 'AppData', 'Local');
  if (process.platform === 'darwin') return path.join(os.homedir(), 'Library', 'Caches');
  return process.env.XDG_CACHE_HOME || path.join(os.homedir(), '.cache');
}

function installRoots() {
  return [
    path.join(__dirname, '..', 'vendor', product.version),
    path.join(cacheBase(), product.packageName, product.version),
  ];
}

function usesMacRelease() {
  return Boolean(product.macRelease) && process.platform === 'darwin' && process.arch === 'arm64'
    && process.env[`${product.envPrefix}_BUILD_FROM_SOURCE`] !== '1';
}

function executablePath() {
  return usesMacRelease() ? product.macRelease.executable : product.executable;
}

const NATIVE_LIBRARIES = {
  x64: {
    ldconfigTag: ',x86-64)',
    directories: ['/usr/lib64', '/lib64', '/usr/lib/x86_64-linux-gnu', '/lib/x86_64-linux-gnu'],
  },
};

function hostLibraryPresent(name, ldconfigLines, native) {
  if (ldconfigLines.some((line) => line.startsWith(`${name} `) && line.includes(native.ldconfigTag))) return true;
  return native.directories.some((directory) => fs.existsSync(path.join(directory, name)));
}

function linuxReleaseObstacle() {
  const release = product.linuxRelease;
  if (!release || process.platform !== 'linux') return 'unsupported';
  if (process.env[`${product.envPrefix}_BUILD_FROM_SOURCE`] === '1') return `${product.envPrefix}_BUILD_FROM_SOURCE=1`;
  if (process.arch !== release.arch) return `the release AppImage is built for ${release.arch}, not ${process.arch}`;
  const glibc = process.report.getReport().header.glibcVersionRuntime;
  if (!glibc) return 'the release AppImage needs glibc, and this system uses another C library';
  if (!versionAtLeast(glibc, release.glibc)) return `the release AppImage needs glibc ${release.glibc}, and this system has ${glibc}`;
  const native = NATIVE_LIBRARIES[release.arch];
  const cache = capture('ldconfig', ['-p']) || capture('/sbin/ldconfig', ['-p']) || '';
  const ldconfigLines = cache.split('\n').map((line) => line.trim());
  const absent = release.hostLibraries.filter((name) => !hostLibraryPresent(name, ldconfigLines, native));
  if (absent.length) return `the release AppImage needs these system libraries: ${absent.join(', ')}`;
  return null;
}

async function installLinuxRelease(staging) {
  const { asset, executable } = product.linuxRelease;
  const extraction = fs.mkdtempSync(`${staging}.extract-`);
  try {
    const image = path.join(extraction, asset);
    const expected = await releaseChecksum(asset);
    verify(await download(`${RELEASE_URL}/${asset}`, image), expected, asset);
    fs.chmodSync(image, 0o755);
    log('unpacking');
    const unpacked = spawnSync(image, ['--appimage-extract'], { cwd: extraction, stdio: 'ignore' });
    if (unpacked.error || unpacked.status !== 0) {
      throw new InstallError(`${asset} --appimage-extract failed${unpacked.error ? `: ${unpacked.error.message}` : ''}`);
    }
    fs.rmSync(staging, { recursive: true, force: true });
    fs.renameSync(path.join(extraction, 'squashfs-root'), staging);
  } finally {
    fs.rmSync(extraction, { recursive: true, force: true });
  }
  const probe = spawnSync(path.join(staging, executable), ['--version'], { encoding: 'utf8', timeout: 60000 });
  if (probe.error || probe.status !== 0) {
    const detail = (probe.stderr || '').trim().split('\n').slice(-3).join('\n');
    throw new AppImageUnusable(`the release AppImage does not start here${detail ? `:\n${detail}` : ''}`);
  }
  fs.writeFileSync(path.join(staging, RUNTIME_FILE), `${JSON.stringify({ path: null, executable })}\n`);
}

function readRuntime(root) {
  try {
    return JSON.parse(fs.readFileSync(path.join(root, RUNTIME_FILE), 'utf8'));
  } catch {
    return {};
  }
}

function markExecutable(root) {
  if (process.platform === 'win32') return;
  for (const directory of EXECUTABLE_DIRECTORIES.map((name) => path.join(root, name))) {
    let entries;
    try {
      entries = fs.readdirSync(directory, { withFileTypes: true });
    } catch {
      continue;
    }
    for (const entry of entries.filter((item) => item.isFile())) {
      const file = path.join(directory, entry.name);
      const { mode } = fs.statSync(file);
      if ((mode & 0o111) !== 0o111) fs.chmodSync(file, mode | 0o755);
    }
  }
}

function findInstalled() {
  for (const root of installRoots()) {
    const runtime = readRuntime(root);
    const exe = path.join(root, runtime.executable || executablePath());
    if (!fs.existsSync(exe)) continue;
    markExecutable(root);
    return { exe, env: runtimeEnvironment(runtime.path) };
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

function megabytes(bytes) {
  return (bytes / 1048576).toFixed(1);
}

async function fetchOrFail(url) {
  let response;
  try {
    response = await fetch(url);
  } catch (error) {
    throw new DownloadFailed(`GET ${url}: ${error.cause ? error.cause.message : error.message}`);
  }
  if (!response.ok) throw new DownloadFailed(`GET ${url}: HTTP ${response.status}`);
  return response;
}

async function download(url, destination) {
  log(`downloading ${url}`);
  const response = await fetchOrFail(url);
  const total = Number(response.headers.get('content-length')) || 0;
  const report = percentReporter('downloading', (done, size) => `${megabytes(done)}/${megabytes(size)} MB`);
  const hash = crypto.createHash('sha256');
  let received = 0;
  const meter = new Transform({
    transform(chunk, encoding, done) {
      hash.update(chunk);
      received += chunk.length;
      if (total) report(received, total);
      done(null, chunk);
    },
  });
  try {
    await pipeline(Readable.fromWeb(response.body), meter, fs.createWriteStream(destination));
  } catch (error) {
    throw new DownloadFailed(`GET ${url}: ${error.message}`);
  }
  return hash.digest('hex');
}

function recordedChecksums() {
  try {
    const recorded = JSON.parse(fs.readFileSync(SOURCE_CHECKSUM_FILE, 'utf8'));
    return recorded.version === product.version ? recorded : null;
  } catch {
    return null;
  }
}

function verify(actual, expected, name) {
  if (actual !== expected) {
    throw new InstallError(`${name} has SHA-256 ${actual}, but ${expected} was expected; refusing to install it`);
  }
}

function parseChecksums(text) {
  const sums = {};
  for (const line of text.split('\n')) {
    const [sum, name] = line.trim().split(/\s+\*?/);
    if (sum && name) sums[name] = sum.toLowerCase();
  }
  return sums;
}

async function releaseChecksum(asset) {
  const recorded = recordedChecksums();
  if (recorded && recorded.assets && recorded.assets[asset]) return recorded.assets[asset];
  log(`this package records no checksum for ${asset} (unpublished checkout); using the release's SHA256SUMS`);
  const response = await fetchOrFail(`${RELEASE_URL}/SHA256SUMS`);
  const sum = parseChecksums(await response.text())[asset];
  if (!sum) throw new DownloadFailed(`${asset} is not listed in the ${product.version} SHA256SUMS`);
  return sum;
}

function run(command, args) {
  const result = spawnSync(command, args, { stdio: 'inherit' });
  if (result.error) throw new InstallError(`${command}: ${result.error.message}`);
  if (result.status !== 0) throw new InstallError(`${command} ${args.join(' ')} exited with ${result.status}`);
}

async function installMacRelease(staging, work) {
  const { asset, app } = product.macRelease;
  const dmg = path.join(work, asset);
  const expected = await releaseChecksum(asset);
  verify(await download(`${RELEASE_URL}/${asset}`, dmg), expected, asset);
  const mountpoint = fs.mkdtempSync(path.join(work, 'mount-'));
  run('hdiutil', ['attach', '-nobrowse', '-readonly', '-quiet', '-mountpoint', mountpoint, dmg]);
  try {
    run('ditto', [path.join(mountpoint, app), path.join(staging, app)]);
  } finally {
    run('hdiutil', ['detach', '-quiet', mountpoint]);
  }
}

function buildLogPath() {
  const directory = path.join(cacheBase(), product.packageName);
  fs.mkdirSync(directory, { recursive: true });
  return path.join(directory, `build-${product.version}.log`);
}

function logTail(file, count) {
  try {
    return fs.readFileSync(file, 'utf8').trimEnd().split('\n').slice(-count).join('\n');
  } catch {
    return '';
  }
}

function percentReporter(label, describe) {
  let lastPercent = -1;
  return (done, total) => {
    const percent = Math.floor((done * 100) / total);
    if (percent === lastPercent) return;
    if (process.stderr.isTTY) {
      write(`\r[${product.packageName}] ${label}: ${describe(done, total)} (${percent}%)\x1b[K`);
      if (done === total) write('\n');
    } else if (percent % 10 === 0) {
      log(`${label}: ${percent}%`);
    }
    lastPercent = percent;
  };
}

function progressReporter(label) {
  const report = percentReporter(label, (done, total) => `${done}/${total}`);
  return (line) => {
    const step = line.match(/^\[(\d+)\/(\d+)\]/);
    if (step) report(Number(step[1]), Number(step[2]));
  };
}

async function buildFromSource(staging, work) {
  const env = buildEnvironment(product);
  const disabledOptions = preflight(product, env, {
    onWarning: (message) => log(message),
    onError: (message) => {
      throw new InstallError(message);
    },
  });

  const recorded = recordedChecksums();
  const expected = recorded ? recorded.sha256 : null;
  const tarball = path.join(work, `${product.packageName}-${product.version}.tar.gz`);
  const actual = await download(SOURCE_URL, tarball);
  if (expected) verify(actual, expected, `the ${product.version} source tarball`);
  else log('this package records no source checksum (unpublished checkout); the tarball is trusted over HTTPS only');

  const logFile = buildLogPath();
  const logFd = fs.openSync(logFile, 'w');
  const source = path.join(work, product.sourceDirectory);
  const build = path.join(work, 'build');
  log(`building ${product.name} ${product.version} from source; this takes several minutes`);
  log(`full build output: ${logFile}`);
  try {
    const tarArgs = ['-xzf', buildPath(tarball), '-C', buildPath(work)];
    await runLogged('tar', process.platform === 'win32' ? ['--force-local', ...tarArgs] : tarArgs, env, logFd);
    log('configuring');
    await runLogged('meson', ['setup', buildPath(build), buildPath(source), ...product.mesonOptions,
      ...disabledOptions, `--prefix=${buildPath(staging)}`], env, logFd);
    await runLogged('meson', ['compile', '-C', buildPath(build)], env, logFd, progressReporter('compiling'));
    log('installing');
    await runLogged('meson', ['install', '-C', buildPath(build), '--quiet'], env, logFd);
  } catch (error) {
    fs.closeSync(logFd);
    throw new InstallError(
      `${error.message}\n\nLast lines of ${logFile}:\n\n${logTail(logFile, 25)}\n\nThe full output is in ${logFile}.`,
    );
  }
  fs.closeSync(logFd);
  fs.rmSync(logFile, { force: true });

  const msys2 = locateMsys2();
  fs.writeFileSync(path.join(staging, RUNTIME_FILE), `${JSON.stringify({ path: msys2 ? msys2.bin : null })}\n`);
}

async function installLinuxReleaseOrBuild(staging, work) {
  const obstacle = linuxReleaseObstacle();
  if (obstacle === null) {
    try {
      await installLinuxRelease(staging);
      return;
    } catch (error) {
      if (!(error instanceof AppImageUnusable || error instanceof DownloadFailed)) throw error;
      log(`${error.message}\nbuilding from source instead`);
      fs.rmSync(staging, { recursive: true, force: true });
      fs.mkdirSync(staging, { recursive: true });
    }
  } else if (obstacle !== 'unsupported') {
    log(`building from source: ${obstacle}`);
  }
  await buildFromSource(staging, work);
}

async function install() {
  const existing = findInstalled();
  if (existing) return existing;

  const root = writableRoot();
  const staging = `${root}.partial-${process.pid}`;
  const work = fs.mkdtempSync(path.join(os.tmpdir(), `${product.packageName}-`));
  fs.rmSync(staging, { recursive: true, force: true });
  fs.mkdirSync(staging, { recursive: true });
  try {
    if (usesMacRelease()) await installMacRelease(staging, work);
    else await installLinuxReleaseOrBuild(staging, work);
    const produced = readRuntime(staging).executable || executablePath();
    if (!fs.existsSync(path.join(staging, produced))) {
      throw new InstallError(`the install did not produce ${produced}`);
    }
    markExecutable(staging);
    fs.rmSync(root, { recursive: true, force: true });
    fs.renameSync(staging, root);
  } finally {
    fs.rmSync(staging, { recursive: true, force: true });
    fs.rmSync(work, { recursive: true, force: true });
  }
  log(`installed ${product.name} ${product.version} in ${root}`);
  return findInstalled();
}

module.exports = { install, product, SOURCE_URL, RELEASE_URL, SOURCE_CHECKSUM_FILE, parseChecksums, InstallError, log };
