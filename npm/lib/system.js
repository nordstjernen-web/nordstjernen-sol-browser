// system.js: runs build programs natively, or from MSYS2's MinGW environment on Windows, and writes to the user's terminal.
'use strict';

const fs = require('fs');
const path = require('path');
const { spawn, spawnSync } = require('child_process');

const MSYS2_ENVIRONMENTS = [
  { directory: 'ucrt64', msystem: 'UCRT64', packagePrefix: 'mingw-w64-ucrt-x86_64-' },
  { directory: 'mingw64', msystem: 'MINGW64', packagePrefix: 'mingw-w64-x86_64-' },
];

let terminal;

function terminalFd() {
  if (terminal !== undefined) return terminal;
  terminal = null;
  if (!process.stderr.isTTY) {
    try {
      terminal = fs.openSync(process.platform === 'win32' ? '\\\\.\\CONOUT$' : '/dev/tty', 'w');
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

function msys2Roots() {
  const roots = [process.env.MSYS2_ROOT, 'C:\\msys64', path.join(process.env.LOCALAPPDATA || '', 'msys64')];
  return roots.filter((root) => root && fs.existsSync(path.join(root, 'usr', 'bin', 'bash.exe')));
}

function locateMsys2() {
  if (process.platform !== 'win32') return null;
  const roots = msys2Roots();
  if (!roots.length) return null;
  for (const root of roots) {
    for (const environment of MSYS2_ENVIRONMENTS) {
      if (fs.existsSync(path.join(root, environment.directory, 'bin', 'libgtk-4-1.dll'))) {
        return { root, ...environment, bin: path.join(root, environment.directory, 'bin') };
      }
    }
  }
  const environment = MSYS2_ENVIRONMENTS[0];
  return { root: roots[0], ...environment, bin: path.join(roots[0], environment.directory, 'bin') };
}

function capture(command, args, env = process.env) {
  const result = spawnSync(command, args, { encoding: 'utf8', env });
  if (result.error || result.status !== 0) return null;
  return result.stdout.trim();
}

function brewPrefix(formula) {
  return process.platform === 'darwin' ? capture('brew', ['--prefix', formula]) : null;
}

function buildEnvironment(product) {
  const env = { ...process.env };
  const extra = product.extraPath ? product.extraPath() : [];
  if (extra.length) env.PATH = [...extra, process.env.PATH].join(path.delimiter);
  const msys2 = locateMsys2();
  if (msys2) {
    env.PATH = [msys2.bin, path.join(msys2.root, 'usr', 'bin'), env.PATH].join(path.delimiter);
    env.MSYSTEM = msys2.msystem;
    Object.assign(env, product.windowsEnv || {});
  }
  const kegs = ['curl', 'openssl@3']
    .map(brewPrefix)
    .filter(Boolean)
    .map((prefix) => path.join(prefix, 'lib', 'pkgconfig'));
  if (kegs.length) {
    env.PKG_CONFIG_PATH = [...kegs, ...(process.env.PKG_CONFIG_PATH ? [process.env.PKG_CONFIG_PATH] : [])]
      .join(path.delimiter);
  }
  return env;
}

function runtimeEnvironment(runtimePath) {
  if (!runtimePath) return process.env;
  return { ...process.env, PATH: [runtimePath, process.env.PATH].join(path.delimiter) };
}

function buildPath(file) {
  return process.platform === 'win32' ? file.replace(/\\/g, '/') : file;
}

function runLogged(command, args, env, logFd, onLine) {
  fs.writeSync(logFd, `\n$ ${command} ${args.join(' ')}\n`);
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { env, stdio: ['ignore', 'pipe', 'pipe'] });
    let pending = '';
    child.stdout.on('data', (chunk) => {
      fs.writeSync(logFd, chunk);
      if (!onLine) return;
      pending += chunk.toString();
      const lines = pending.split(/\r?\n/);
      pending = lines.pop();
      lines.forEach(onLine);
    });
    child.stderr.on('data', (chunk) => fs.writeSync(logFd, chunk));
    child.on('error', (error) => reject(new Error(`${command}: ${error.message}`)));
    child.on('close', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`${command} ${args[0]} failed (exit ${code})`));
    });
  });
}

module.exports = { write, locateMsys2, capture, buildEnvironment, runtimeEnvironment, buildPath, runLogged };
