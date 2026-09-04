// Cmd/Ctrl+K "quick open" — a global keyboard shortcut, works from any
// page (this app does a full page reload per navigation, see router.js's
// own comment on why — so this just re-wires itself fresh on every load,
// same as everything else here). Filters against /api/nav/tree, the same
// flat, already visibility-gated document list the sidebar tree builds
// from (see nav.js) — fetched lazily on first open and cached for the
// rest of this page's lifetime, not re-fetched on every keystroke.
(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var el = WikiCommon.el;

  var docsCache = null;
  var overlay = null;
  var input = null;
  var listEl = null;
  var selectedIndex = 0;
  var filtered = [];

  function fetchDocs() {
    if (docsCache) return Promise.resolve(docsCache);
    return fetch(basePath() + "/api/nav/tree", { credentials: "same-origin" })
      .then(function (r) {
        return r.json();
      })
      .then(function (docs) {
        docsCache = docs;
        return docs;
      });
  }

  function buildOverlay() {
    overlay = el("div", { class: "quick-open-overlay", hidden: "" });
    var box = el("div", { class: "quick-open-box" });
    input = el("input", {
      type: "text",
      class: "quick-open-input",
      placeholder: "Jump to a document...",
      autocomplete: "off",
    });
    listEl = el("ul", { class: "quick-open-list" });
    box.appendChild(input);
    box.appendChild(listEl);
    overlay.appendChild(box);
    // Click on the dimmed backdrop closes it; a click inside `box` never
    // reaches this listener (it's a descendant, not the overlay itself),
    // so this can't be tripped by clicking the input/list.
    overlay.addEventListener("mousedown", function (evt) {
      if (evt.target === overlay) close();
    });
    input.addEventListener("input", renderResults);
    input.addEventListener("keydown", onInputKeydown);
    document.body.appendChild(overlay);
  }

  function renderResults() {
    var q = input.value.trim().toLowerCase();
    var docs = docsCache || [];
    filtered = !q
      ? docs.slice(0, 20)
      : docs
          .filter(function (d) {
            return (
              (d.title || "").toLowerCase().indexOf(q) !== -1 ||
              d.path.toLowerCase().indexOf(q) !== -1
            );
          })
          .slice(0, 20);
    selectedIndex = 0;
    listEl.innerHTML = "";
    if (filtered.length === 0) {
      listEl.appendChild(el("li", { class: "quick-open-empty", text: "No matches." }));
      return;
    }
    filtered.forEach(function (d, i) {
      var li = el("li", { class: "quick-open-item" + (i === 0 ? " selected" : "") });
      li.appendChild(document.createTextNode(d.title || d.path));
      li.appendChild(el("span", { class: "quick-open-path", text: " " + d.path }));
      if (d.visibility !== "public") li.appendChild(el("em", { text: " *" }));
      // mousedown, not click — a click fires AFTER the input's own blur,
      // and blur closes nothing here on its own, but preventDefault on
      // mousedown keeps focus in `input` throughout, which avoids a
      // flash of the list re-filtering against an empty/committed value
      // between blur and the actual navigation.
      li.addEventListener("mousedown", function (evt) {
        evt.preventDefault();
        navigateTo(d);
      });
      listEl.appendChild(li);
    });
  }

  function updateSelection(newIndex) {
    var items = listEl.querySelectorAll(".quick-open-item");
    if (items.length === 0) return;
    selectedIndex = ((newIndex % items.length) + items.length) % items.length;
    items.forEach(function (item, i) {
      item.classList.toggle("selected", i === selectedIndex);
    });
    items[selectedIndex].scrollIntoView({ block: "nearest" });
  }

  function navigateTo(doc) {
    window.location.href = basePath() + "/d/" + encodeVaultPath(doc.path);
  }

  function onInputKeydown(evt) {
    if (evt.key === "ArrowDown") {
      evt.preventDefault();
      updateSelection(selectedIndex + 1);
    } else if (evt.key === "ArrowUp") {
      evt.preventDefault();
      updateSelection(selectedIndex - 1);
    } else if (evt.key === "Enter") {
      evt.preventDefault();
      if (filtered[selectedIndex]) navigateTo(filtered[selectedIndex]);
    } else if (evt.key === "Escape") {
      evt.preventDefault();
      close();
    }
  }

  function open() {
    if (!overlay) buildOverlay();
    overlay.hidden = false;
    input.value = "";
    renderResults(); // shows the first 20 docs instantly if already cached
    fetchDocs().then(renderResults); // refreshes once the real list is in
    input.focus();
  }

  function close() {
    if (overlay) overlay.hidden = true;
  }

  // Toast UI Editor (edit.js) is a ProseMirror-based widget with its own
  // keymap — Ctrl/Cmd+K is a common "insert link" binding in that class
  // of editor. Rather than assume a conflict either way, just don't
  // intercept the shortcut at all while focus is inside the editor's own
  // contenteditable surface (WYSIWYG or Markdown mode) — quick-open stays
  // available from every OTHER field/page (including the rest of the
  // edit form: Path/Title/Tags), the editor keeps whatever its own K
  // binding does uninterrupted.
  function isInsideEditor(target) {
    return !!(
      target &&
      target.closest &&
      target.closest(".toastui-editor-contents, .ProseMirror, .toastui-editor-md-container")
    );
  }

  document.addEventListener("keydown", function (evt) {
    var isK = evt.key === "k" || evt.key === "K";
    if (!isK || !(evt.metaKey || evt.ctrlKey) || evt.shiftKey || evt.altKey) return;
    if (isInsideEditor(evt.target) || isInsideEditor(document.activeElement)) return;
    evt.preventDefault();
    if (overlay && !overlay.hidden) close();
    else open();
  });
})();
