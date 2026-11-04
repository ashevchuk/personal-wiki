// Theme picker (sidebar) — the reader's actual choice-of-theme UI. The
// early, synchronous <script> at the very top of shell.html's <head> is
// what APPLIES a saved choice on every single load (that's what avoids a
// flash of the wrong theme); this file only has to handle picking a NEW
// one and remembering it for next time.
//
// Full page reload on change rather than live-swapping the stylesheet —
// this app already reloads on every navigation (see router.js's own
// comment on why: no History API, deliberately), so a theme change
// doing the same is consistent with that, not a new pattern.
(function () {
  "use strict";

  var THEME_KEY = "wiki.theme";
  // Same window.__WIKI_THEMES__ shell.html's own bootstrap script reads —
  // injected by PageRoutes.cpp from static/css/themes/*.css, so a new
  // theme file needs no change here.
  var THEMES = window.__WIKI_THEMES__ || [];

  document.addEventListener("DOMContentLoaded", function () {
    var toggleBtn = document.getElementById("theme-toggle-btn");
    var menu = document.getElementById("theme-menu");
    if (!toggleBtn || !menu) return;

    // A "portal": moved out of .theme-picker (the sidebar) into
    // document.body, same technique and same reason as edit.js's
    // typeahead dropdown (see css/themes/*.css's .theme-menu comment) --
    // a non-positioned ancestor can't win a page-level stacking fight
    // against ANY positioned element elsewhere with an explicit
    // z-index, no matter what z-index this menu sets on itself.
    // position:fixed + coordinates computed here, not CSS, since the
    // toggle button's screen position varies by viewport/scroll.
    document.body.appendChild(menu);
    function positionMenu() {
      var rect = toggleBtn.getBoundingClientRect();
      menu.style.top = rect.bottom + 4 + "px";
      menu.style.left = rect.left + "px";
    }

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
      if (menu.hidden) positionMenu();
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
