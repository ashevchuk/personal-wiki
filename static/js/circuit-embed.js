// Activates ```circuit fenced blocks on the document VIEW page --
// util/MarkdownRenderer.cpp's substituteCircuitBlocks already turns them
// into a bare <iframe class="circuit-embed" src="js/circuitjs1/
// circuitjs.html?..." data-circuit-xml="..."> (no query-string cct param
// -- see that function's own comment for why circuitjs1's own GWT
// decoder can't round-trip XML attribute syntax through a URL). This
// file's only job is loading the actual circuit into that iframe once
// its own app has finished booting, via circuitjs1's documented
// same-origin JS API (falstad.com/circuit/doc/js-interface.html):
// iframe.contentWindow.oncircuitjsloaded fires once, handing back the
// CircuitJS1 object, and CircuitJS1.importCircuit(xml, false) is the
// same call "Import from Text" makes internally.
//
// No lazy-loaded library here (unlike mermaid.js/Prism.js) -- circuitjs1
// is reached by the iframe's own src, not a <script> this page loads
// itself; the browser only fetches it at all once a document actually
// contains a circuit-embed iframe.
window.WikiCircuitEmbed = (function () {
  "use strict";

  function activate(iframe) {
    const xml = iframe.getAttribute("data-circuit-xml");
    if (!xml) return;
    // Same-origin by construction (the src is always this app's own
    // vendored js/circuitjs1/circuitjs.html -- see substituteCircuitBlocks'
    // own comment on that trust shape), so contentWindow is readable
    // synchronously right after the iframe lands in the DOM, well before
    // its own navigation finishes -- setting oncircuitjsloaded here is
    // not a race: the browser can't have fired it yet.
    iframe.contentWindow.oncircuitjsloaded = function () {
      iframe.contentWindow.CircuitJS1.importCircuit(xml, false);
    };
  }

  function renderIn(container) {
    const iframes = container.querySelectorAll(".circuit-embed");
    iframes.forEach(activate);
  }

  return { renderIn: renderIn };
})();
