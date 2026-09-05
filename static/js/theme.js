// Theme picker (sidebar) — the reader's actual choice-of-theme UI. The
// early, synchronous <script> at the very top of shell.html's <head> is
// what APPLIES a saved choice on every single load (that's what avoids a
// flash of the wrong theme); this file only has to handle picking a NEW
// one and remembering it for next time.
//
// Full page reload on change rather than live-swapping the stylesheet or
// trying to reconcile matrix.js's already-running canvas — this app
// already reloads on every navigation (see router.js's own comment on
// why: no History API, deliberately), so a theme change doing the same
// is consistent with that, not a new pattern.
(function () {
  "use strict";

  var THEME_KEY = "wiki.theme";
  var THEMES = ["classic", "dark", "green"];

  document.addEventListener("DOMContentLoaded", function () {
    var toggleBtn = document.getElementById("theme-toggle-btn");
    var menu = document.getElementById("theme-menu");
    if (!toggleBtn || !menu) return;

    // Marks whichever entry matches the theme <head>'s script already
    // applied (via <html data-theme>) — reading that attribute back here
    // rather than re-reading localStorage directly, so this stays correct
    // even in the one case they could disagree: localStorage blocked/
    // cleared between the head script running and this one, where
    // data-theme (already committed to the DOM) is the one that's real.
    var current = document.documentElement.getAttribute("data-theme") || "green";
    Array.prototype.forEach.call(menu.querySelectorAll("[data-theme]"), function (btn) {
      if (btn.getAttribute("data-theme") === current) {
        btn.classList.add("theme-menu-current");
      }
    });

    toggleBtn.addEventListener("click", function (e) {
      e.stopPropagation();
      menu.hidden = !menu.hidden;
      toggleBtn.setAttribute("aria-expanded", menu.hidden ? "false" : "true");
    });
    // Same "click outside closes it" convention as the tag/type
    // multiselect dropdowns (search.js's createMultiSelect) — a dropdown
    // left open until a SECOND explicit click on the toggle is a worse
    // interaction than closing on any other click landing outside it.
    document.addEventListener("click", function (e) {
      if (!menu.hidden && !menu.contains(e.target) && e.target !== toggleBtn) {
        menu.hidden = true;
        toggleBtn.setAttribute("aria-expanded", "false");
      }
    });

    Array.prototype.forEach.call(menu.querySelectorAll("[data-theme]"), function (btn) {
      var theme = btn.getAttribute("data-theme");
      if (THEMES.indexOf(theme) === -1) return; // unknown value in markup — ignore rather than persist garbage
      btn.addEventListener("click", function () {
        try {
          localStorage.setItem(THEME_KEY, theme);
        } catch (e) {
          // Blocked/unavailable localStorage — the reload below still
          // re-applies the CURRENT (unchanged) theme; the choice just
          // won't stick past this one page view.
        }
        location.reload();
      });
    });
  });
})();
