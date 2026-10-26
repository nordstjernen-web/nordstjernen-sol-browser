// postinstall.js: installs the browser binary when the npm package is installed, so the first launch is immediate.
'use strict';

const { install, installPlan, storeMessage, InstallError, log } = require('./install');

if (process.env.NORDSTJERNEN_SKIP_INSTALL === '1') {
  log('NORDSTJERNEN_SKIP_INSTALL=1: the browser is installed on first launch instead');
} else if (installPlan().kind === 'store') {
  log(storeMessage());
} else {
  install().catch((error) => {
    log(error instanceof InstallError ? error.message : error.stack);
    process.exit(1);
  });
}
