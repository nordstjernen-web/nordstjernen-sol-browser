// install.js: builds the browser release from source with meson, or copies a notarised macOS release where the product has one, and installs it inside the package.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawnSync } = require('child_process');
const { Readable } = require('stream');
const { pipeline } = require('stream/promises');
const product = require('./product');
const { preflight } = require('./dependencies');
const { write, locateMsys2, buildEnvironment, runtimeEnvironment, buildPath, runLogged } = require('./system');

const RELEASE_URL = `https://github.com/${product.repo}/releases/download/${product.version}`;
const SOURCE_URL = `https://github.com/${product.repo}/archive/refs/tags/${product.version}.tar.gz`;
const SOURCE_CHECKSUM_FILE = path.join(__dirname, 'source.json');
const RUNTIME_FILE = 'npm-runtime.json';
const EXECUTABLE_DIRECTORIES = ['bin', 'libexec'];

class InstallError extends Error {}

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
    const exe = path.join(root, executablePath());
    if (!fs.existsSync(exe)) continue;
    markExecutable(root);
    return { exe, env: runtimeEnvironment(readRuntime(root).path) };
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

async function download(url, destination) {
  log(`downloading ${url}`);
  const response = await fetch(url);
  if (!response.ok) throw new InstallError(`GET ${url}: HTTP ${response.status}`);
  await pipeline(Readable.fromWeb(response.body), fs.createWriteStream(destination));
}

function sourceChecksum() {
  try {
    const recorded = JSON.parse(fs.readFileSync(SOURCE_CHECKSUM_FILE, 'utf8'));
    return recorded.version === product.version ? recorded.sha256 : null;
  } catch {
    return null;
  }
}

function sha256(file) {
  return crypto.createHash('sha256').update(fs.readFileSync(file)).digest('hex');
}

function verify(file, expected, name) {
  const actual = sha256(file);
  if (actual !== expected) {
    throw new InstallError(`${name} has SHA-256 ${actual}, but ${expected} was expected; refusing to install it`);
  }
}

async function releaseChecksum(asset) {
  const response = await fetch(`${RELEASE_URL}/SHA256SUMS`);
  if (!response.ok) throw new InstallError(`GET ${RELEASE_URL}/SHA256SUMS: HTTP ${response.status}`);
  for (const line of (await response.text()).split('\n')) {
    const [sum, name] = line.trim().split(/\s+\*?/);
    if (name === asset) return sum.toLowerCase();
  }
  throw new InstallError(`${asset} is not listed in the ${product.version} SHA256SUMS`);
}

function run(command, args) {
  const result = spawnSync(command, args, { stdio: 'inherit' });
  if (result.error) throw new InstallError(`${command}: ${result.error.message}`);
  if (result.status !== 0) throw new InstallError(`${command} ${args.join(' ')} exited with ${result.status}`);
}

async function installMacRelease(staging, work) {
  const { asset, app } = product.macRelease;
  const dmg = path.join(work, asset);
  await download(`${RELEASE_URL}/${asset}`, dmg);
  verify(dmg, await releaseChecksum(asset), asset);
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

function progressReporter(label) {
  let lastPercent = -1;
  return (line) => {
    const step = line.match(/^\[(\d+)\/(\d+)\]/);
    if (!step) return;
    const [done, total] = [Number(step[1]), Number(step[2])];
    const percent = Math.floor((done * 100) / total);
    if (percent === lastPercent) return;
    if (process.stderr.isTTY) {
      write(`\r[${product.packageName}] ${label}: ${done}/${total} (${percent}%)\x1b[K`);
      if (done === total) write('\n');
    } else if (percent % 10 === 0) {
      log(`${label}: ${percent}%`);
    }
    lastPercent = percent;
  };
}

async function buildFromSource(staging, work) {
  const env = buildEnvironment(product);
  preflight(product, env, {
    onWarning: (message) => log(message),
    onError: (message) => {
      throw new InstallError(message);
    },
  });

  const expected = sourceChecksum();
  const tarball = path.join(work, `${product.packageName}-${product.version}.tar.gz`);
  await download(SOURCE_URL, tarball);
  if (expected) verify(tarball, expected, `the ${product.version} source tarball`);
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
      `--prefix=${buildPath(staging)}`], env, logFd);
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
    else await buildFromSource(staging, work);
    if (!fs.existsSync(path.join(staging, executablePath()))) {
      throw new InstallError(`the install did not produce ${executablePath()}`);
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

module.exports = { install, product, SOURCE_URL, SOURCE_CHECKSUM_FILE, InstallError, log };
