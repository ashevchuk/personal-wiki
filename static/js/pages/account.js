// Account page — admin-only "change password" form. Renders into
// #app-content, posts to POST /api/account/password (JSON, CSRF-protected
// same as every other mutating route — see common.js's getCookie() use
// below). No HTML from C++ at any point, per the project's standing
// mandate: this whole page exists client-side, the backend only ever
// answers with {ok:true}/{error:"..."} (see AuthRoutes.cpp).
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var errorFromResponse = WikiCommon.errorFromResponse;

  window.WikiPages.renderAccount = function (container, session) {
    document.getElementById("page-title").textContent = "Account — wiki";

    // Same gate every admin-only page module uses (see edit.js) — a
    // direct hit on /account while logged out bounces to /login rather
    // than rendering a form that would just 401 on submit.
    if (!session.authenticated) {
      window.location.href = basePath() + "/login";
      return;
    }

    container.innerHTML =
      '<h1>Change password</h1>' +
      '<p id="account-error" style="color:#ff5555"></p>' +
      '<p id="account-success" style="color:#50fa7b"></p>' +
      '<form id="account-form">' +
      '<p><label>Current password ' +
      '<input type="password" name="currentPassword" autocomplete="current-password" required></label></p>' +
      '<p><label>New password ' +
      '<input type="password" name="newPassword" autocomplete="new-password" required></label></p>' +
      '<p><label>Confirm new password ' +
      '<input type="password" name="confirmPassword" autocomplete="new-password" required></label></p>' +
      '<p><button type="submit">Change password</button></p>' +
      "</form>";

    var errorEl = document.getElementById("account-error");
    var successEl = document.getElementById("account-success");

    document.getElementById("account-form").addEventListener("submit", function (evt) {
      evt.preventDefault();
      errorEl.textContent = "";
      successEl.textContent = "";

      var form = evt.target;
      var currentPassword = form.currentPassword.value;
      var newPassword = form.newPassword.value;
      var confirmPassword = form.confirmPassword.value;

      if (newPassword !== confirmPassword) {
        errorEl.textContent = "New password and confirmation don't match.";
        return;
      }

      fetch(basePath() + "/api/account/password", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-CSRF-Token": WikiCommon.getCookie("wiki_csrf_token"),
        },
        credentials: "same-origin",
        body: JSON.stringify({ currentPassword: currentPassword, newPassword: newPassword }),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (err) { throw err; });
          form.reset();
          successEl.textContent =
            "Password changed. Any other active sessions were signed out.";
        })
        .catch(function (err) {
          errorEl.textContent = err.message;
        });
    });
  };
})();
