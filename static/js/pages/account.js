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
  var getCookie = WikiCommon.getCookie;

  // Remote MCP settings — see AdminRoutes.h for the exact route shapes
  // and docs/mcp.md for what this feature actually does. Every control
  // here applies immediately (PUT/POST on change, no separate "Save"
  // button) — that immediacy (no server restart) is the entire point of
  // storing this in SQLite instead of config.toml.
  function renderRemoteMcpSection() {
    return (
      "<h2>Remote MCP</h2>" +
      "<p>Lets an MCP client (Claude Desktop/Code, or anything else that speaks " +
      "MCP) reach this wiki over HTTPS instead of only a local stdio spawn — " +
      "off by default. Requires TLS in front of this server (a reverse proxy, " +
      "same as any other public exposure of this app) — the bearer token below " +
      "travels in a plain HTTP header.</p>" +
      '<p id="mcp-remote-error" style="color:#ff5555"></p>' +
      '<p id="mcp-remote-success" style="color:#50fa7b"></p>' +
      '<p><label><input type="checkbox" id="mcp-remote-enabled"> Enabled</label></p>' +
      '<p><label><input type="checkbox" id="mcp-remote-write"> ' +
      "Allow create/update through remote MCP (independent of the local " +
      "stdio server's own write-access setting)</label></p>" +
      "<p>Bearer token: " +
      '<span id="mcp-remote-token-status">&hellip;</span> ' +
      '<button type="button" id="mcp-remote-regen-btn">Regenerate token</button></p>' +
      '<div id="mcp-remote-token-reveal" hidden>' +
      "<p>New token — shown once, right here. Copy it now; nothing in this " +
      "app displays it again.</p>" +
      '<input type="text" id="mcp-remote-token-value" readonly ' +
      'style="width:100%" onclick="this.select()"></div>' +
      "<h3>Allowed IPs / CIDRs</h3>" +
      "<p>Empty means no IP restriction — the bearer token is the actual gate; " +
      "this is an optional extra layer.</p>" +
      '<ul id="mcp-remote-cidr-list"></ul>' +
      '<p><input type="text" id="mcp-remote-cidr-input" placeholder="e.g. 203.0.113.0/24 or 2001:db8::1">' +
      ' <button type="button" id="mcp-remote-cidr-add-btn">Add</button></p>' +
      "<h3>Recent activity</h3>" +
      '<ul id="mcp-remote-audit-list"><li class="empty">Loading&hellip;</li></ul>'
    );
  }

  function wireRemoteMcpSection() {
    var errorEl = document.getElementById("mcp-remote-error");
    var successEl = document.getElementById("mcp-remote-success");
    var enabledCb = document.getElementById("mcp-remote-enabled");
    var writeCb = document.getElementById("mcp-remote-write");
    var tokenStatus = document.getElementById("mcp-remote-token-status");
    var cidrList = document.getElementById("mcp-remote-cidr-list");

    function showError(err) {
      errorEl.textContent = err.message || String(err);
      successEl.textContent = "";
    }
    function showSuccess(msg) {
      successEl.textContent = msg;
      errorEl.textContent = "";
    }

    function renderSettings(settings) {
      enabledCb.checked = settings.enabled;
      writeCb.checked = settings.writeEnabled;
      tokenStatus.textContent = settings.hasToken
        ? "a token is set (hidden — regenerate to see a new one)"
        : "no token generated yet";

      cidrList.innerHTML = "";
      if (settings.allowedCidrs.length === 0) {
        cidrList.innerHTML = '<li class="empty">None — no IP restriction.</li>';
      } else {
        settings.allowedCidrs.forEach(function (cidr) {
          var li = document.createElement("li");
          li.appendChild(document.createTextNode(cidr + " "));
          var removeBtn = document.createElement("button");
          removeBtn.type = "button";
          removeBtn.textContent = "Remove";
          removeBtn.addEventListener("click", function () {
            fetch(
              basePath() +
                "/api/admin/mcp-remote-config/allowed-cidrs?cidr=" +
                encodeURIComponent(cidr),
              {
                method: "DELETE",
                headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
                credentials: "same-origin",
              }
            )
              .then(function (resp) {
                if (!resp.ok) return errorFromResponse(resp).then(function (e) { throw e; });
                return resp.json();
              })
              .then(function (settings) {
                renderSettings(settings);
                showSuccess("Removed " + cidr + ".");
              })
              .catch(showError);
          });
          li.appendChild(removeBtn);
          cidrList.appendChild(li);
        });
      }
    }

    function loadSettings() {
      fetch(basePath() + "/api/admin/mcp-remote-config", { credentials: "same-origin" })
        .then(function (r) {
          return r.json();
        })
        .then(renderSettings)
        .catch(showError);
    }

    function putFlag(field, value) {
      var body = {};
      body[field] = value;
      fetch(basePath() + "/api/admin/mcp-remote-config", {
        method: "PUT",
        headers: {
          "Content-Type": "application/json",
          "X-CSRF-Token": getCookie("wiki_csrf_token"),
        },
        credentials: "same-origin",
        body: JSON.stringify(body),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (e) { throw e; });
          return resp.json();
        })
        .then(function (settings) {
          renderSettings(settings);
          showSuccess("Saved.");
        })
        .catch(showError);
    }

    enabledCb.addEventListener("change", function () {
      putFlag("enabled", enabledCb.checked);
    });
    writeCb.addEventListener("change", function () {
      putFlag("writeEnabled", writeCb.checked);
    });

    document.getElementById("mcp-remote-regen-btn").addEventListener("click", function () {
      if (
        !window.confirm(
          "Regenerate the remote MCP bearer token? The CURRENT token stops working immediately."
        )
      ) {
        return;
      }
      fetch(basePath() + "/api/admin/mcp-remote-config/regenerate-token", {
        method: "POST",
        headers: { "X-CSRF-Token": getCookie("wiki_csrf_token") },
        credentials: "same-origin",
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (e) { throw e; });
          return resp.json();
        })
        .then(function (data) {
          document.getElementById("mcp-remote-token-reveal").hidden = false;
          document.getElementById("mcp-remote-token-value").value = data.token;
          tokenStatus.textContent = "a token is set (hidden — regenerate to see a new one)";
          showSuccess("New token generated — copy it now, it won't be shown again.");
        })
        .catch(showError);
    });

    document.getElementById("mcp-remote-cidr-add-btn").addEventListener("click", function () {
      var input = document.getElementById("mcp-remote-cidr-input");
      var cidr = input.value.trim();
      if (!cidr) return;
      fetch(basePath() + "/api/admin/mcp-remote-config/allowed-cidrs", {
        method: "POST",
        headers: {
          "Content-Type": "application/json",
          "X-CSRF-Token": getCookie("wiki_csrf_token"),
        },
        credentials: "same-origin",
        body: JSON.stringify({ cidr: cidr }),
      })
        .then(function (resp) {
          if (!resp.ok) return errorFromResponse(resp).then(function (e) { throw e; });
          return resp.json();
        })
        .then(function (settings) {
          input.value = "";
          renderSettings(settings);
          showSuccess("Added " + cidr + ".");
        })
        .catch(showError);
    });

    function loadAuditLog() {
      var list = document.getElementById("mcp-remote-audit-list");
      fetch(basePath() + "/api/admin/mcp-audit-log", { credentials: "same-origin" })
        .then(function (r) {
          return r.json();
        })
        .then(function (data) {
          var entries = data.entries || [];
          if (entries.length === 0) {
            list.innerHTML = '<li class="empty">Nothing recorded yet.</li>';
            return;
          }
          list.innerHTML = "";
          entries.slice(0, 20).forEach(function (e) {
            var li = document.createElement("li");
            var status = e.success ? "ok" : "FAILED";
            li.textContent =
              e.at + " — " + e.toolName + " " + e.path + " (" + status + ": " + e.detail + ")";
            if (!e.success) li.style.color = "#ff5555";
            list.appendChild(li);
          });
        })
        .catch(function () {
          list.innerHTML = '<li class="empty">Failed to load.</li>';
        });
    }

    loadSettings();
    loadAuditLog();
  }

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
      "</form>" +
      renderRemoteMcpSection();

    wireRemoteMcpSection();

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
