// Cosmetic-only fix for the same gap youtube-embed-preview.js plugs, for
// mermaid instead of YouTube: Toast UI Editor's own markdown engine has
// zero idea a ```mermaid fence means anything special (that's
// MarkdownRenderer::substituteMermaidBlocks, server-side, save+view only)
// — left alone, editing shows the raw diagram source as plain text the
// whole time. This file plugs the editor's `customHTMLRenderer.codeBlock`
// hook (edit.js wires it in) to render a real diagram while editing too.
//
// Empirically confirmed against the vendored 3.2.2 bundle (a throwaway
// sandbox page, not this app, logging the hook's actual arguments) before
// writing this: `node.info` carries the fence's language string exactly
// ("mermaid", not "language-mermaid"), `node.literal` the raw block
// content, `type: "text"` content is HTML-escaped by the library itself
// (verified with a real `<img src=x onerror=alert(1)>` payload -- it
// rendered as inert text, never executed) -- same trust boundary as
// customImageRenderer's own `attributes` values, just for text content
// instead. No XSS risk letting whatever text a document body contains
// flow straight into `content` below.
//
// ONE HARD LIMIT, confirmed the same way, not assumed: this hook's
// return value is only ever honored by the Markdown-mode "Preview" panel
// (`.toastui-editor-md-preview`) -- switch to WYSIWYG mode and the exact
// same ```mermaid block renders as an ordinary, still-editable code block
// with a plain "mermaid" language badge, this hook's return value
// silently discarded. Confirmed in the same sandbox: identical to how
// customImageRenderer's own doc comment describes ProseMirror collapsing
// a wrapped <img> back down to a bare one for an INLINE node -- here a
// BLOCK node (codeBlock) collapses the same way, back to its own default
// editable widget, for what's very likely the same underlying reason:
// WYSIWYG's canvas is a live, editable ProseMirror document, and a code
// block there has to stay a real editable text node or there'd be no way
// to keep typing the diagram source. Not a bug to chase -- there is no
// live-diagram-while-still-editable-as-text WYSIWYG mode to reach for
// with this hook alone.
window.WikiMermaidEditorPreview = (function () {
  "use strict";

  function customCodeBlockRenderer(node, context) {
    if (node.info !== "mermaid") return context.origin();
    return [
      { type: "openTag", tagName: "pre", classNames: ["mermaid"] },
      { type: "text", content: node.literal },
      { type: "closeTag", tagName: "pre" },
    ];
  }

  function isVisible(el) {
    return !!el && getComputedStyle(el).display !== "none";
  }

  // Called (debounced -- see edit.js) after every content change and
  // mode switch. Finds the Markdown-mode Preview panel -- the only DOM
  // location this hook's output can ever land, per the doc comment above
  // -- and re-renders whatever pre.mermaid blocks are in it, reusing the
  // exact same lazy-loader this app's document VIEW page already uses
  // (mermaid-render.js) rather than a second copy of that logic. A no-op,
  // fetching nothing, whenever the Preview panel isn't currently in the
  // DOM at all (WYSIWYG mode active, or the user has never switched to
  // Markdown mode yet), contains no mermaid block, OR -- confirmed live,
  // not assumed, see the real bug this guards against below -- is
  // currently display:none (the Write tab is active instead of Preview).
  //
  // REAL BUG this guard fixes: rendering into a display:none container
  // permanently breaks flowchart/graph-family diagrams. mermaid's dagre
  // layout engine calls getBBox() on the live DOM to size nodes, which
  // every Chromium/WebKit browser returns as all-zero for anything
  // inside a display:none ancestor -- the diagram "renders" (a real
  // <svg> lands in the DOM, no error, nothing in the console) but at
  // ~0px height, invisible. It never recovers even after the panel
  // becomes visible, because mermaid marks that code block as already
  // processed and silently skips it on every later mermaid.run() call.
  // sequenceDiagram-family blocks were NOT affected in live testing --
  // their layout is computed, not DOM-measured -- which is exactly why
  // this looked like a diagram-type-specific bug at first rather than a
  // visibility-timing one, until checked with real DevTools measurements
  // (getBoundingClientRect().height: ~59px for a broken flowchart vs.
  // ~434px for a correctly-sized sequence diagram from the same page).
  function refreshPreview() {
    var preview = document.querySelector(".toastui-editor-md-preview");
    if (preview && isVisible(preview) && window.WikiMermaid) {
      window.WikiMermaid.renderIn(preview);
    }
  }

  // Toast UI Editor's Write/Preview tab toggle (previewStyle: "tab") is a
  // DIFFERENT switch from changeMode's markdown/wysiwyg one -- confirmed
  // empirically (a throwaway sandbox, editor.on("changeMode", ...) never
  // fired from a Write/Preview click) that it fires no public editor
  // event at all; it just flips the Preview panel's own `display` CSS
  // property directly. This is what actually catches the "user typed a
  // diagram while on the Write tab, then switched to Preview" case that
  // change/changeMode listeners alone (see edit.js) can't: without this,
  // refreshPreview() above would correctly REFUSE to render into the
  // still-hidden panel while the user is on Write, and then nothing
  // triggers a follow-up render once they switch to Preview, since a
  // bare tab click emits no event for edit.js to listen to.
  function watchPreviewVisibility() {
    var editorRoot = document.getElementById("editor");
    if (!editorRoot) return;
    var wasVisible = false;
    var observer = new MutationObserver(function () {
      var preview = document.querySelector(".toastui-editor-md-preview");
      var visible = isVisible(preview);
      if (visible && !wasVisible) refreshPreview();
      wasVisible = visible;
    });
    observer.observe(editorRoot, {
      attributes: true,
      attributeFilter: ["style", "class"],
      subtree: true,
    });
  }

  return {
    customCodeBlockRenderer: customCodeBlockRenderer,
    refreshPreview: refreshPreview,
    watchPreviewVisibility: watchPreviewVisibility,
  };
})();
