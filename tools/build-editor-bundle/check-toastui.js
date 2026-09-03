// Sanity check for a vendored toastui-editor.min.js: evaluates it in a
// bare-bones sandbox that mimics a plain <script> tag's global scope (no
// module/exports/require/define in scope, forcing the UMD wrapper's
// browser-global branch — the exact branch that was silently broken for
// months, see the comment in fetch.sh) and confirms `toastui.Editor` ends
// up as an actual constructor function afterward, not just that the file
// downloaded and evaluated without throwing.
//
// Usage: node check-toastui.js <path-to-toastui-editor.min.js>
// Exit 0 = looks fine, exit 1 = broken (prints why).
"use strict";

const vm = require("vm");
const fs = require("fs");

const path = process.argv[2];
if (!path) {
  console.error("usage: node check-toastui.js <path-to-toastui-editor.min.js>");
  process.exit(2);
}

const code = fs.readFileSync(path, "utf8");

const sandbox = {};
sandbox.self = sandbox;
class FakeElement {}
sandbox.Element = FakeElement;
sandbox.HTMLElement = class extends FakeElement {};
sandbox.Node = class {};
sandbox.document = {
  createElement: () => ({
    style: {},
    classList: { add() {}, remove() {}, contains: () => false },
    setAttribute() {},
    appendChild() {},
    addEventListener() {},
  }),
  documentElement: { style: {} },
  addEventListener() {},
};
sandbox.navigator = { userAgent: "node", platform: "node" };
sandbox.window = sandbox;

vm.createContext(sandbox);
try {
  vm.runInContext(code, sandbox, { filename: path });
} catch (e) {
  console.error("BROKEN: script threw while evaluating as a plain <script> tag would:");
  console.error("  " + e.message);
  process.exit(1);
}

const Editor = sandbox.toastui && sandbox.toastui.Editor;
if (typeof Editor !== "function") {
  console.error(
    "BROKEN: toastui.Editor is " + typeof Editor + ", not a constructor function."
  );
  process.exit(1);
}

console.log("OK: toastui.Editor is a real constructor.");
