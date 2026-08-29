/*
   Runs the interface's script against a stub browser, once per entry point.

   The interface is a single file with no build step and no test framework, which
   is the right trade for something that has to fit in a flash partition. The
   gap that leaves is startup: a variable declared below the code that reads it
   throws a ReferenceError before anything renders, and the page is simply blank.
   That has happened twice -- both times only on one particular #hash, so the
   other entry points kept working and hid it.

   This does not test behaviour. It evaluates the script the way a browser would
   and reports whether it survives, for every hash the interface routes to.

   (C) 2026 Jonas Brüstel
   Licensed under the GNU General Public License version 3.0.
*/

const fs = require('fs');
const vm = require('vm');

const html = fs.readFileSync(process.argv[2] || 'www/index.html', 'utf8');
const script = [...html.matchAll(/<script[^>]*>([\s\S]*?)<\/script>/g)]
  .map(m => m[1]).join('\n');

/* Enough of a DOM that the script can wire itself up. Every node answers to
   everything and remembers nothing: the point is to reach the end of the file,
   not to reproduce a browser. */
function makeElement() {
  const el = {
    style: {}, dataset: {}, classList: {add(){}, remove(){}, toggle(){}, contains(){return false}},
    children: [], hidden: false, textContent: '', innerHTML: '', value: '', checked: false,
    files: [], placeholder: '', className: '', id: '', ariaSelected: '',
    appendChild(c){ return c }, append(){}, prepend(){}, remove(){}, removeChild(){},
    insertAdjacentHTML(){}, addEventListener(){}, removeEventListener(){},
    querySelector(){ return makeElement() }, querySelectorAll(){ return [] },
    scrollIntoView(){}, click(){}, focus(){}, showModal(){}, close(){},
    getAttribute(){ return null }, setAttribute(){}, matches(){ return false },
    get firstChild(){ return null }, get lastChild(){ return null },
    childElementCount: 0, scrollTop: 0, scrollHeight: 0, clientHeight: 0, offsetHeight: 0,
  };
  return el;
}

const failures = [];
for (const hash of ['', '#home', '#settings', '#editor', '#nonsense']) {
  const sandbox = {
    document: {
      getElementById: () => makeElement(),
      querySelector: () => makeElement(),
      querySelectorAll: () => [],
      createElement: () => makeElement(),
      addEventListener() {},
      body: makeElement(),
    },
    location: { hash, host: 'device.local', href: '' },
    /* Never resolves: startup must not depend on a reply having arrived. */
    fetch: () => new Promise(() => {}),
    WebSocket: function () { return { onmessage: null, onclose: null, send() {}, close() {} }; },
    setInterval: () => 0,
    setTimeout: () => 0,
    clearTimeout: () => {},
    addEventListener() {},
    confirm: () => false,
    alert() {},
    console,
    Date, Math, JSON, Number, String, Boolean, Array, Object, Map, Set, Promise, RegExp, Error,
    isNaN, parseInt, parseFloat, encodeURIComponent, decodeURIComponent,
  };
  sandbox.window = sandbox;
  sandbox.globalThis = sandbox;

  try {
    vm.runInNewContext(script, sandbox, { timeout: 5000 });
  } catch (err) {
    failures.push(`  ${hash || '(no hash)'}: ${err.name}: ${err.message}`);
  }
}

if (failures.length) {
  console.error('check_ui: the interface fails to start:');
  console.error(failures.join('\n'));
  process.exit(1);
}
console.error('check_ui: starts cleanly on every entry point');
