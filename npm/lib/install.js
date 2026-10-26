// install.js: puts the Nordstjernen binary for this machine in place, from the notarised macOS release or a source build.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const crypto = require('crypto');
const { spawn, spawnSync } = require('child_process');
const { Readable } = require('stream');
const { pipeline } = require('stream/promises');
const { version } = require('../package.json');
const { preflight, capture } = require('./dependencies');

const REPO = 'nordstjernen-web/nordstjernen-sol-browser';
const RELEASE_URL = `https://github.com/${REPO}/releases/download/${version}`;
const SOURCE_URL = `https://github.com/${REPO}/archive/refs/tags/${version}.tar.gz`;
const SOURCE_CHECKSUM_FILE = path.join(__dirname, 'source.json');
const STORE_URL = 'https://apps.microsoft.com/detail/9NW8T7W5Z4PL';
class InstallError extends Error {}

let terminal;

function terminalFd() {
  if (terminal !== undefined) return terminal;
  terminal = null;
  if (!process.stderr.isTTY && process.platform !== 'win32') {
    try {
      terminal = fs.openSync('/dev/tty', 'w');
    } catch {
      terminal = null;
    }
  }
  return terminal;
}

function write(text) {
  const fd = terminalFd();
  if (fd !== null) fs.writeSync(fd, text);
  else process.stderr.write(text);
}

function log(message) {
  write(`[nordstjernen-browser] ${message}\n`);
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

function brewPrefix(formula) {
  return process.platform === 'darwin' ? capture('brew', ['--prefix', formula]) : null;
}

function buildEnvironment() {
  const extra = ['curl', 'openssl@3']
    .map(brewPrefix)
    .filter(Boolean)
    .map((prefix) => path.join(prefix, 'lib', 'pkgconfig'));
  if (!extra.length) return process.env;
  const current = process.env.PKG_CONFIG_PATH ? [process.env.PKG_CONFIG_PATH] : [];
  return { ...process.env, PKG_CONFIG_PATH: [...extra, ...current].join(path.delimiter) };
}

async function download(url, destination) {
  log(`downloading ${url}`);
  const response = await fetch(url);
  if (!response.ok) throw new InstallError(`GET ${url}: HTTP ${response.status}`);
  await pipeline(Readable.fromWeb(response.body), fs.createWriteStream(destination));
}

async function releaseChecksum(asset) {
  const response = await fetch(`${RELEASE_URL}/SHA256SUMS`);
  if (!response.ok) throw new InstallError(`GET ${RELEASE_URL}/SHA256SUMS: HTTP ${response.status}`);
  for (const line of (await response.text()).split('\n')) {
    const [sum, name] = line.trim().split(/\s+\*?/);
    if (name === asset) return sum.toLowerCase();
  }
  throw new InstallError(`${asset} is not listed in the ${version} SHA256SUMS`);
}

function sourceChecksum() {
  try {
    const recorded = JSON.parse(fs.readFileSync(SOURCE_CHECKSUM_FILE, 'utf8'));
    if (recorded.version === version) return recorded.sha256;
  } catch {
    return null;
  }
  return null;
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

async function installDmg(plan, staging, work) {
  const dmg = path.join(work, plan.asset);
  await download(`${RELEASE_URL}/${plan.asset}`, dmg);
  verify(dmg, await releaseChecksum(plan.asset), plan.asset);
  const mountpoint = fs.mkdtempSync(path.join(work, 'mount-'));
  run('hdiutil', ['attach', '-nobrowse', '-readonly', '-quiet', '-mountpoint', mountpoint, dmg]);
  try {
    run('ditto', [path.join(mountpoint, 'Nordstjernen.app'), path.join(staging, 'Nordstjernen.app')]);
  } finally {
    run('hdiutil', ['detach', '-quiet', mountpoint]);
  }
}

function buildLogPath() {
  const directory = path.join(cacheBase(), 'nordstjernen-browser');
  fs.mkdirSync(directory, { recursive: true });
  return path.join(directory, `build-${version}.log`);
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
  return (done, total) => {
    const percent = Math.floor((done * 100) / total);
    if (percent === lastPercent) return;
    if (process.stderr.isTTY) {
      write(`\r[nordstjernen-browser] ${label}: ${done}/${total} (${percent}%)\x1b[K`);
      if (done === total) write('\n');
    } else if (percent % 10 === 0) {
      log(`${label}: ${percent}%`);
    }
    lastPercent = percent;
  };
}

function runLogged(command, args, env, logFd, onProgress) {
  fs.writeSync(logFd, `\n$ ${command} ${args.join(' ')}\n`);
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let pending = '';
    child.stdout.on('data', (chunk) => {
      fs.writeSync(logFd, chunk);
      if (!onProgress) return;
      pending += chunk.toString();
      const lines = pending.split('\n');
      pending = lines.pop();
      for (const line of lines) {
        const step = line.match(/^\[(\d+)\/(\d+)\]/);
        if (step) onProgress(Number(step[1]), Number(step[2]));
      }
    });
    child.stderr.on('data', (chunk) => fs.writeSync(logFd, chunk));
    child.on('error', (error) => reject(new InstallError(`${command}: ${error.message}`)));
    child.on('close', (code) => {
      if (code === 0) resolve();
      else reject(new InstallError(`${command} ${args[0]} failed (exit ${code})`));
    });
  });
}

async function installSource(staging, work) {
  const env = buildEnvironment();
  preflight(env, {
    onWarning: (message) => log(message),
    onError: (message) => {
      throw new InstallError(message);
    },
  });

  const expected = sourceChecksum();
  const tarball = path.join(work, `nordstjernen-${version}.tar.gz`);
  await download(SOURCE_URL, tarball);
  if (expected) verify(tarball, expected, `the ${version} source tarball`);
  else log('this package records no source checksum (unpublished checkout); the tarball is trusted over HTTPS only');
  run('tar', ['-xzf', tarball, '-C', work]);
  const source = path.join(work, `nordstjernen-sol-browser-${version}`);
  const build = path.join(work, 'build');

  const logFile = buildLogPath();
  const logFd = fs.openSync(logFile, 'w');
  log(`building Nordstjernen ${version} from source; this takes several minutes`);
  log(`full build output: ${logFile}`);
  try {
    log('configuring');
    await runLogged('meson', ['setup', build, source, '--buildtype=release', `--prefix=${staging}`], env, logFd);
    await runLogged('meson', ['compile', '-C', build], env, logFd, progressReporter('compiling'));
    log('installing');
    await runLogged('meson', ['install', '-C', build, '--quiet'], env, logFd);
  } catch (error) {
    fs.closeSync(logFd);
    throw new InstallError(
      `${error.message}\n\nLast lines of ${logFile}:\n\n${logTail(logFile, 25)}\n\nThe full output is in ${logFile}.`,
    );
  }
  fs.closeSync(logFd);
  fs.rmSync(logFile, { force: true });
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

module.exports = { install, installPlan, storeMessage, STORE_URL, SOURCE_URL, SOURCE_CHECKSUM_FILE, InstallError, log };
