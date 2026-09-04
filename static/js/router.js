// Dispatches the current URL to the right page renderer. Loaded LAST (see
// shell.html) — every static/js/pages/*.js module must already be loaded
// and have registered itself on window.WikiPages before this runs.
//
// Full page reloads on every navigation (no History API / pushState) —
// deliberately simple: every link in this app is a normal <a href> (or a
// window.location.href assignment), the server always returns the exact
// same shell.html for any of the routes below (see PageRoutes.cpp), and
// this file just re-runs from scratch on each load, the same way
// nav.js/matrix.js already did before this rewrite. No client-side
// router library, no route table beyond the plain if/else below.
(function () {
  "use strict";

  var basePath = WikiCommon.basePath;

  function fixSidebarLinks() {
    var bp = basePath();
    document.querySelectorAll("[data-href]").forEach(function (el) {
      el.setAttribute("href", bp + el.getAttribute("data-href"));
    });
  }

  // Sidebar admin-only chrome (+New, Log out) — fetched once per page
  // load here, then the resolved `session` is handed to whichever page
  // renderer runs next so it doesn't need a second, redundant
  // GET /api/session of its own for the SAME chrome decision.
  function wireSidebarAuthChrome(session) {
    var newDocBtn = document.getElementById("sidebar-new-doc");
    var accountLink = document.getElementById("sidebar-account");
    var logoutBtn = document.getElementById("sidebar-logout");
    if (!session.authenticated) return;

    if (accountLink) accountLink.hidden = false;

    if (newDocBtn) {
      newDocBtn.hidden = false;
      newDocBtn.addEventListener("click", function () {
        if (window.WikiPages && window.WikiPages.newDocument) {
          window.WikiPages.newDocument("");
        }
      });
    }
    if (logoutBtn) {
      logoutBtn.hidden = false;
      logoutBtn.addEventListener("click", function () {
        fetch(basePath() + "/api/logout", {
          method: "POST",
          headers: { "X-CSRF-Token": WikiCommon.getCookie("wiki_csrf_token") },
          credentials: "same-origin",
        }).then(function () {
          window.location.href = basePath() + "/login";
        });
      });
    }
  }

  function localPath() {
    var bp = basePath();
    var p = location.pathname;
    if (bp && p.indexOf(bp) === 0) p = p.slice(bp.length);
    return p || "/";
  }

  document.addEventListener("DOMContentLoaded", function () {
    fixSidebarLinks();

    var content = document.getElementById("app-content");
    var path = localPath();
    var pages = window.WikiPages || {};

    WikiCommon.fetchSession().then(function (session) {
      wireSidebarAuthChrome(session);

      if (path === "/") {
        window.location.href = basePath() + "/search";
        return;
      }
      if (path === "/login" || path === "/login/") {
        pages.renderLogin(content, session);
        return;
      }
      if (path === "/search" || path === "/search/") {
        pages.renderSearch(content, session);
        return;
      }
      if (path === "/folder" || path === "/folder/") {
        pages.renderFolder(content, "", session);
        return;
      }
      if (path === "/account" || path === "/account/") {
        pages.renderAccount(content, session);
        return;
      }
      // location.pathname is already percent-decoded by the browser, so
      // these slices are plain vault-relative paths already — no further
      // decoding needed at any of these three call sites.
      if (path.indexOf("/folder/") === 0) {
        pages.renderFolder(content, path.slice("/folder/".length), session);
        return;
      }
      if (path.indexOf("/d/") === 0) {
        pages.renderView(content, path.slice("/d/".length), session);
        return;
      }
      if (path.indexOf("/edit/") === 0) {
        pages.renderEdit(content, path.slice("/edit/".length), session);
        return;
      }
      if (path.indexOf("/history/") === 0) {
        pages.renderHistory(content, path.slice("/history/".length), session);
        return;
      }

      content.textContent = "Not found.";
    });
  });
})();
