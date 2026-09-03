// Login page — renders into #app-content, posts to /api/login (JSON),
// and does the "already logged in" redirect that used to be a
// server-side check in the old HTML-returning /login GET handler.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var errorFromResponse = WikiCommon.errorFromResponse;

  window.WikiPages.renderLogin = function (container, session) {
    document.getElementById("page-title").textContent = "Sign in — wiki";

    if (session.authenticated) {
      window.location.href = basePath() + "/search";
      return;
    }

    container.innerHTML =
      '<h1>Sign in</h1>' +
      '<p id="login-error" style="color:#ff5555"></p>' +
      '<form id="login-form">' +
      '<p><label>Username <input type="text" name="username" autocomplete="username" required></label></p>' +
      '<p><label>Password <input type="password" name="password" autocomplete="current-password" required></label></p>' +
      '<p><button type="submit">Sign in</button></p>' +
      "</form>";

    var errorEl = document.getElementById("login-error");
    document.getElementById("login-form").addEventListener("submit", function (evt) {
      evt.preventDefault();
      errorEl.textContent = "";
      var form = evt.target;
      var username = form.username.value;
      var password = form.password.value;

      fetch(basePath() + "/api/login", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        credentials: "same-origin",
        body: JSON.stringify({ username: username, password: password }),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          window.location.href = basePath() + "/search";
        })
        .catch(function (err) {
          errorEl.textContent = err.message;
        });
    });
  };
})();
