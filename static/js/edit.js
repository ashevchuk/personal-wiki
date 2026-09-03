// Edit page glue: hydrates from #doc-data (a JSON <script> tag, not
// executable — see EditPage.csp), mounts Toast UI Editor, and saves via
// the JSON API. No build step: this is loaded as a plain <script>, same
// as the vendored editor bundle.
(function () {
  "use strict";

  function getCookie(name) {
    const match = document.cookie.match(
      new RegExp("(?:^|; )" + name.replace(/([.$?*|{}()[\]\\/+^])/g, "\\$1") + "=([^;]*)")
    );
    return match ? decodeURIComponent(match[1]) : "";
  }

  // Encodes each path segment individually so legitimate '/' separators
  // survive while everything else in a segment gets properly escaped.
  function encodeVaultPath(path) {
    return path.split("/").map(encodeURIComponent).join("/");
  }

  function setStatus(message, kind) {
    const el = document.getElementById("f-status");
    el.textContent = message;
    el.className = kind || "";
  }

  document.addEventListener("DOMContentLoaded", function () {
    const data = JSON.parse(document.getElementById("doc-data").textContent);
    // Mirrors [server].base_path (see AppConfig::basePath / BasePath.h) —
    // every path this file builds itself (save target, post-save redirect)
    // needs the same prefix the server-rendered chrome already carries, or
    // they'd point at the wrong place when this app is reverse-proxied
    // under a subpath.
    const basePath = data.basePath || "";

    const pathInput = document.getElementById("f-path");
    const titleInput = document.getElementById("f-title");
    const tagsInput = document.getElementById("f-tags");
    const typeInput = document.getElementById("f-type");
    const visibilityInput = document.getElementById("f-visibility");

    pathInput.value = data.path || "";
    pathInput.readOnly = !data.isNew;
    titleInput.value = data.title || "";
    tagsInput.value = (data.tags || []).join(", ");
    typeInput.value = data.type || "";
    visibilityInput.checked = data.visibility === "public";

    // Attachments (POST /api/attachments/{docPath}, see
    // vault/AttachmentService.h) belong to an OWNING document that has to
    // exist already — there's no "attach to a document that isn't saved
    // yet" case on the server side, so refuse client-side too rather than
    // let a confusing 404 surface from inside the editor's own upload UI.
    function currentDocPath() {
      return data.isNew ? null : pathInput.value.trim();
    }

    // Shared by both attachment paths below (drag/paste-an-image and the
    // explicit "Attach file" button). Resolves with {url, filename} — url
    // is the ABSOLUTE /assets/... path (basePath-prefixed), not the
    // relative-to-the-.assets-folder form a hand-edited markdown file
    // would use outside the web UI: this app's own /d/{path} view isn't a
    // directory-shaped URL, so a bare relative link wouldn't resolve
    // through the browser correctly the way it would in an external
    // markdown previewer/editor pointed straight at the vault on disk.
    function uploadAttachment(file) {
      const docPath = currentDocPath();
      if (!docPath) {
        return Promise.reject(new Error("Save the document first — attachments need an existing document to attach to."));
      }
      const form = new FormData();
      form.append("file", file, file.name);
      return fetch(basePath + "/api/attachments/" + encodeVaultPath(docPath), {
        method: "POST",
        headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
        credentials: "same-origin",
        body: form,
      }).then(function (resp) {
        if (!resp.ok) {
          return resp.text().then(function (text) {
            throw new Error(text || ("HTTP " + resp.status));
          });
        }
        return resp.json().then(function (info) {
          return { url: basePath + "/assets/" + encodeVaultPath(info.path), filename: file.name };
        });
      });
    }

    const editor = new toastui.Editor({
      el: document.getElementById("editor"),
      height: "500px",
      initialEditType: "wysiwyg",
      previewStyle: "tab",
      initialValue: data.body || "",
      // Fires on paste/drag-drop of an image straight into the editor.
      // Toast UI's default behavior with no hook is to inline the image
      // as a base64 data URI in the markdown — fine for a quick note, bad
      // for a wiki (bloats the stored document, no de-dup, no visibility
      // gating on the image itself). Route it through the same attachment
      // pipeline as everything else instead.
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
    });

    document.getElementById("doc-form").addEventListener("submit", function (evt) {
      evt.preventDefault();
      setStatus("Saving...", "");

      const path = pathInput.value.trim();
      if (!path) {
        setStatus("Path is required.", "error");
        return;
      }

      const payload = {
        title: titleInput.value.trim(),
        tags: tagsInput.value
          .split(",")
          .map(function (t) { return t.trim(); })
          .filter(function (t) { return t.length > 0; }),
        type: typeInput.value.trim(),
        visibility: visibilityInput.checked ? "public" : "private",
        body: editor.getMarkdown(),
      };

      const isNew = data.isNew;
      const url = isNew
        ? basePath + "/api/documents"
        : basePath + "/api/documents/" + encodeVaultPath(path);
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
          if (!resp.ok) {
            return resp.text().then(function (text) {
              throw new Error(text || ("HTTP " + resp.status));
            });
          }
          setStatus("Saved.", "ok");
          window.location.href = basePath + "/d/" + encodeVaultPath(path);
        })
        .catch(function (err) {
          setStatus("Save failed: " + err.message, "error");
        });
    });

    // Explicit "Attach file" button — covers anything addImageBlobHook
    // doesn't (PDFs, zips, audio/video, ...; see the extension allowlist
    // in AttachmentService.cpp). Inserts a markdown link at the cursor
    // rather than trying to render inline, since most of those types
    // aren't images.
    const attachInput = document.getElementById("f-attach");
    const attachBtn = document.getElementById("f-attach-btn");
    if (attachBtn && attachInput) {
      attachBtn.addEventListener("click", function () {
        attachInput.click();
      });
      attachInput.addEventListener("change", function () {
        const file = attachInput.files[0];
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
    }
  });
})();
