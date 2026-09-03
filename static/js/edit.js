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

    const editor = new toastui.Editor({
      el: document.getElementById("editor"),
      height: "500px",
      initialEditType: "wysiwyg",
      previewStyle: "tab",
      initialValue: data.body || "",
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
  });
})();
