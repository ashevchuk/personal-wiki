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
      buildForm(container, docPath, { isNew: true });
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
        buildForm(container, docPath, data);
      })
      .catch(function () {
        container.textContent = "Failed to load document.";
      });
  };

  function buildForm(container, docPath, data) {
    var isNew = data.isNew;
    document.getElementById("page-title").textContent =
      (isNew ? "New document" : "Edit — " + (data.title || docPath)) + " — wiki";

    container.innerHTML =
      renderBreadcrumbs(docPath) +
      '<form id="doc-form" autocomplete="off">' +
      '<div class="field-row">' +
      '<label>Path <input type="text" id="f-path" placeholder="e.g. notes/getting-started.md" required></label>' +
      '<label>Type <input type="text" id="f-type" placeholder="note"></label>' +
      "</div>" +
      '<div class="field-row">' +
      '<label>Title <input type="text" id="f-title" required></label>' +
      '<label>Tags <input type="text" id="f-tags" placeholder="comma, separated"></label>' +
      '<label class="visibility-toggle"><input type="checkbox" id="f-visibility"> Public</label>' +
      "</div>" +
      '<div id="editor"></div>' +
      '<div class="field-row">' +
      '<button type="submit" id="f-save">Save</button>' +
      '<button type="button" id="f-attach-btn">Attach file</button>' +
      '<input type="file" id="f-attach" hidden>' +
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

    // Attachments (POST /api/attachments/{docPath}) belong to an OWNING
    // document that has to exist already — refuse client-side too rather
    // than let a confusing 404 surface from inside the editor's own
    // upload UI.
    function currentDocPath() {
      return isNew ? null : pathInput.value.trim();
    }

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
          return { url: basePath() + "/assets/" + encodeVaultPath(info.path), filename: file.name };
        });
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

    var editor = new toastui.Editor({
      el: document.getElementById("editor"),
      height: "500px",
      initialEditType: "wysiwyg",
      previewStyle: "tab",
      theme: editorTheme,
      initialValue: data.body || "",
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
              alert("Image upload failed: " + err.message);
            });
        },
      },
      // Cosmetic only -- see youtube-embed-preview.js. The actual
      // ![youtube](url) -> <iframe> substitution happens server-side on
      // save+view (util/YouTubeEmbed.h, MarkdownRenderer.cpp); without
      // this hook the editor's own markdown engine would just try to
      // load the YouTube page URL as a normal <img> and show a broken
      // image icon while editing.
      customHTMLRenderer: {
        image: window.WikiYouTubeEmbedPreview.customImageRenderer,
      },
    });

    document.getElementById("doc-form").addEventListener("submit", function (evt) {
      evt.preventDefault();
      setStatus("Saving...", "");

      var path = pathInput.value.trim();
      if (!path) {
        setStatus("Path is required.", "error");
        return;
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
        body: editor.getMarkdown(),
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
          setStatus("Saved.", "ok");
          window.location.href = basePath() + "/d/" + encodeVaultPath(path);
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
          editor.insertText("[" + result.filename + "](" + result.url + ")");
          setStatus("Attached " + result.filename + ".", "ok");
        })
        .catch(function (err) {
          setStatus("Attachment failed: " + err.message, "error");
        });
    });

    var deleteBtn = document.getElementById("doc-delete-btn");
    if (deleteBtn && window.WikiDocument) {
      window.WikiDocument.wireDeleteButton(deleteBtn);
    }
  }
})();
