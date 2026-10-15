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
// A step is {name, sql, expect?}, and names are unique. Steps run in order on
// one connection. A step with `expect` must return it as the first cell of its
// first row; cast the result to VARCHAR in the SQL so both runtimes print it the
// same way. A step without `expect` (setup) must only succeed. The run passes
// only when every step ran and passed.
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const steps = JSON.parse(fs.readFileSync(path.join(here, 'steps.json'), 'utf8'));
const args = Object.fromEntries(process.argv.slice(2).reduce((acc, a, i, all) =>
  (a.startsWith('--') ? [...acc, [a.slice(2), all[i + 1]]] : acc), []));

const names = new Set();
for (const s of steps) {
  if (typeof s.name !== 'string' || typeof s.sql !== 'string') {
    console.error(`steps.json: every step needs a string name and sql: ${JSON.stringify(s).slice(0, 120)}`);
    process.exit(2);
  }
  if (names.has(s.name)) {
    console.error(`steps.json: duplicate step name '${s.name}'`);
    process.exit(2);
  }
  names.add(s.name);
}

// Each step is reported once, as passed or failed; whatever is left was not run.
// `aborted` is a failure outside any step (the shell or the page), which fails the
// run even if no step did.
let passed = 0;
let failed = 0;
let aborted = null;
function report(step, r) {
  if (!r.ok) {
    failed++;
    console.log(`FAIL ${step.name}: ${r.error}`);
  } else if (step.expect !== undefined && r.value !== step.expect) {
    failed++;
    console.log(`FAIL ${step.name}:\n  expected ${step.expect}\n  got      ${r.value}`);
  } else {
    passed++;
    console.log(`ok   ${step.name}`);
  }
}

function runNative(duckdb) {
  // One shell session, with a marker before each step so its output can be told apart.
  const script = steps.map(s => `.print @@step ${s.name}\n${s.sql.trim().replace(/;?$/, ';')}`).join('\n')
    + '\n.print @@end\n';
  const r = spawnSync(duckdb, ['-noheader', '-list', '-bail', ':memory:'], { input: script, encoding: 'utf8' });
  if (r.error) {
    aborted = `could not run ${duckdb}: ${r.error.message}`;
    return;
  }
  const out = new Map();
  let cur = null;
  let ended = false;
  for (const line of r.stdout.split('\n')) {
    const m = line.match(/^@@step (.*)$/);
    if (m) { cur = m[1]; out.set(cur, []); } else if (line === '@@end') { ended = true; cur = null; } else if (cur) out.get(cur).push(line);
  }
  const clean = ended && r.status === 0;
  const last = [...out.keys()].at(-1);
  for (const s of steps) {
    if (!out.has(s.name)) break;
    // With -bail the shell stops at the first error, so a failure belongs to the
    // last step it started.
    report(s, !clean && s.name === last
      ? { ok: false, error: `shell exited ${r.status}: ${r.stderr.trim() || '(no message)'}` }
      : { ok: true, value: out.get(s.name)[0] ?? null });
  }
  if (!clean && out.size === 0) aborted = `shell exited ${r.status} before the first step: ${r.stderr.trim() || '(no message)'}`;
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
    if (!boot.ok) {
      aborted = `boot: ${boot.error}`;
      return;
    }
    console.log(`duckdb-wasm ${boot.version}, bundle ${boot.mainModule}`);
    const load = await page.evaluate(u => window.__load(u), `${base}/miint.duckdb_extension.wasm`);
    if (!load.ok) {
      aborted = `LOAD: ${load.error}`;
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
          console.log('  the database no longer answers');
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
const notRun = steps.length - passed - failed;
if (aborted) console.log(`FAIL ${aborted}`);
if (!aborted && failed === 0 && notRun === 0) {
  console.log(`all ${steps.length} steps passed`);
  process.exit(0);
}
console.log(`${passed} passed, ${failed} failed, ${notRun} not run, of ${steps.length} steps`);
process.exit(1);
