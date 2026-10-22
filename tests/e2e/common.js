// Helpers of the page scripts of a drop-in app's end-to-end tests (a build with BT_APP_TEST_HOOKS): the runner joins this
// file with the chain's script and gives it to the app (--webuitestscript); it runs in the page, with the test methods
// of the app's bridge (test.log, test.snapshot, test.confirmAnswer, test.confirmed, test.exit, test.node).
const E2E = (() => {
  const failures = [];
  const passed = [];
  const host = (method, params = {}) => window.__chains({ kind: 'host', method, params });
  const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
  const log = (message) => host('test.log', { message: String(message) });
  const T = () => window.__chainsTest;

  async function waitFor(what, fn, ms = 60000) {
    const until = Date.now() + ms;
    let last;
    let noted = Date.now();
    while (Date.now() < until) {
      if (Date.now() - noted > 20000) {
        noted = Date.now();
        await log(`WAITING for ${what}`);
      }
      try {
        last = await fn();
        if (last) return last;
      } catch (e) {
        last = e;
      }
      await sleep(250);
    }
    throw new Error(`timed out waiting for ${what} (${last && last.message ? last.message : JSON.stringify(last)})`);
  }

  function check(name, ok, detail = '') {
    if (ok) passed.push(name);
    else failures.push(`${name}${detail ? ': ' + detail : ''}`);
    return log(`${ok ? 'PASS' : 'FAIL'} ${name}${!ok && detail ? ' (' + detail + ')' : ''}`);
  }

  async function snapshot(name) {
    await sleep(500);
    await new Promise((resolve) => requestAnimationFrame(() => requestAnimationFrame(resolve)));
    await host('test.snapshot', { name });
  }

  /** A button by its words (exactly, or starting with them). */
  function button(label, root = document) {
    return [...root.querySelectorAll('button')].find((b) => b.textContent.trim() === label || b.textContent.trim().startsWith(label));
  }

  async function click(label, root = document) {
    const b = await waitFor(`the button ${label}`, () => {
      const x = button(label, root);
      return x && !x.disabled ? x : null;
    }, 30000);
    b.click();
  }

  /** Type into an input as the user does (the page's bindings hear an input event). */
  function type(selector, value) {
    const el = document.querySelector(selector);
    if (!el) throw new Error(`no ${selector}`);
    const proto = el.tagName === 'TEXTAREA' ? HTMLTextAreaElement.prototype : HTMLInputElement.prototype;
    Object.getOwnPropertyDescriptor(proto, 'value').set.call(el, value);
    el.dispatchEvent(new Event('input', { bubbles: true }));
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }

  async function go(page, tab = '') {
    T().go(page, tab);
    await sleep(700);
  }

  async function theme(id) {
    T().savePrefs({ theme: id });
    await sleep(600);
  }

  async function finish() {
    const result = { failures, passed };
    await log(`RESULT ${JSON.stringify(result)}`);
    await host('test.exit', { code: failures.length ? 1 : 0 });
  }

  async function ready() {
    await waitFor('the app ready', () => T() && T().app.phase === 'ready', 180000);
    await waitFor('the node state', () => T().node.tip && T().node.tip.height !== undefined, 60000);
  }

  return { host, sleep, log, T, waitFor, check, snapshot, button, click, type, go, theme, finish, ready };
})();
