import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import path from 'node:path';
import process from 'node:process';

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function runMacLoader(loader, executable, arguments_, environment, timeoutMs) {
  await new Promise((resolve, reject) => {
    const child = spawn(loader, [executable, ...arguments_], {
      env: environment,
      stdio: 'ignore',
      windowsHide: true,
    });
    const timeout = setTimeout(() => {
      child.kill();
      reject(new Error(`MacLoader did not return within ${timeoutMs} ms`));
    }, timeoutMs);
    let settled = false;
    const finish = (action) => {
      if (settled) return;
      settled = true;
      clearTimeout(timeout);
      action();
    };
    child.once('error', (error) => finish(() => reject(error)));
    child.once('exit', (code, signal) => finish(() => {
      if (code === 0) resolve();
      else reject(new Error(
        `MacLoader exited with code ${code ?? 'none'} and signal ${signal ?? 'none'}`,
      ));
    }));
  });
}

async function waitForDevToolsPort(userDataDirectory, timeoutMs) {
  const portFile = path.join(userDataDirectory, 'DevToolsActivePort');
  const deadline = Date.now() + timeoutMs;
  do {
    try {
      const [portText] = (await readFile(portFile, 'utf8')).split(/\r?\n/u);
      const port = Number(portText);
      if (Number.isInteger(port) && port > 0 && port <= 65535) return port;
    } catch (error) {
      if (error?.code !== 'ENOENT') throw error;
    }
    await delay(25);
  } while (Date.now() < deadline);
  throw new Error('MacLoader Chromium did not publish DevToolsActivePort');
}

async function browserPid(browser) {
  const session = await browser.newBrowserCDPSession();
  try {
    const result = await session.send('SystemInfo.getProcessInfo');
    const processInfo = result.processInfo.find((entry) => entry.type === 'browser');
    return Number.isInteger(processInfo?.id) && processInfo.id > 0
      ? processInfo.id
      : null;
  } catch {
    return null;
  } finally {
    await session.detach();
  }
}

async function closeBrowserWithin(browser, timeoutMs) {
  await Promise.race([
    browser.close().catch(() => undefined),
    delay(timeoutMs),
  ]);
}

function isProcessRunning(pid) {
  try {
    process.kill(pid, 0);
    return true;
  } catch {
    return false;
  }
}

export function processTreeTerminationCommand(
  pid,
  platform = process.platform,
  systemRoot = process.env.SystemRoot || 'C:\\Windows',
) {
  if (!Number.isInteger(pid) || pid <= 0) {
    throw new Error(`Invalid browser PID for process-tree cleanup: ${pid}`);
  }
  if (platform !== 'win32') return null;
  return {
    executable: path.win32.join(systemRoot, 'System32', 'taskkill.exe'),
    arguments: ['/PID', String(pid), '/T', '/F'],
  };
}

async function terminateWindowsProcessTree(pid) {
  const command = processTreeTerminationCommand(pid);
  if (!command || !isProcessRunning(pid)) return false;
  await new Promise((resolve, reject) => {
    const child = spawn(command.executable, command.arguments, {
      stdio: 'ignore',
      windowsHide: true,
    });
    child.once('error', reject);
    child.once('exit', (code, signal) => {
      if (code === 0 || !isProcessRunning(pid)) {
        resolve();
      } else {
        reject(new Error(
          `Browser process-tree cleanup exited with code ` +
            `${code ?? 'none'} and signal ${signal ?? 'none'}`,
        ));
      }
    });
  });
  return true;
}

async function closeBrowserProcess(browser, pid) {
  // Browser.close can let the root exit before Windows releases every child.
  // Terminate the exact test-owned tree while its verified root PID still
  // exists, so a detached utility process cannot retain the temporary profile.
  if (pid && await terminateWindowsProcessTree(pid)) {
    await closeBrowserWithin(browser, 2000);
    return;
  }
  try {
    const session = await browser.newBrowserCDPSession();
    await Promise.race([
      session.send('Browser.close').catch(() => undefined),
      delay(2000),
    ]);
  } catch {
    // Continue to the PID-scoped fallback when the CDP connection is already gone.
  }
  await closeBrowserWithin(browser, 2000);
  if (!pid) return;

  const deadline = Date.now() + 3000;
  while (isProcessRunning(pid) && Date.now() < deadline) {
    await delay(25);
  }
  if (isProcessRunning(pid)) {
    try {
      process.kill(pid);
    } catch (error) {
      if (error?.code !== 'ESRCH') throw error;
    }
  }
}

async function removeUserDataDirectory(userDataDirectory) {
  await rm(userDataDirectory, {
    force: true,
    maxRetries: 20,
    recursive: true,
    retryDelay: 250,
  });
}

export async function launchChromiumWithProductLoader({
  browserType,
  environment,
  executable,
  loader,
  timeoutMs,
}) {
  const userDataDirectory = await mkdtemp(path.join(
    process.env.RUNNER_TEMP || tmpdir(),
    'mactype-chromium-loader-',
  ));
  let browser = null;
  let pid = null;
  try {
    // Match Playwright's ordinary headless stability baseline while replacing
    // only its pipe transport with a local DevTools port. In particular, no
    // Chromium font-service feature is disabled in this product proof.
    const chromiumArguments = [
      '--headless',
      '--disable-background-networking',
      '--disable-breakpad',
      '--disable-component-update',
      '--disable-default-apps',
      '--disable-dev-shm-usage',
      '--disable-extensions',
      '--disable-sync',
      '--enable-automation',
      '--enable-unsafe-swiftshader',
      '--force-color-profile=srgb',
      '--hide-scrollbars',
      '--metrics-recording-only',
      '--mute-audio',
      '--no-default-browser-check',
      '--no-first-run',
      '--no-sandbox',
      '--password-store=basic',
      '--remote-debugging-port=0',
      `--user-data-dir=${userDataDirectory}`,
      'about:blank',
    ];
    await runMacLoader(loader, executable, chromiumArguments, environment, timeoutMs);
    const port = await waitForDevToolsPort(userDataDirectory, timeoutMs);
    browser = await browserType.connectOverCDP(`http://127.0.0.1:${port}`, {
      timeout: timeoutMs,
    });
    pid = await browserPid(browser);
    if (!Number.isInteger(pid) || pid <= 0) {
      throw new Error('MacLoader Chromium did not expose its browser PID');
    }

    let closed = false;
    return {
      browser,
      browserPid: pid,
      close: async () => {
        if (closed) return;
        await closeBrowserProcess(browser, pid);
        await removeUserDataDirectory(userDataDirectory);
        closed = true;
      },
    };
  } catch (error) {
    if (browser) await closeBrowserProcess(browser, pid);
    await removeUserDataDirectory(userDataDirectory);
    throw error;
  }
}
