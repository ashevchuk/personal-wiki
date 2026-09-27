// Same idea as mermaid-editor-preview.js, for ```query blocks instead of
// ```mermaid: Toast UI Editor's own markdown engine has zero idea a
// ```query fence means anything (that's MarkdownRenderer's own
// substituteQueryBlocks marker, resolved server-side into a real table only
// on the document VIEW page) -- left alone, editing shows the raw DSL text.
// This plugs the same customHTMLRenderer.codeBlock hook (edit.js composes
// this with mermaid's own handler into one dispatcher, since Toast UI only
// takes a single function per node type) to render a live table while
// editing too, reusing query-block.js's actual fetch+render engine --
// nothing about rendering a query result is reimplemented here.
//
// One real difference from mermaid, worth calling out because it shaped the
// debounce timing edit.js uses for this specifically: a mermaid re-render is
// a free, local computation, but a query-block re-render is a genuine
// network round trip to /api/query (a real SQLite query on the other end)
// for every visible query block. Debouncing on "change" the same way still
// works, just with a longer interval than mermaid's -- see edit.js.
window.WikiQueryEditorPreview = (function () {
  "use strict";

  function customCodeBlockRenderer(node, context) {
    if (node.info !== "query") return context.origin();
    return [
      { type: "openTag", tagName: "pre", classNames: ["query"] },
      { type: "text", content: node.literal },
      { type: "closeTag", tagName: "pre" },
    ];
  }

  function isVisible(el) {
    return !!el && getComputedStyle(el).display !== "none";
  }

  // Called (debounced -- see edit.js) after every content change and mode
  // switch. Finds the Markdown-mode Preview panel -- same one-DOM-location
  // limitation as mermaid's own hook, for the same reason (WYSIWYG's canvas
  // is a live ProseMirror document; a code block there has to stay a real
  // editable text node, so this hook's return value is discarded there) --
  // and re-renders whatever pre.query blocks are in it via the exact same
  // renderIn() the document VIEW page uses. A no-op when the Preview panel
  // isn't in the DOM at all, has no query block, or is currently
  // display:none (the Write tab is active instead of Preview) -- no reason
  // to fire a real HTTP request into a panel nobody can see.
  function refreshPreview() {
    var preview = document.querySelector(".toastui-editor-md-preview");
    if (preview && isVisible(preview) && window.WikiQueryBlock) {
      window.WikiQueryBlock.renderIn(preview);
    }
  }

  // Same Write/Preview-tab-toggle gap mermaid's own watcher exists for: that
  // toggle (previewStyle: "tab") flips the Preview panel's `display` CSS
  // directly and fires no editor event at all, so without this, a query
  // block typed while on the Write tab would never render once the user
  // switches to Preview.
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
