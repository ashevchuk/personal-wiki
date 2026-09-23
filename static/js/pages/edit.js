// Edit page — GET /api/documents/{path} (a 404 means "new, unsaved
// document" rather than an error), builds the whole form dynamically
// (used to be static HTML from EditPage.csp), mounts Toast UI Editor,
// saves via the JSON API, and owns attachment upload wiring.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var getCookie = WikiCommon.getCookie;
  var encodeVaultPath = WikiCommon.encodeVaultPath;
  var escapeHtml = WikiCommon.escapeHtml;
  var errorFromResponse = WikiCommon.errorFromResponse;
  var renderBreadcrumbs = WikiCommon.renderBreadcrumbs;

  function setStatus(message, kind) {
    var el = document.getElementById("f-status");
    if (el) {
      el.textContent = message;
      el.className = kind || "";
    }
  }

  // Strips one layer of YAML scalar quoting ('...'/"...") — yaml-cpp
  // (FrontMatter.cpp's serializeFrontMatter, the server side of this same
  // round trip) only quotes a scalar when it actually needs to (a title
  // containing ": ", a leading special character, etc.), so most values
  // arrive bare and this is a no-op for them.
  function unquoteYaml(s) {
    var m = /^'(.*)'$/.exec(s) || /^"(.*)"$/.exec(s);
    return m ? m[1] : s;
  }

  // A small, deliberately flat parser for exactly the shape
  // FrontMatter::serializeFrontMatter (src/vault/FrontMatter.cpp) writes
  // on disk -- NOT a general YAML parser. Only pulls out title/type/
  // visibility/tags, the fields this form actually has inputs for; id/
  // created/updated are read back from the file too but intentionally
  // ignored here, same as the save path already never sends them --
  // the server owns those. Returns { fields: {...}, body: "..." };
  // `fields` only contains keys actually found, so a caller can tell
  // "not present in the file" apart from "present but empty" and leave
  // the corresponding form input alone in the former case. A file with
  // no "---" front-matter block at all is treated as pure body.
  function parseFrontMatterClientSide(raw) {
    if (raw.slice(0, 4) !== "---\n") return { fields: {}, body: raw };
    var closeIdx = raw.indexOf("\n---\n", 4);
    if (closeIdx === -1) return { fields: {}, body: raw };

    var block = raw.slice(4, closeIdx);
    var body = raw.slice(closeIdx + 5);
    var fields = {};

    block.split("\n").forEach(function (line) {
      var colon = line.indexOf(":");
      if (colon === -1) return;
      var key = line.slice(0, colon).trim();
      var value = line.slice(colon + 1).trim();
      if (key === "title" || key === "type" || key === "visibility") {
        fields[key] = unquoteYaml(value);
      } else if (key === "tags") {
        // Flow-sequence form only ("tags: [a, b]") -- the only shape
        // serializeFrontMatter ever writes (YAML::Flow). A block-style
        // list, hand-edited outside this app, is out of scope for this
        // deliberately flat parser -- falls through as an empty list
        // rather than guessing wrong.
        var m = /^\[(.*)\]$/.exec(value);
        fields.tags = m
          ? m[1]
              .split(",")
              .map(function (t) { return unquoteYaml(t.trim()); })
              .filter(function (t) { return t.length > 0; })
          : [];
      }
    });

    return { fields: fields, body: body };
  }

  window.WikiPages.renderEdit = function (container, docPath, session) {
    if (!session.authenticated) {
      window.location.href = basePath() + "/login";
      return;
    }

    // An empty path (sidebar "+ New") or one ending in "/" (a folder's
    // own "+ New Document", which pre-fills that folder as a prefix —
    // see folder.js::newDocument) can never be an EXISTING document's
    // path — skip the round-trip to the server and go straight to the
    // "new document" form instead of asking the backend to 404 on
    // something that was never going to be a real lookup. Also sidesteps
    // depending on how DocumentRoutes/PathGuard happen to answer a
    // trailing-slash or empty path, which was never a case worth
    // exercising over the network just to throw the answer away.
    if (!docPath || docPath.endsWith("/")) {
      buildForm(container, docPath, { isNew: true }, session);
      return;
    }

    fetch(basePath() + "/api/documents/" + encodeVaultPath(docPath), {
      credentials: "same-origin",
    })
      .then(function (resp) {
        if (resp.status === 404) return { isNew: true };
        if (!resp.ok) throw new Error("HTTP " + resp.status);
        return resp.json().then(function (doc) {
          doc.isNew = false;
          return doc;
        });
      })
      .then(function (data) {
        buildForm(container, docPath, data, session);
      })
      .catch(function () {
        container.textContent = "Failed to load document.";
      });
  };

  // A small typeahead/combobox, NOT the same shape as search.js's own
  // createMultiSelect (checkbox popover) -- that one filters over a
  // CLOSED set (you can only pick a tag/type that already exists in the
  // vault). Type and tags are free text -- the first document of a new
  // type, or the first use of a new tag, has to stay possible -- so this
  // is a plain text input with suggestions layered on top: pick one from
  // the dropdown, or just keep typing your own value, either works.
  //
  // input: the existing <input type="text"> to enhance in place (wrapped
  // in a positioning <span>, not replaced -- keeps its id/name/form
  // association intact). optionsUrl/jsonKey: where the suggestion list
  // comes from and which field of each returned object holds the value
  // (`/api/nav/types` returns {type, count}, `/api/nav/tags` returns
  // {tag, count} -- same shape as the tag cloud / search page's own
  // multiselect already consume, both already visibility-gated
  // server-side the same fail-safe-private way as everything else, so
  // this can never suggest a value that would leak what private content
  // exists). mode: "single" replaces the WHOLE field value (Type, which
  // only ever holds one value) -- "token" (Tags) only replaces the
  // comma-separated segment currently being typed, so picking a
  // suggestion for the second tag doesn't clobber the first one already
  // written.
  function createTypeahead(input, optionsUrl, jsonKey, mode) {
    var options = []; // [{value, count}]
    var filtered = [];
    var activeIndex = -1;

    var wrap = document.createElement("span");
    wrap.className = "typeahead-wrap";
    input.parentNode.insertBefore(wrap, input);
    wrap.appendChild(input);

    // Appended to document.body, NOT wrap -- a "portal", the standard
    // fix for exactly this class of problem. Toast UI Editor's own
    // WYSIWYG canvas (#editor) turns out to nest SEVERAL of its own
    // position:relative/absolute containers (.toastui-editor-main,
    // .toastui-editor-main-container, the ProseMirror root itself) --
    // found live via getComputedStyle walking that ancestor chain, not
    // guessed. That nesting made the dropdown's stacking order relative
    // to the editor's body text UNRELIABLE even after giving .field-row
    // its own explicit position+z-index (which reliably beat the
    // editor's plain TOOLBAR, itself unpositioned, but not consistently
    // the WYSIWYG canvas text several positioned layers deep inside
    // #editor). Rendering the menu as a direct child of <body> instead,
    // with `position: fixed` and JS-computed coordinates (positionMenu
    // below), sidesteps ancestor stacking-context interactions with the
    // editor entirely -- the menu's own stacking rank is then compared
    // directly against #editor at the SAME (body-level) stacking depth,
    // not nested three layers inside it.
    var menu = document.createElement("div");
    menu.className = "typeahead-menu";
    menu.hidden = true;
    document.body.appendChild(menu);

    // input.getBoundingClientRect() is already viewport-relative -- the
    // exact same coordinate space `position: fixed` uses, so no
    // scroll-offset math is needed. Re-run before every render (not just
    // once on open) since fixed positioning doesn't auto-follow the
    // input if the page scrolls or resizes while the dropdown is open.
    function positionMenu() {
      var r = input.getBoundingClientRect();
      menu.style.top = (r.bottom + 4) + "px";
      menu.style.left = r.left + "px";
      menu.style.minWidth = r.width + "px";
    }

    fetch(basePath() + optionsUrl, { credentials: "same-origin" })
      .then(function (resp) { return resp.ok ? resp.json() : []; })
      .then(function (data) {
        options = data.map(function (d) { return { value: d[jsonKey], count: d.count }; });
      })
      .catch(function () {
        // Suggestions are a convenience layered on top of a plain text
        // field, never load-bearing -- a failed fetch just means an
        // empty dropdown, the field itself still works exactly like a
        // normal <input type="text">.
      });

    // Deliberately caret-position-agnostic for "token" mode: always
    // treats the LAST comma-separated segment as "what's currently being
    // typed", regardless of where the text cursor actually is. Covers
    // the overwhelmingly common case (typing new tags onto the end of
    // the list) with far less code than real caret-aware token editing;
    // editing a tag in the MIDDLE of an existing list just won't show
    // suggestions scoped to it, a deliberate simplicity trade-off, not
    // an oversight.
    function currentToken() {
      if (mode === "single") {
        return { query: input.value, start: 0 };
      }
      var lastComma = input.value.lastIndexOf(",");
      var segment = input.value.slice(lastComma + 1);
      var query = segment.replace(/^\s+/, "");
      return { query: query, start: input.value.length - query.length };
    }

    function otherTokenValues() {
      if (mode !== "token") return [];
      var tok = currentToken();
      return input.value
        .slice(0, tok.start)
        .split(",")
        .map(function (s) { return s.trim().toLowerCase(); })
        .filter(Boolean);
    }

    function applyToken(value) {
      var tok = currentToken();
      input.value =
        input.value.slice(0, tok.start) + value + (mode === "token" ? ", " : "");
      closeMenu();
      input.focus();
    }

    function renderMenu() {
      menu.innerHTML = "";
      filtered.forEach(function (opt, i) {
        var row = document.createElement("div");
        row.className = "typeahead-option";
        var label = document.createElement("span");
        label.textContent = opt.value;
        row.appendChild(label);
        var count = document.createElement("span");
        count.className = "typeahead-count";
        count.textContent = "(" + opt.count + ")";
        row.appendChild(count);
        // mousedown, not click -- fires BEFORE the input's own blur
        // handler below, so applyToken() runs before closeMenu()-on-blur
        // would otherwise race it and discard the pick.
        row.addEventListener("mousedown", function (evt) {
          evt.preventDefault();
          applyToken(opt.value);
        });
        row.addEventListener("mouseenter", function () { setActive(i); });
        menu.appendChild(row);
      });
      menu.hidden = filtered.length === 0;
    }

    function setActive(i) {
      var rows = menu.children;
      for (var j = 0; j < rows.length; j++) rows[j].classList.remove("active");
      activeIndex = i;
      if (i >= 0 && i < rows.length) rows[i].classList.add("active");
    }

    function closeMenu() {
      menu.hidden = true;
      activeIndex = -1;
    }

    function refreshMenu() {
      var tok = currentToken();
      var q = tok.query.toLowerCase();
      var exclude = otherTokenValues();
      filtered = options.filter(function (opt) {
        var v = opt.value.toLowerCase();
        if (exclude.indexOf(v) !== -1) return false; // already picked -- don't re-suggest
        return q === "" || v.indexOf(q) !== -1;
      });
      activeIndex = -1;
      if (filtered.length > 0) positionMenu();
      renderMenu();
    }

    input.addEventListener("input", refreshMenu);
    input.addEventListener("focus", refreshMenu);
    input.addEventListener("blur", function () {
      // Delayed so a suggestion row's own mousedown handler gets a
      // chance to run first -- an immediate close here would hide the
      // menu before that click actually lands.
      setTimeout(closeMenu, 150);
    });
    input.addEventListener("keydown", function (evt) {
      if (menu.hidden) return;
      if (evt.key === "ArrowDown") {
        evt.preventDefault();
        setActive(Math.min(activeIndex + 1, filtered.length - 1));
      } else if (evt.key === "ArrowUp") {
        evt.preventDefault();
        setActive(Math.max(activeIndex - 1, 0));
      } else if (evt.key === "Enter") {
        if (activeIndex >= 0) {
          evt.preventDefault();
          applyToken(filtered[activeIndex].value);
        }
        // else: no suggestion highlighted -- fall through to this
        // input's normal behavior (same as before this feature existed).
      } else if (evt.key === "Escape") {
        closeMenu();
      }
    });
  }

  // Toast UI's WYSIWYG code-block widget already has a language badge
  // that pops a plain <input> (createLanguageEditor in the vendored
  // bundle — no language list, no Editor option to feed it one). Don't
  // fork that min.js: watch for the input appearing and hang the same
  // body-portal typeahead the Type/Tags fields already use onto it.
  // Markdown-mode fences stay typed by hand (```cpp); this is the
  // WYSIWYG picker only.
  //
  // List is the Prism bundle (static/js/prism/VENDORED.md) plus the
  // aliases that bundle actually registers (js/ts/py/html/sh/yml) plus
  // mermaid/query, which aren't Prism grammars but are real fences this
  // app understands. An unrecognized value still saves — Toast UI
  // writes whatever string is in the input, same as before.
  var CODE_FENCE_LANGUAGES = [
    "bash", "c", "cpp", "css", "go", "html", "javascript", "js", "json",
    "mermaid", "py", "python", "query", "rust", "sh", "sql", "toml",
    "ts", "typescript", "yaml", "yml",
  ];

  function attachCodeBlockLanguageTypeahead(input) {
    if (input.dataset.wikiLangTa === "1") return;
    input.dataset.wikiLangTa = "1";

    var filtered = [];
    var activeIndex = -1;
    var menu = document.createElement("div");
    menu.className = "typeahead-menu codeblock-lang-menu";
    menu.hidden = true;
    document.body.appendChild(menu);

    function positionMenu() {
      var r = input.getBoundingClientRect();
      menu.style.top = r.bottom + 4 + "px";
      menu.style.left = r.left + "px";
      menu.style.minWidth = Math.max(r.width, 160) + "px";
    }

    function closeMenu() {
      menu.hidden = true;
      activeIndex = -1;
    }

    function destroyMenu() {
      closeMenu();
      if (menu.parentNode) menu.parentNode.removeChild(menu);
    }

    function setActive(i) {
      var rows = menu.children;
      for (var j = 0; j < rows.length; j++) rows[j].classList.remove("active");
      activeIndex = i;
      if (i >= 0 && i < rows.length) rows[i].classList.add("active");
    }

    // Commit through Toast UI's own blur handler (changeLanguage), not
    // by poking ProseMirror ourselves — that handler also tears the
    // input down. Set the value first, then blur.
    function pick(value) {
      input.value = value;
      destroyMenu();
      input.blur();
    }

    function renderMenu() {
      menu.innerHTML = "";
      filtered.forEach(function (lang, i) {
        var row = document.createElement("div");
        row.className = "typeahead-option";
        row.textContent = lang;
        row.addEventListener("mousedown", function (evt) {
          evt.preventDefault();
          pick(lang);
        });
        row.addEventListener("mouseenter", function () {
          setActive(i);
        });
        menu.appendChild(row);
      });
      menu.hidden = filtered.length === 0;
    }

    function refreshMenu() {
      var q = (input.value || "").toLowerCase();
      filtered = CODE_FENCE_LANGUAGES.filter(function (lang) {
        return q === "" || lang.indexOf(q) !== -1;
      });
      activeIndex = -1;
      if (filtered.length > 0) positionMenu();
      renderMenu();
    }

    input.addEventListener("input", refreshMenu);
    input.addEventListener("focus", refreshMenu);
    // Toast UI focuses this input via setTimeout(0) after creating it.
    // The MutationObserver callback can run after that focus already
    // happened, so the focus listener above would miss the first open.
    if (document.activeElement === input) refreshMenu();
    input.addEventListener("blur", function () {
      setTimeout(destroyMenu, 150);
    });
    // capture: Toast UI's own keydown always commits on Enter
    // (preventDefault + changeLanguage). When a suggestion is
    // highlighted we have to win that race, otherwise Enter saves
    // whatever prefix was typed ("c") instead of the pick ("cpp").
    input.addEventListener(
      "keydown",
      function (evt) {
        if (menu.hidden) return;
        if (evt.key === "ArrowDown") {
          evt.preventDefault();
          evt.stopPropagation();
          setActive(Math.min(activeIndex + 1, filtered.length - 1));
        } else if (evt.key === "ArrowUp") {
          evt.preventDefault();
          evt.stopPropagation();
          setActive(Math.max(activeIndex - 1, 0));
        } else if (evt.key === "Enter" && activeIndex >= 0) {
          evt.preventDefault();
          evt.stopPropagation();
          pick(filtered[activeIndex]);
        } else if (evt.key === "Escape") {
          closeMenu();
        }
      },
      true
    );
  }

  function watchCodeBlockLanguageInputs(editorRoot) {
    function scan() {
      var nodes = editorRoot.querySelectorAll(
        ".toastui-editor-ww-code-block-language input"
      );
      for (var i = 0; i < nodes.length; i++) {
        attachCodeBlockLanguageTypeahead(nodes[i]);
      }
    }
    scan();
    var obs = new MutationObserver(scan);
    obs.observe(editorRoot, { childList: true, subtree: true });
  }

  function buildForm(container, docPath, data, session) {
    var isNew = data.isNew;
    document.getElementById("page-title").textContent =
      (isNew ? "New document" : "Edit — " + (data.title || docPath)) + " — wiki";

    container.innerHTML =
      renderBreadcrumbs(docPath) +
      '<form id="doc-form" autocomplete="off">' +
      '<div class="edit-meta">' +
      '<div class="edit-meta-fields">' +
      '<div class="field-row">' +
      '<label>Path <input type="text" id="f-path" placeholder="e.g. notes/getting-started.md" required></label>' +
      '<label>Type <input type="text" id="f-type" placeholder="note"></label>' +
      "</div>" +
      '<div class="field-row">' +
      '<label>Title <input type="text" id="f-title" required></label>' +
      '<label>Tags <input type="text" id="f-tags" placeholder="comma, separated"></label>' +
      '<label class="visibility-toggle"><input type="checkbox" id="f-visibility"> Public</label>' +
      "</div>" +
      "</div>" +
      '<div class="edit-attachments-slot" hidden>' +
      '<div id="edit-attachments" class="edit-attachments" hidden></div>' +
      "</div>" +
      "</div>" +
      '<div id="editor"></div>' +
      '<div class="field-row" id="edit-actions-row">' +
      '<button type="submit" id="f-save">Save</button>' +
      (session && session.agentEnabled
        ? '<button type="button" id="f-draft-btn">Draft</button>'
        : "") +
      '<button type="button" id="f-attach-btn">Attach file</button>' +
      '<input type="file" id="f-attach" hidden>' +
      '<button type="button" id="f-upload-btn">Upload</button>' +
      '<input type="file" id="f-upload" hidden accept=".md,text/markdown">' +
      (isNew
        ? ""
        : '<button type="button" id="doc-delete-btn" data-path="' +
          escapeHtml(docPath) +
          '">Delete</button>') +
      '<span id="f-status" role="status"></span>' +
      "</div>" +
      "</form>";

    var pathInput = document.getElementById("f-path");
    var titleInput = document.getElementById("f-title");
    var tagsInput = document.getElementById("f-tags");
    var typeInput = document.getElementById("f-type");
    var visibilityInput = document.getElementById("f-visibility");

    pathInput.value = isNew ? docPath : data.path;
    pathInput.readOnly = !isNew;
    titleInput.value = data.title || "";
    tagsInput.value = (data.tags || []).join(", ");
    typeInput.value = data.type || "";
    visibilityInput.checked = data.visibility === "public";

    createTypeahead(typeInput, "/api/nav/types", "type", "single");
    createTypeahead(tagsInput, "/api/nav/tags", "tag", "token");

    // Attachments (POST /api/attachments/{docPath}) belong to an OWNING
    // document that has to exist already — refuse client-side too rather
    // than let a confusing 404 surface from inside the editor's own
    // upload UI.
    function currentDocPath() {
      return isNew ? null : pathInput.value.trim();
    }

    var ICON_DOWNLOAD =
      '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">' +
      '<path fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" ' +
      'stroke-linejoin="round" d="M8 2v8m-3-2.5L8 11l3-3.5M3 13.5h10"/></svg>';
    var ICON_DELETE =
      '<svg viewBox="0 0 16 16" width="14" height="14" aria-hidden="true">' +
      '<path fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" ' +
      'stroke-linejoin="round" d="M3 4h10M6.5 4V3h3v1M5 4l.5 9h5L11 4"/></svg>';

    function attachmentBasename(p) {
      var i = p.lastIndexOf("/");
      return i < 0 ? p : p.slice(i + 1);
    }

    function loadAttachments() {
      var host = document.getElementById("edit-attachments");
      var slot = host && host.parentElement;
      function hideList() {
        if (host) {
          host.hidden = true;
          host.innerHTML = "";
        }
        if (slot) slot.hidden = true;
      }
      if (!host) return Promise.resolve();
      var docP = currentDocPath();
      if (!docP) {
        hideList();
        return Promise.resolve();
      }
      return fetch(basePath() + "/api/attachments/" + encodeVaultPath(docP), {
        credentials: "same-origin",
        cache: "no-store",
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          return resp.json();
        })
        .then(function (data) {
          var files = (data && data.files) || [];
          if (!files.length) {
            hideList();
          } else {
            host.hidden = false;
            if (slot) slot.hidden = false;
            host.innerHTML = files
              .map(function (f) {
                var name = attachmentBasename(f.path);
                var href = basePath() + "/assets/" + encodeVaultPath(f.path);
                return (
                  '<div class="edit-att-row" data-path="' +
                  escapeHtml(f.path) +
                  '" data-mime="' +
                  escapeHtml(f.mimeType || "") +
                  '">' +
                  '<span class="edit-att-name" title="' +
                  escapeHtml(name) +
                  ' — double-click to insert">' +
                  escapeHtml(name) +
                  "</span>" +
                  '<a class="edit-att-icon" href="' +
                  escapeHtml(href) +
                  '" download="' +
                  escapeHtml(name) +
                  '" title="Download" aria-label="Download ' +
                  escapeHtml(name) +
                  '">' +
                  ICON_DOWNLOAD +
                  "</a>" +
                  '<button type="button" class="edit-att-icon edit-att-del" title="Delete" aria-label="Delete ' +
                  escapeHtml(name) +
                  '">' +
                  ICON_DELETE +
                  "</button>" +
                  "</div>"
                );
              })
              .join("");
          }
          if (editor && editor.setHeight) editor.setHeight(computeEditorHeight());
        })
        .catch(function () {
          hideList();
        });
    }

    document.getElementById("edit-attachments").addEventListener("click", function (ev) {
      var btn = ev.target.closest(".edit-att-del");
      if (!btn) return;
      var row = btn.closest(".edit-att-row");
      if (!row) return;
      var assetPath = row.getAttribute("data-path");
      var name = row.querySelector(".edit-att-name");
      name = name ? name.textContent : assetPath;
      WikiDialog.confirm(
        'Delete attachment "' + name + '"? The file is removed; markdown links to it stay in the document.',
        { danger: true, okLabel: "Delete" }
      ).then(function (ok) {
        if (!ok) return;
        fetch(basePath() + "/api/attachments/" + encodeVaultPath(assetPath), {
          method: "DELETE",
          headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
          credentials: "same-origin",
        })
          .then(function (resp) {
            if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
            setStatus("Deleted " + name + ".", "ok");
            return loadAttachments();
          })
          .catch(function (err) {
            setStatus("Delete failed: " + err.message, "error");
          });
      });
    });

    // Raster images become an image node / ![name](url) (svg is
    // image/svg+xml but not an <img> here — same rule as
    // AttachToDocument); everything else a regular link.
    // insertText("[name](url)") is correct in Markdown mode but in
    // WYSIWYG it dumps the brackets as literal text (Toast UI then
    // auto-linkifies the [name] piece and leaves "](url)" visible).
    // editor.exec("addLink"/"addImage") is the same command the
    // toolbar's own link/image popups use, and it writes the right
    // node in BOTH modes (markdown command inserts the []() source).
    function isInlineImageName(mime, name) {
      if (!mime || mime.indexOf("image/") !== 0) return false;
      if (mime === "image/svg+xml") return false;
      var lower = (name || "").toLowerCase();
      return !/\.svg$/.test(lower);
    }

    function insertAttachmentLink(url, name, mime) {
      editor.focus();
      if (isInlineImageName(mime, name)) {
        editor.exec("addImage", { imageUrl: url, altText: name || "image" });
      } else {
        editor.exec("addLink", { linkUrl: url, linkText: name || url });
      }
    }

    document.getElementById("edit-attachments").addEventListener("dblclick", function (ev) {
      var nameEl = ev.target.closest(".edit-att-name");
      if (!nameEl) return;
      var row = nameEl.closest(".edit-att-row");
      if (!row) return;
      ev.preventDefault();
      var sel = window.getSelection();
      if (sel && sel.removeAllRanges) sel.removeAllRanges();
      insertAttachmentLink(
        basePath() + "/assets/" + encodeVaultPath(row.getAttribute("data-path")),
        nameEl.textContent,
        row.getAttribute("data-mime") || ""
      );
      setStatus("Inserted " + nameEl.textContent + ".", "ok");
    });

    // Shared by both attachment paths below (drag/paste-an-image and the
    // explicit "Attach file" button). url is the ABSOLUTE /assets/...
    // path — this app's own /d/{path} view isn't a directory-shaped URL,
    // so a bare relative link (the way a hand-edited markdown file
    // outside the web UI might use it) wouldn't resolve correctly
    // through the browser here.
    function uploadAttachment(file) {
      var docP = currentDocPath();
      if (!docP) {
        return Promise.reject(
          new Error("Save the document first — attachments need an existing document to attach to.")
        );
      }
      var form = new FormData();
      form.append("file", file, file.name);
      return fetch(basePath() + "/api/attachments/" + encodeVaultPath(docP), {
        method: "POST",
        headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
        credentials: "same-origin",
        body: form,
      }).then(function (resp) {
        if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
        return resp.json().then(function (info) {
          return { url: basePath() + "/assets/" + encodeVaultPath(info.path), filename: file.name, mimeType: info.mimeType || file.type || "" };
        });
      }).then(function (result) {
        return loadAttachments().then(function () { return result; });
      });
    }

    // Toast UI's own `theme` option ("light"/"dark") is separate from and
    // NOT auto-derived from this app's own CSS — shell.html unconditionally
    // loads both toastui-editor.css and toastui-editor-dark.css (the option
    // just toggles a `.toastui-editor-dark` class the library adds itself),
    // so nothing stops picking either one per site theme. This used to be
    // hardcoded to "dark" — harmless back when the only site theme WAS a
    // dark one (green.css), genuinely wrong once classic.css (a light
    // theme) existed: a solid black editor panel sitting in the middle of
    // an otherwise white page. `<html data-theme="...">` is set
    // synchronously by shell.html's own bootstrap script before this file
    // ever runs, so it's already there to read — green/dark both want the
    // editor's dark theme (both have dark page backgrounds), only classic
    // wants Toast UI's own light one ("light" is the library's actual
    // documented/default value, confirmed in the vendored
    // toastui-editor.min.js itself — not "default").
    var siteTheme = document.documentElement.getAttribute("data-theme");
    var editorTheme = siteTheme === "classic" ? "light" : "dark";

    // Fill whatever vertical space is actually left in the viewport below
    // the editor's own top (path/title/tags fields above it) instead of a
    // fixed height that leaves a big dead gap on a tall screen and forces
    // a second, page-level scrollbar on a short one. Measured live, not
    // guessed: everything below #editor (the Save/Attach/Delete row) is
    // already in the DOM by this point (built in one innerHTML= call
    // above), so its real height is known before Toast UI Editor ever
    // mounts. 300px floor covers a very short viewport (e.g. a phone in
    // landscape) where the arithmetic would otherwise go negative.
    var actionsRow = document.getElementById("edit-actions-row");
    function computeEditorHeight() {
      var top = document.getElementById("editor").getBoundingClientRect().top;
      var actionsHeight = actionsRow ? actionsRow.getBoundingClientRect().height : 0;
      // 32px: the same breathing room #editor's own margin-bottom (see
      // edit.css) already reserves below it, plus a little more so the
      // action row never sits flush against the editor's own border.
      var available = window.innerHeight - top - actionsHeight - 32;
      return Math.max(available, 300) + "px";
    }

    var editor = new toastui.Editor({
      el: document.getElementById("editor"),
      height: computeEditorHeight(),
      initialEditType: "wysiwyg",
      previewStyle: "tab",
      theme: editorTheme,
      initialValue: window.WikiCommon.wikiBodyToEditor(data.body || ""),
      // Fires on paste/drag-drop of an image straight into the editor.
      // Toast UI's default with no hook is to inline the image as a
      // base64 data URI in the markdown — bad for a wiki (bloats the
      // document, no de-dup, no visibility gating on the image). Route
      // it through the same attachment pipeline as everything else.
      hooks: {
        addImageBlobHook: function (blob, callback) {
          uploadAttachment(blob)
            .then(function (result) {
              callback(result.url, result.filename);
            })
            .catch(function (err) {
              WikiDialog.alert("Image upload failed: " + err.message);
            });
        },
      },
      // Cosmetic only -- see youtube-embed-preview.js. The actual
      // ![youtube](url) -> <iframe> substitution happens server-side on
      // save+view (util/YouTubeEmbed.h, MarkdownRenderer.cpp); without
      // this hook the editor's own markdown engine would just try to
      // load the YouTube page URL as a normal <img> and show a broken
      // image icon while editing.
      //
      // codeBlock: same idea, for ```mermaid blocks -- see
      // mermaid-editor-preview.js for what it actually renders and its
      // one real limitation (Markdown-mode Preview panel only, never the
      // WYSIWYG canvas -- confirmed empirically, not assumed).
      customHTMLRenderer: {
        image: window.WikiYouTubeEmbedPreview.customImageRenderer,
        codeBlock: window.WikiMermaidEditorPreview.customCodeBlockRenderer,
      },
    });

    // Re-render mermaid diagrams in the Markdown-mode Preview panel after
    // every content change and mode switch (both debounced -- a diagram
    // re-render isn't free, and `change` fires on every single keystroke).
    // See mermaid-editor-preview.js's own comment for why this can only
    // ever affect the Preview panel, never the WYSIWYG canvas. No listener
    // cleanup needed here either, same reasoning as the resize listener
    // above: this app does a real full browser navigation on every page
    // change, so this whole JS context is torn down by the browser itself
    // when the user navigates away.
    var mermaidPreviewTimer = null;
    function scheduleMermaidPreviewRefresh() {
      if (mermaidPreviewTimer) clearTimeout(mermaidPreviewTimer);
      mermaidPreviewTimer = setTimeout(function () {
        window.WikiMermaidEditorPreview.refreshPreview();
      }, 300);
    }
    editor.on("change", scheduleMermaidPreviewRefresh);
    editor.on("changeMode", scheduleMermaidPreviewRefresh);
    // Also cover the initial mount: an existing document opened straight
    // into Markdown mode (or with a saved user preference -- editType
    // isn't currently persisted, but this costs nothing to be correct
    // either way) should show its diagrams immediately, not only after
    // the first edit.
    scheduleMermaidPreviewRefresh();
    // Also watch for the Write/Preview tab toggle specifically -- a bare
    // tab click fires neither `change` nor `changeMode` (confirmed
    // empirically), so without this, a diagram typed while on the Write
    // tab would never actually render once the user switches to Preview.
    // See mermaid-editor-preview.js's own comment for the real,
    // previously-live bug (flowchart diagrams silently breaking) this
    // also fixes as a side effect.
    window.WikiMermaidEditorPreview.watchPreviewVisibility();
    watchCodeBlockLanguageInputs(document.getElementById("editor"));

    // Re-fit on viewport resize (window resize, or a mobile browser's
    // address bar showing/hiding changing innerHeight) -- debounced since
    // a drag-resize fires this dozens of times a second and setHeight()
    // forces Toast UI Editor to redo its own internal layout each call.
    // No listener cleanup needed: this app does a real full browser
    // navigation on every page change (see router.js's own comment), so
    // this whole JS context — this listener included — is torn down by
    // the browser itself the moment the user navigates away.
    var resizeTimer = null;
    window.addEventListener("resize", function () {
      if (resizeTimer) clearTimeout(resizeTimer);
      resizeTimer = setTimeout(function () {
        editor.setHeight(computeEditorHeight());
      }, 150);
    });

    loadAttachments();

    var draftBtn = document.getElementById("f-draft-btn");
    if (draftBtn && window.WikiAgent) {
      draftBtn.addEventListener("click", function () {
        window.WikiAgent.open({
          snapshot: function () {
            var rawSelection = "";
            try {
              rawSelection = editor.getSelectedText() || "";
            } catch (e) {
              rawSelection = "";
            }
            return {
              path: pathInput.value.trim(),
              title: titleInput.value.trim(),
              type: typeInput.value.trim(),
              tags: tagsInput.value
                .split(",")
                .map(function (t) {
                  return t.trim();
                })
                .filter(function (t) {
                  return t.length > 0;
                }),
              body: window.WikiCommon.wikiBodyFromEditor(editor.getMarkdown()),
              selection: rawSelection
                ? window.WikiCommon.wikiBodyFromEditor(rawSelection)
                : "",
              isNew: isNew,
            };
          },
          currentBody: function () {
            return window.WikiCommon.wikiBodyFromEditor(editor.getMarkdown());
          },
          applyDraft: function (draft) {
            if (draft.path && isNew && !pathInput.readOnly) {
              pathInput.value = draft.path;
            }
            if (draft.title) titleInput.value = draft.title;
            if (draft.type) typeInput.value = draft.type;
            if (draft.tags && draft.tags.length) {
              tagsInput.value = draft.tags.join(", ");
            }
            if (typeof draft.body === "string") {
              editor.setMarkdown(window.WikiCommon.wikiBodyToEditor(draft.body));
            }
          },
          appendToBody: function (text) {
            var body = window.WikiCommon.wikiBodyFromEditor(editor.getMarkdown());
            if (body && body.charAt(body.length - 1) !== "\n") body += "\n";
            if (body) body += "\n";
            body += text || "";
            editor.setMarkdown(window.WikiCommon.wikiBodyToEditor(body));
          },
          replaceInBody: function (find, replacement) {
            if (!find) return false;
            var body = window.WikiCommon.wikiBodyFromEditor(editor.getMarkdown());
            var idx = body.indexOf(find);
            if (idx < 0) return false;
            var next = body.slice(0, idx) + replacement + body.slice(idx + find.length);
            editor.setMarkdown(window.WikiCommon.wikiBodyToEditor(next));
            return true;
          },
        });
      });
    }

    document.getElementById("doc-form").addEventListener("submit", function (evt) {
      evt.preventDefault();
      setStatus("Saving...", "");

      var path = pathInput.value.trim();
      if (!path) {
        setStatus("Path is required.", "error");
        return;
      }
      if (isNew) {
        path = path.replace(/\/+$/, "");
        if (!path) {
          setStatus("Path is required.", "error");
          return;
        }
        if (/\.md$/i.test(path)) path = path.replace(/\.md$/i, ".md");
        else path = path + ".md";
        pathInput.value = path;
      }

      var payload = {
        title: titleInput.value.trim(),
        tags: tagsInput.value
          .split(",")
          .map(function (t) {
            return t.trim();
          })
          .filter(function (t) {
            return t.length > 0;
          }),
        type: typeInput.value.trim(),
        visibility: visibilityInput.checked ? "public" : "private",
        body: window.WikiCommon.wikiBodyFromEditor(editor.getMarkdown()),
      };

      var url = isNew
        ? basePath() + "/api/documents"
        : basePath() + "/api/documents/" + encodeVaultPath(path);
      if (isNew) payload.path = path;

      fetch(url, {
        method: isNew ? "POST" : "PUT",
        headers: {
          "Content-Type": "application/json",
          "X-CSRF-Token": getCookie("wiki_csrf_token"),
        },
        credentials: "same-origin",
        body: JSON.stringify(payload),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          return resp.json().then(function (data) {
            var savedPath = (data && data.path) || path;
            setStatus("Saved.", "ok");
            if (window.WikiAgent && window.WikiAgent.endSession) {
              window.WikiAgent.endSession();
            }
            window.location.href = basePath() + "/d/" + encodeVaultPath(savedPath);
          });
        })
        .catch(function (err) {
          setStatus("Save failed: " + err.message, "error");
        });
    });

    // Explicit "Attach file" button — covers anything addImageBlobHook
    // doesn't (PDFs, zips, audio/video, anything at all now — see
    // AttachmentService, no extension policy on upload anymore). Inserts
    // a markdown link at the cursor rather than trying to render inline.
    var attachInput = document.getElementById("f-attach");
    var attachBtn = document.getElementById("f-attach-btn");
    attachBtn.addEventListener("click", function () {
      attachInput.click();
    });
    attachInput.addEventListener("change", function () {
      var file = attachInput.files[0];
      attachInput.value = ""; // allow re-selecting the same file later
      if (!file) return;
      setStatus("Uploading " + file.name + "...", "");
      uploadAttachment(file)
        .then(function (result) {
          insertAttachmentLink(result.url, result.filename, result.mimeType);
          setStatus("Attached " + result.filename + ".", "ok");
        })
        .catch(function (err) {
          setStatus("Attachment failed: " + err.message, "error");
        });
    });

    // "Upload" button — imports a whole .md file (front-matter + body)
    // as this document's content, the reverse of the view page's
    // "Download" button. Purely client-side: FileReader reads the file,
    // parseFrontMatterClientSide splits it, nothing touches the network
    // until the user hits Save themselves. `path` is deliberately left
    // alone -- for an existing document it's read-only anyway, and for a
    // new one it may already carry a folder prefix from "+ New" that the
    // uploaded file has no opinion about.
    var uploadInput = document.getElementById("f-upload");
    var uploadBtn = document.getElementById("f-upload-btn");
    uploadBtn.addEventListener("click", function () {
      uploadInput.click();
    });
    uploadInput.addEventListener("change", function () {
      var file = uploadInput.files[0];
      uploadInput.value = ""; // allow re-selecting the same file later
      if (!file) return;

      function applyFile() {
        var reader = new FileReader();
        reader.onload = function () {
          var parsed = parseFrontMatterClientSide(String(reader.result));
          var f = parsed.fields;
          if (f.title !== undefined) titleInput.value = f.title;
          if (f.type !== undefined) typeInput.value = f.type;
          if (f.visibility !== undefined) visibilityInput.checked = f.visibility === "public";
          if (f.tags !== undefined) tagsInput.value = f.tags.join(", ");
          editor.setMarkdown(parsed.body);
          setStatus("Loaded " + file.name + ".", "ok");
        };
        reader.onerror = function () {
          setStatus("Failed to read " + file.name + ".", "error");
        };
        reader.readAsText(file);
      }

      if (editor.getMarkdown().trim() !== "") {
        WikiDialog.confirm('Replace the current editor content with "' + file.name + '"?', {
          okLabel: "Replace",
        }).then(function (ok) {
          if (ok) applyFile();
        });
        return;
      }
      applyFile();
    });

    var deleteBtn = document.getElementById("doc-delete-btn");
    if (deleteBtn && window.WikiDocument) {
      window.WikiDocument.wireDeleteButton(deleteBtn);
    }
  }
})();
