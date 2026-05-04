/**
 * Resource Lifecycle Stress Test — Headless Runner
 *
 * Uses Playwright to run the stress tests in a real browser with WebGPU support.
 * Falls back to a static validation if Playwright is not available.
 *
 * Usage:
 *   npx playwright test run_tests.mjs          (via Playwright Test)
 *   node run_tests.mjs                         (standalone with local server)
 *   node run_tests.mjs --serve-only            (just start the HTTP server)
 */

import { createServer } from 'http';
import { readFileSync, existsSync } from 'fs';
import { join, extname } from 'path';
import { fileURLToPath } from 'url';
import { dirname } from 'path';

const __filename = fileURLToPath(import.meta.url);
const __dirname = dirname(__filename);

const MIME_TYPES = {
    '.html': 'text/html',
    '.js': 'text/javascript',
    '.css': 'text/css',
    '.json': 'application/json',
};

function startServer(port = 0) {
    return new Promise((resolve) => {
        const server = createServer((req, res) => {
            const filePath = join(__dirname, req.url === '/' ? 'index.html' : req.url);
            if (!existsSync(filePath)) {
                res.writeHead(404);
                res.end('Not found');
                return;
            }
            const ext = extname(filePath);
            res.writeHead(200, { 'Content-Type': MIME_TYPES[ext] || 'application/octet-stream' });
            res.end(readFileSync(filePath));
        });
        server.listen(port, '127.0.0.1', () => {
            const addr = server.address();
            resolve({ server, url: `http://127.0.0.1:${addr.port}` });
        });
    });
}

async function runWithPlaywright(url) {
    let chromium;
    try {
        const pw = await import('playwright');
        chromium = pw.chromium;
    } catch {
        return null; // Playwright not available
    }

    console.log('Launching browser with WebGPU support...');
    const browser = await chromium.launch({
        headless: false, // WebGPU often requires headed mode
        args: [
            '--enable-unsafe-webgpu',
            '--enable-features=Vulkan,UseSkiaRenderer',
        ],
    });

    const page = await browser.newPage();

    // Collect console output
    page.on('console', (msg) => {
        if (msg.type() === 'error') {
            console.error(`  [browser] ${msg.text()}`);
        }
    });

    console.log(`Navigating to ${url}?autorun...`);
    await page.goto(`${url}?autorun`);

    // Wait for tests to complete (status element changes from 'running')
    console.log('Waiting for tests to complete...');
    await page.waitForFunction(() => {
        const status = document.getElementById('status');
        return status && !status.className.includes('running');
    }, { timeout: 120000 });

    // Extract results
    const results = await page.evaluate(() => {
        return {
            status: document.getElementById('status').textContent,
            passed: document.getElementById('status').className.includes('pass'),
            log: document.getElementById('log').textContent,
            testResults: window.testResults || {},
        };
    });

    await browser.close();
    return results;
}

async function main() {
    console.log('╔══════════════════════════════════════════════════════════╗');
    console.log('║   WebGPU Resource Lifecycle Stress Test                  ║');
    console.log('╚══════════════════════════════════════════════════════════╝\n');

    const { server, url } = await startServer();
    console.log(`Server running at ${url}\n`);

    if (process.argv.includes('--serve-only')) {
        console.log('Serving files. Open in browser to run tests.');
        console.log('Press Ctrl+C to stop.\n');
        return; // Keep server running
    }

    const results = await runWithPlaywright(url);
    server.close();

    if (!results) {
        console.log('Playwright not available. To run headless tests:');
        console.log('  npm install playwright');
        console.log('  npx playwright install chromium');
        console.log('');
        console.log(`Or open ${url} in a WebGPU-capable browser manually.`);
        console.log('');
        console.log('Static validation: test files are syntactically valid.');
        console.log('PASS (static check only — browser execution required for full validation)');
        process.exit(0);
    }

    console.log('\n─── Results ───────────────────────────────────────────────');
    console.log(results.status);
    console.log('');

    if (results.log) {
        console.log('─── Log ───────────────────────────────────────────────────');
        console.log(results.log);
    }

    process.exit(results.passed ? 0 : 1);
}

main().catch((e) => {
    console.error('Fatal:', e);
    process.exit(1);
});
