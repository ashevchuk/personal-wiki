// Drag-to-resize the sidebar via #sidebar-resizer (see shell.html) —
// width lives in the --sidebar-w custom property on :root, which
// .sidebar's own width/flex-basis (each css/themes/*.css file) read from. Persisted to
// localStorage purely as a per-browser convenience (remembers the last
// width on this device); never sent anywhere, never trusted for
// anything server-side.
(function () {
  "use strict";

  var STORAGE_KEY = "wiki.sidebarWidth";
  var MIN_WIDTH = 160;
  var MAX_WIDTH = 600;
  var DEFAULT_WIDTH = 260; // matches each theme file's un-resized .sidebar width

  function clamp(w) {
    return Math.max(MIN_WIDTH, Math.min(MAX_WIDTH, w));
  }

  function applyWidth(w) {
    document.documentElement.style.setProperty("--sidebar-w", w + "px");
  }

  // Read the saved width before anything paints — avoids a flash of the
  // default width snapping to the saved one a frame later. localStorage
  // can legitimately throw (private browsing, blocked site data) or hold
  // garbage from a much older version of this app; either way, falling
  // back to the CSS default is correct, not an error worth surfacing.
  (function restoreSavedWidth() {
    try {
      var saved = parseInt(localStorage.getItem(STORAGE_KEY), 10);
      if (!isNaN(saved)) applyWidth(clamp(saved));
    } catch (e) {
      // ignore — default width from the active theme file applies as-is
    }
  })();

  document.addEventListener("DOMContentLoaded", function () {
    var resizer = document.getElementById("sidebar-resizer");
    var sidebar = document.querySelector(".sidebar");
    if (!resizer || !sidebar) return;

    var dragging = false;

    function onMouseMove(evt) {
      if (!dragging) return;
      // clientX is already relative to the viewport's left edge, which
      // is exactly where .sidebar starts (it's the layout's first flex
      // child) — no offset math needed.
      applyWidth(clamp(evt.clientX));
    }

    function onMouseUp() {
      if (!dragging) return;
      dragging = false;
      document.body.classList.remove("sidebar-resizing");
      try {
        var current = parseInt(getComputedStyle(document.documentElement)
          .getPropertyValue("--sidebar-w"), 10);
        if (!isNaN(current)) localStorage.setItem(STORAGE_KEY, String(current));
      } catch (e) {
        // localStorage unavailable — the width still applies for the
        // rest of this page load, it just won't survive a reload
      }
    }

    resizer.addEventListener("mousedown", function (evt) {
      dragging = true;
      // Suppresses text selection/cursor flicker across the rest of the
      // page while dragging — see the .sidebar-resizing rule in each
      // css/themes/*.css file.
      document.body.classList.add("sidebar-resizing");
      evt.preventDefault();
    });
    document.addEventListener("mousemove", onMouseMove);
    document.addEventListener("mouseup", onMouseUp);

    // Double-click resets to the default width — the only way back once
    // a saved width has drifted somewhere awkward, short of clearing
    // localStorage by hand.
    resizer.addEventListener("dblclick", function () {
      applyWidth(DEFAULT_WIDTH);
      try {
        localStorage.setItem(STORAGE_KEY, String(DEFAULT_WIDTH));
      } catch (e) {
        // ignore, same as above
      }
    });
  });
})();
