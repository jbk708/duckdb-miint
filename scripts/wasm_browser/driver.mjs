// Run steps.json against the miint extension and compare each step to its
// expected value.
//
//   node driver.mjs --wasm <miint.duckdb_extension.wasm> --chrome <chrome binary>
//   node driver.mjs --native <duckdb shell with miint built in>
//
// --wasm boots duckdb-wasm in headless Chrome the way the playground does
// (index.html), serving the page and the extension with no COOP/COEP headers,
// like GitHub Pages, so duckdb-wasm picks the same bundle the playground gets.
// --native runs the same steps in a native shell, which is how the expected
// values are produced and kept honest.
//
// A step is {name, sql, expect?}. Steps run in order on one connection. A step
// with `expect` must return it as the first cell of its first row; cast the
// result to VARCHAR in the SQL so both runtimes print it the same way. A step
// without `expect` (setup) must only succeed.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const steps = JSON.parse(fs.readFileSync(path.join(here, 'steps.json'), 'utf8'));
const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) =>
  (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []));

let failures = 0;
function report(step, r) {
  if (!r.ok) {
    failures++;
    console.log(`FAIL ${step.name}: ${r.error}`);
  } else if (step.expect !== undefined && r.value !== step.expect) {
    failures++;
    console.log(`FAIL ${step.name}:\n  expected ${step.expect}\n  got      ${r.value}`);
  } else {
    console.log(`ok   ${step.name}`);
  }
}

function runNative(duckdb) {
  // One shell session, with a marker before each step so its output can be told apart.
  const script = steps.map(s => `.print @@step ${s.name}\n${s.sql.trim().replace(/;?$/, ';')}`).join('\n')
    + '\n.print @@end\n';
  const r = spawnSync(duckdb, ['-noheader', '-list', '-bail', ':memory:'], { input: script, encoding: 'utf8' });
  const out = {};
  let cur = null;
  for (const line of r.stdout.split('\n')) {
    const m = line.match(/^@@step (.*)$/);
    if (m) { cur = m[1]; out[cur] = []; } else if (line === '@@end') { cur = null; } else if (cur) out[cur].push(line);
  }
  const ended = r.stdout.includes('\n@@end');
  for (const s of steps) {
    if (!(s.name in out)) break;
    const failed = !ended && s.name === Object.keys(out).at(-1);
    report(s, failed ? { ok: false, error: r.stderr.trim() } : { ok: true, value: out[s.name][0] ?? null });
  }
}

async function runWasm(wasmFile, chrome) {
  const { default: puppeteer } = await import('puppeteer-core');
  const files = { '/': [path.join(here, 'index.html'), 'text/html'],
                  '/miint.duckdb_extension.wasm': [wasmFile, 'application/wasm'] };
  const server = http.createServer((req, res) => {
    const f = files[new URL(req.url, 'http://localhost').pathname];
    if (!f) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'content-type': f[1], 'content-length': fs.statSync(f[0]).size, 'cache-control': 'no-store' });
    if (req.method === 'HEAD') return res.end();
    fs.createReadStream(f[0]).pipe(res);
  }).listen(0, '127.0.0.1');
  await new Promise(r => server.once('listening', r));
  const base = `http://127.0.0.1:${server.address().port}`;

  const browser = await puppeteer.launch({ executablePath: chrome, headless: true, args: ['--no-sandbox'],
                                           protocolTimeout: 900_000 });
  try {
    const page = await browser.newPage();
    const log = [];
    page.on('workercreated', w => w.on('console', m => log.push(m.text())));
    await page.goto(`${base}/`, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => window.__ready === true, { timeout: 120_000 });
    const boot = await page.evaluate(() => window.__boot());
    console.log(`duckdb-wasm ${boot.version}, bundle ${boot.mainModule}`);
    const load = await page.evaluate(u => window.__load(u), `${base}/miint.duckdb_extension.wasm`);
    if (!load.ok) {
      failures++;
      console.log(`FAIL LOAD: ${load.error}`);
      return;
    }
    console.log(`miint ${load.version}`);
    for (const s of steps) {
      const before = log.length;
      const r = await page.evaluate(q => window.__step(q), s.sql);
      report(s, r);
      if (!r.ok) {
        const worker = log.slice(before).filter(l => l.trim());
        if (worker.length) console.log(worker.slice(0, 12).map(l => `  [worker] ${l}`).join('\n'));
        if (!r.alive) {
          console.log('  the database no longer answers; the remaining steps were not run');
          failures++;
          break;
        }
      }
    }
  } finally {
    await browser.close();
    server.close();
  }
}

if (args.native) runNative(args.native);
else if (args.wasm && args.chrome) await runWasm(args.wasm, args.chrome);
else {
  console.error('usage: driver.mjs --wasm <file> --chrome <binary> | --native <duckdb>');
  process.exit(2);
}
console.log(failures ? `${failures} step(s) failed` : `all ${steps.length} steps passed`);
process.exit(failures ? 1 : 0);
