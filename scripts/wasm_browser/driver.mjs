// Run steps.json against the miint extension and compare each step to its
// expected value.
//
//   node driver.mjs --wasm <miint.duckdb_extension.wasm> --chrome <chrome binary> --duckdb-version <vX.Y.Z>
//   node driver.mjs --native <duckdb shell with miint built in>
//
// --wasm boots duckdb-wasm in headless Chrome the way the playground does
// (index.html), serving the page and the extension with no COOP/COEP headers,
// like GitHub Pages, so duckdb-wasm picks the same bundle the playground gets.
// --duckdb-version is the core the extension targets; the run fails if the
// pinned duckdb-wasm reports another.
// --native runs the same steps in a native shell. That is where expected values
// come from: after adding a step or bumping rype or sc, run it by hand and update
// any `expect` that changed.
//
// A step is {name, sql, expect?, expect_error?}, and names are unique. Steps run
// in order on one connection. A step with `expect` must return it as the first
// cell of its first row; cast the result to VARCHAR in the SQL so both runtimes
// print it the same way. A step with `expect_error` must fail with an error
// containing that text and leave the database answering. A step with neither
// (setup) must only succeed. The run passes only when every step ran and passed.
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
  if (!/^[A-Za-z0-9_.-]+$/.test(s.name)) {
    console.error(`steps.json: step name '${s.name}' may use only letters, digits, '_', '.' and '-'`);
    process.exit(2);
  }
  if (s.expect !== undefined && s.expect_error !== undefined) {
    console.error(`steps.json: step '${s.name}' has both expect and expect_error`);
    process.exit(2);
  }
  if (s.expect_error !== undefined && (typeof s.expect_error !== 'string' || s.expect_error === '')) {
    console.error(`steps.json: step '${s.name}' needs a non-empty string expect_error`);
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
// r is {ok, value} or {ok: false, error, alive}.
function report(step, r) {
  let why = null;
  if (step.expect_error !== undefined) {
    if (r.ok) why = `expected an error containing "${step.expect_error}", but the step succeeded`;
    else if (!r.error.includes(step.expect_error)) why = `expected an error containing "${step.expect_error}", got: ${r.error}`;
    else if (!r.alive) why = `failed as expected, but the database no longer answers: ${r.error}`;
  } else if (!r.ok) {
    why = r.error;
  } else if (step.expect !== undefined && r.value !== step.expect) {
    why = `\n  expected ${step.expect}\n  got      ${r.value}`;
  }
  if (why === null) {
    passed++;
    console.log(`ok   ${step.name}`);
  } else {
    failed++;
    console.log(`FAIL ${step.name}: ${why}`.replace(': \n', ':\n'));
  }
}

// The shell prints an error's first line as "<Kind> Error: ...".
const ERROR_LINE = /^[A-Za-z][A-Za-z ]*Error: /;

function runNative(duckdb) {
  // One shell session with a marker before each step. Its output goes to stderr
  // (.output /dev/stderr), so each step's errors land between its markers in
  // order. No -bail: a step expected to fail must not end the run, and a step
  // that fails unexpectedly is reported without stopping the rest.
  const script = '.output /dev/stderr\n'
    + steps.map(s => `.print @@step ${s.name}\n${s.sql.trim().replace(/;?$/, ';')}`).join('\n') + '\n.print @@end\n';
  const r = spawnSync(duckdb, ['-noheader', '-list', ':memory:'], { input: script, encoding: 'utf8' });
  if (r.error) {
    aborted = `could not run ${duckdb}: ${r.error.message}`;
    return;
  }
  const out = new Map();
  let cur = null;
  let ended = false;
  for (const line of r.stderr.split('\n')) {
    const m = line.match(/^@@step (.*)$/);
    if (m) { cur = m[1]; out.set(cur, []); } else if (line === '@@end') { ended = true; cur = null; } else if (cur) out.get(cur).push(line);
  }
  const started = [...out.keys()];
  const stopped = `shell stopped (${r.signal ? `signal ${r.signal}` : `status ${r.status}`})`;
  for (const [i, s] of steps.entries()) {
    if (!out.has(s.name)) break;
    const lines = out.get(s.name);
    // The shell went on to another step or the end marker, so it still answers.
    const alive = ended || i < started.length - 1;
    const at = lines.findIndex(l => ERROR_LINE.test(l));
    if (at >= 0) report(s, { ok: false, error: lines.slice(at).join('\n').trim(), alive });
    else if (!alive) report(s, { ok: false, error: `${stopped} during this step`, alive });
    else report(s, { ok: true, value: lines[0] ?? null });
  }
  if (out.size === 0) aborted = `${stopped} before the first step: ${r.stdout.trim() || '(no message)'}`;
}

async function runWasm(wasmFile, chrome, duckdbVersion) {
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

  const browser = await puppeteer.launch({ executablePath: chrome, headless: true, protocolTimeout: 900_000 });
  try {
    const page = await browser.newPage();
    const log = [];
    page.on('workercreated', w => w.on('console', m => log.push(m.text())));
    await page.goto(`${base}/`, { waitUntil: 'domcontentloaded' });
    await page.waitForFunction(() => window.__ready === true, { timeout: 120_000 });
    const boot = await page.evaluate(v => window.__boot(v), duckdbVersion);
    if (!boot.ok) {
      aborted = `boot: ${boot.error}`;
      return;
    }
    console.log(`duckdb-wasm ${boot.version}, platform ${boot.platform}, bundle ${boot.mainModule}`);
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
      if (!r.ok && !(s.expect_error !== undefined && r.alive)) {
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
else if (args.wasm && args.chrome && args['duckdb-version']) await runWasm(args.wasm, args.chrome, args['duckdb-version']);
else {
  console.error('usage: driver.mjs --wasm <file> --chrome <binary> --duckdb-version <vX.Y.Z> | --native <duckdb>');
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
