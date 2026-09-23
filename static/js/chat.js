// Floating vault Chat panel, opened from the sidebar. Lives on every
// shell page (not only /edit/). Talks to the same /api/agent/sessions
// as Draft, with kind:"chat" — read-only vault tools, a separate
// system prompt. MCP is not in this path.
//
// This app full-page-reloads on every navigation (router.js), so JS
// state dies. Chat history lives in SQLite (agent_chats); we keep the
// current session id (and open/geometry) in sessionStorage and restore
// via GET + SSE ?after=. Do not DELETE on pagehide — that would wipe
// the conversation the moment the reader clicks a wiki-link. New chat
// only clears the local id; rename/delete are explicit sidebar actions.
window.WikiChat = (function () {
  "use strict";

  var MIN_W = 480;
  var MIN_H = 220;
  var MIN_SIDE = 140;
  var MAX_SIDE = 420;
  var DEFAULT_SIDE = 188;
  var MIN_MAIN = 260;
  var STORE_ID = "wiki.chat.sessionId";
  var STORE_OPEN = "wiki.chat.open";
  var STORE_GEOM = "wiki.chat.geom";
  var STORE_SIDE = "wiki.chat.sidebarWidth";

  var basePath = function () {
    return window.WikiCommon.basePath();
  };
  var getCookie = function (name) {
    return window.WikiCommon.getCookie(name);
  };
  var encodeVaultPath = function (path) {
    return window.WikiCommon.encodeVaultPath(path);
  };

  var panel = null;
  var logEl = null;
  var inputEl = null;
  var sendBtn = null;
  var stopBtn = null;
  var statusEl = null;
  var listEl = null;
  var sidebarEl = null;
  var sessionId = null;
  var pollTimer = null;
  var streamAbort = null;
  var eventEls = [];
  var seenEvents = 0;
  var booted = false;
  var savedSidebarWidth = DEFAULT_SIDE;

  function csrfHeaders() {
    return {
      "Content-Type": "application/json",
      "X-CSRF-Token": getCookie("wiki_csrf_token"),
    };
  }

  function clamp(n, lo, hi) {
    return Math.max(lo, Math.min(hi, n));
  }

  function storageGet(key) {
    try {
      return sessionStorage.getItem(key);
    } catch (e) {
      return null;
    }
  }

  function storageSet(key, value) {
    try {
      if (value == null) sessionStorage.removeItem(key);
      else sessionStorage.setItem(key, value);
    } catch (e) {}
  }

  function localGet(key) {
    try {
      return localStorage.getItem(key);
    } catch (e) {
      return null;
    }
  }

  function localSet(key, value) {
    try {
      if (value == null) localStorage.removeItem(key);
      else localStorage.setItem(key, value);
    } catch (e) {}
  }

  (function restoreSavedSidebarWidth() {
    var n = parseInt(localGet(STORE_SIDE), 10);
    if (!isNaN(n)) savedSidebarWidth = clamp(n, MIN_SIDE, MAX_SIDE);
  })();

  function persistSession() {
    storageSet(STORE_ID, sessionId || null);
  }

  function persistOpen(open) {
    storageSet(STORE_OPEN, open ? "1" : "0");
  }

  function persistGeom() {
    if (!panel || panel.hidden) return;
    var r = panel.getBoundingClientRect();
    storageSet(
      STORE_GEOM,
      JSON.stringify({
        left: r.left,
        top: r.top,
        width: r.width,
        height: r.height,
        sidebarWidth: currentSidebarWidth(),
      })
    );
  }

  // display:none (the hidden attribute) makes getBoundingClientRect()
  // return 0, which used to clamp a restored width down to MIN_SIDE.
  function panelWidthPx() {
    if (!panel) return MIN_W;
    if (!panel.hidden) {
      var w = panel.getBoundingClientRect().width;
      if (w > 0) return w;
    }
    var styleW = parseFloat(panel.style.width);
    if (!isNaN(styleW) && styleW > 0) return styleW;
    return MIN_W;
  }

  function sidebarWidthMax() {
    return Math.max(MIN_SIDE, Math.min(MAX_SIDE, panelWidthPx() - MIN_MAIN));
  }

  function currentSidebarWidth() {
    if (sidebarEl) {
      var w = sidebarEl.getBoundingClientRect().width;
      if (w > 0) return w;
      var styleW = parseFloat(sidebarEl.style.width);
      if (!isNaN(styleW) && styleW > 0) return styleW;
    }
    return savedSidebarWidth;
  }

  function applySidebarWidth(w) {
    if (!sidebarEl) return;
    var px = clamp(Math.round(w), MIN_SIDE, sidebarWidthMax());
    sidebarEl.style.width = px + "px";
    sidebarEl.style.flexBasis = px + "px";
    sidebarEl.style.flexGrow = "0";
    sidebarEl.style.flexShrink = "0";
    savedSidebarWidth = px;
    localSet(STORE_SIDE, String(px));
  }

  function applyGeom() {
    if (!panel) return;
    var raw = storageGet(STORE_GEOM);
    if (!raw) return;
    var geom;
    try {
      geom = JSON.parse(raw);
    } catch (e) {
      return;
    }
    if (!geom || typeof geom.left !== "number") return;
    var maxW = Math.max(16, window.innerWidth - 16);
    var maxH = Math.max(16, window.innerHeight - 16);
    var w = typeof geom.width === "number" ? geom.width : MIN_W;
    var h = typeof geom.height === "number" ? geom.height : MIN_H;
    panel.style.left = geom.left + "px";
    panel.style.top = geom.top + "px";
    panel.style.width = clamp(w, Math.min(MIN_W, maxW), maxW) + "px";
    panel.style.height = clamp(h, Math.min(MIN_H, maxH), maxH) + "px";
    panel.style.right = "auto";
    panel.style.bottom = "auto";
    var side =
      typeof geom.sidebarWidth === "number" ? geom.sidebarWidth : savedSidebarWidth;
    applySidebarWidth(side);
  }

  function pinToPixels() {
    if (!panel) return;
    var r = panel.getBoundingClientRect();
    panel.style.left = r.left + "px";
    panel.style.top = r.top + "px";
    panel.style.width = r.width + "px";
    panel.style.height = r.height + "px";
    panel.style.right = "auto";
    panel.style.bottom = "auto";
  }

  function keepOnScreen() {
    if (!panel) return;
    var r = panel.getBoundingClientRect();
    var w = clamp(r.width, Math.min(MIN_W, window.innerWidth - 16), Math.max(16, window.innerWidth - 16));
    var h = clamp(r.height, Math.min(MIN_H, window.innerHeight - 16), Math.max(16, window.innerHeight - 16));
    var left = clamp(r.left, 8, Math.max(8, window.innerWidth - w - 8));
    var top = clamp(r.top, 8, Math.max(8, window.innerHeight - h - 8));
    panel.style.left = left + "px";
    panel.style.top = top + "px";
    panel.style.right = "auto";
    panel.style.bottom = "auto";
    panel.style.width = w + "px";
    panel.style.height = h + "px";
    applySidebarWidth(currentSidebarWidth());
    persistGeom();
  }

  function bindPointerDrag(el, onMove, shouldIgnore, dragClass) {
    el.addEventListener("pointerdown", function (ev) {
      if (ev.button !== 0) return;
      if (shouldIgnore && shouldIgnore(ev.target)) return;
      ev.preventDefault();
      pinToPixels();
      el.setPointerCapture(ev.pointerId);
      document.body.classList.add(dragClass || "agent-panel-dragging");
      var lastX = ev.clientX;
      var lastY = ev.clientY;
      function move(e) {
        onMove(e.clientX - lastX, e.clientY - lastY);
        lastX = e.clientX;
        lastY = e.clientY;
      }
      function up() {
        el.removeEventListener("pointermove", move);
        el.removeEventListener("pointerup", up);
        el.removeEventListener("pointercancel", up);
        document.body.classList.remove(dragClass || "agent-panel-dragging");
        keepOnScreen();
      }
      el.addEventListener("pointermove", move);
      el.addEventListener("pointerup", up);
      el.addEventListener("pointercancel", up);
    });
  }

  function ensurePanel() {
    if (panel) return;
    panel = document.createElement("div");
    panel.className = "agent-panel";
    panel.id = "wiki-chat-panel";
    panel.hidden = true;
    panel.innerHTML =
      '<aside class="chat-sidebar">' +
      '<button type="button" class="chat-new-btn" id="chat-new">New chat</button>' +
      '<div class="chat-session-list" id="chat-session-list"></div>' +
      "</aside>" +
      '<div class="chat-sidebar-resize" aria-hidden="true" title="Drag to resize"></div>' +
      '<div class="chat-main">' +
      '<div class="agent-panel-header">' +
      "<strong>Chat</strong>" +
      '<span class="agent-panel-header-actions">' +
      '<button type="button" class="agent-panel-help-btn" aria-label="How to use Chat" title="How to use Chat" aria-expanded="false">' +
      '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      '<path d="M9.1 9a3 3 0 0 1 5.83 1c0 2-3 3-3 4"/>' +
      '<circle cx="12" cy="17" r="0.85" fill="currentColor" stroke="none"/>' +
      "</svg></button>" +
      '<button type="button" class="agent-panel-close" aria-label="Close" title="Close">' +
      '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      '<path d="M18 6 6 18M6 6l12 12"/>' +
      "</svg></button>" +
      "</span></div>" +
      '<div class="agent-help" id="chat-help" hidden>' +
      "<p>Searches the vault and answers here. Sees which document or folder you have open. Cannot create or edit notes — open the editor (and Draft) for that. <strong>Save</strong> is still the only write to disk.</p>" +
      "<ul>" +
      "<li>Ask about existing notes, including \"what's in this document\" on the page you have open. Send with the button or Ctrl/Cmd+Enter.</li>" +
      "<li>The left list is previous chats. New chat starts a blank thread without deleting the last one. Rename or delete from the row.</li>" +
      "<li>A ```query block on a page is a live table. The assistant can run it and can diff a document's history; it still cannot save.</li>" +
      "<li>Answers cite notes as [[path]] wiki-links; click one to open it. This panel stays open across wiki navigation.</li>" +
      "<li>If the question is unclear, the assistant asks here.</li>" +
      "<li>Stop cancels a run. Replies stream into this log as they arrive.</li>" +
      "<li>Close hides the panel and keeps the conversation. Draft on the edit page uses the same cloud client — only one of them can run at a time.</li>" +
      "</ul></div>" +
      '<div class="agent-panel-log" id="chat-log"></div>' +
      '<p class="agent-status" id="chat-status" hidden></p>' +
      "<textarea id=\"chat-input\" rows=\"4\" placeholder=\"Ask about the wiki…\"></textarea>" +
      '<div class="agent-panel-actions">' +
      '<button type="button" id="chat-stop" hidden>Stop</button>' +
      '<button type="button" id="chat-send">Send</button>' +
      "</div></div>" +
      '<div class="agent-panel-resize" aria-hidden="true"></div>';
    document.body.appendChild(panel);
    logEl = panel.querySelector("#chat-log");
    inputEl = panel.querySelector("#chat-input");
    sendBtn = panel.querySelector("#chat-send");
    stopBtn = panel.querySelector("#chat-stop");
    statusEl = panel.querySelector("#chat-status");
    listEl = panel.querySelector("#chat-session-list");
    sidebarEl = panel.querySelector(".chat-sidebar");
    panel.querySelector("#chat-new").addEventListener("click", function (ev) {
      ev.stopPropagation();
      newChat();
    });
    panel.querySelector(".agent-panel-close").addEventListener("click", function (ev) {
      ev.stopPropagation();
      hide();
    });
    var helpBtn = panel.querySelector(".agent-panel-help-btn");
    var helpEl = panel.querySelector("#chat-help");
    helpBtn.addEventListener("click", function (ev) {
      ev.stopPropagation();
      var open = helpEl.hidden;
      helpEl.hidden = !open;
      helpBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
    sendBtn.addEventListener("click", send);
    stopBtn.addEventListener("click", stop);
    inputEl.addEventListener("keydown", function (ev) {
      if (ev.key === "Enter" && (ev.metaKey || ev.ctrlKey)) {
        ev.preventDefault();
        send();
      }
    });
    bindPointerDrag(
      panel.querySelector(".agent-panel-header"),
      function (dx, dy) {
        var r = panel.getBoundingClientRect();
        panel.style.left = r.left + dx + "px";
        panel.style.top = r.top + dy + "px";
      },
      function (target) {
        return target.closest(".agent-panel-close, .agent-panel-help-btn");
      }
    );
    bindPointerDrag(panel.querySelector(".agent-panel-resize"), function (dx, dy) {
      var r = panel.getBoundingClientRect();
      var maxW = window.innerWidth - r.left - 8;
      var maxH = window.innerHeight - r.top - 8;
      panel.style.width = clamp(r.width + dx, MIN_W, maxW) + "px";
      panel.style.height = clamp(r.height + dy, MIN_H, maxH) + "px";
      applySidebarWidth(currentSidebarWidth());
    });
    var sideResize = panel.querySelector(".chat-sidebar-resize");
    bindPointerDrag(
      sideResize,
      function (dx) {
        applySidebarWidth(currentSidebarWidth() + dx);
      },
      null,
      "chat-sidebar-resizing"
    );
    sideResize.addEventListener("dblclick", function (ev) {
      ev.preventDefault();
      applySidebarWidth(DEFAULT_SIDE);
      persistGeom();
    });
    window.addEventListener("resize", function () {
      if (panel && !panel.hidden) keepOnScreen();
    });
  }

  // Literal [[path]] / [[path|Label]] as clickable wiki-links. Escape
  // the rest — never assign the model text as innerHTML.
  function fillWikiText(el, text) {
    el.textContent = "";
    var re = /\[\[([^\]|\n]+)(?:\|([^\]]+))?\]\]/g;
    var last = 0;
    var match;
    var src = String(text || "");
    while ((match = re.exec(src))) {
      if (match.index > last) {
        el.appendChild(document.createTextNode(src.slice(last, match.index)));
      }
      var path = match[1].trim();
      var label = (match[2] || path).trim();
      if (path) {
        var a = document.createElement("a");
        a.href = basePath() + "/d/" + encodeVaultPath(path);
        a.textContent = label;
        a.className = "wiki-link";
        el.appendChild(a);
      } else {
        el.appendChild(document.createTextNode(match[0]));
      }
      last = match.index + match[0].length;
    }
    if (last < src.length) {
      el.appendChild(document.createTextNode(src.slice(last)));
    }
  }

  function appendEvent(ev) {
    var p = document.createElement("p");
    var type = ev.type;
    var data = ev.data || {};
    if (type === "user") {
      p.className = "agent-user";
      p.textContent = "You: " + (data.text || "");
    } else if (type === "assistant") {
      fillAssistant(p, ev);
    } else if (type === "tool") {
      var extra = data.detail ? " " + data.detail : "";
      if (typeof data.count === "number") extra += " (" + data.count + ")";
      p.textContent = (data.name || "tool") + extra;
    } else if (type === "error") {
      p.className = "agent-error";
      p.textContent = data.text || "error";
    } else if (type === "cancelled") {
      p.textContent = "Stopped.";
    } else if (type === "done") {
      return null;
    } else {
      p.textContent = type;
    }
    logEl.appendChild(p);
    logEl.scrollTop = logEl.scrollHeight;
    return p;
  }

  function fillAssistant(p, ev) {
    var data = ev.data || {};
    p.className = "agent-assistant" + (data.streaming ? " agent-streaming" : "");
    fillWikiText(p, data.text || "");
    logEl.scrollTop = logEl.scrollHeight;
  }

  function applyEvent(ev, index) {
    if (!ev) return;
    if (ev.type === "done") return;
    if (typeof index === "number" && eventEls[index]) {
      if (ev.type === "assistant") fillAssistant(eventEls[index], ev);
      return;
    }
    var p = appendEvent(ev);
    if (p && typeof index === "number") eventEls[index] = p;
  }

  function setRunning(running) {
    sendBtn.disabled = running;
    stopBtn.hidden = !running;
    stopBtn.disabled = !running;
    statusEl.hidden = !running;
    statusEl.textContent = running ? "Working…" : "";
    if (running) startLive();
    else stopLive();
  }

  function resetLog() {
    seenEvents = 0;
    eventEls = [];
    if (logEl) logEl.innerHTML = "";
  }

  function renderView(view) {
    if (!view) return;
    sessionId = view.id;
    persistSession();
    var events = view.events || [];
    var start = seenEvents;
    if (start > 0 && events[start - 1] && events[start - 1].type === "assistant") {
      applyEvent(events[start - 1], start - 1);
    }
    for (var i = start; i < events.length; i++) {
      applyEvent(events[i], i);
    }
    seenEvents = events.length;
    setRunning(view.status === "running");
  }

  function startLive() {
    if (!sessionId) return;
    if (streamAbort || pollTimer) return;
    startStream();
  }

  function startStream() {
    if (!sessionId || streamAbort) return;
    streamAbort = new AbortController();
    var after = seenEvents;
    fetch(
      basePath() +
        "/api/agent/sessions/" +
        encodeURIComponent(sessionId) +
        "/stream?after=" +
        encodeURIComponent(String(after)),
      {
        credentials: "same-origin",
        headers: { Accept: "text/event-stream" },
        signal: streamAbort.signal,
      }
    )
      .then(function (r) {
        if (!r.ok || !r.body || !r.body.getReader) throw new Error("no stream");
        return readSse(r.body.getReader());
      })
      .then(function () {
        if (!streamAbort || !sessionId) return;
        streamAbort = null;
        return fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId), {
          credentials: "same-origin",
        }).then(function (r) {
          return r.json().then(function (body) {
            if (r.ok) renderView(body);
            else if (r.status === 404) forgetSession();
          });
        });
      })
      .catch(function (err) {
        if (!streamAbort) return;
        if (err && err.name === "AbortError") return;
        streamAbort = null;
        startPoll();
      });
  }

  function readSse(reader) {
    var decoder = new TextDecoder();
    var buf = "";
    var eventType = "";
    var dataLines = [];
    var id = "";
    function flush() {
      if (!dataLines.length && !eventType) {
        eventType = "";
        id = "";
        return;
      }
      var dataStr = dataLines.join("\n");
      var data = {};
      try {
        data = JSON.parse(dataStr);
      } catch (e) {
        data = { text: dataStr };
      }
      var ev = { type: eventType || "message", data: data };
      var index = id === "" ? undefined : parseInt(id, 10);
      if (typeof index === "number" && !isNaN(index)) {
        applyEvent(ev, index);
        seenEvents = Math.max(seenEvents, index + 1);
      } else {
        applyEvent(ev);
      }
      if (ev.type === "done" || ev.type === "error" || ev.type === "cancelled") {
        setRunning(false);
      }
      eventType = "";
      dataLines = [];
      id = "";
    }
    function consume(chunk) {
      buf += chunk;
      var idx;
      while ((idx = buf.indexOf("\n")) >= 0) {
        var line = buf.slice(0, idx);
        buf = buf.slice(idx + 1);
        if (line.charAt(line.length - 1) === "\r") line = line.slice(0, -1);
        if (line === "") {
          flush();
          continue;
        }
        if (line.charAt(0) === ":") continue;
        var colon = line.indexOf(":");
        var field = colon === -1 ? line : line.slice(0, colon);
        var value = colon === -1 ? "" : line.slice(colon + 1);
        if (value.charAt(0) === " ") value = value.slice(1);
        if (field === "event") eventType = value;
        else if (field === "data") dataLines.push(value);
        else if (field === "id") id = value;
      }
    }
    function pump() {
      return reader.read().then(function (res) {
        if (res.done) {
          if (buf) consume("\n");
          return;
        }
        consume(decoder.decode(res.value, { stream: true }));
        return pump();
      });
    }
    return pump();
  }

  function startPoll() {
    if (pollTimer || streamAbort) return;
    pollTimer = setInterval(function () {
      if (!sessionId) return;
      fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId), {
        credentials: "same-origin",
      })
        .then(function (r) {
          return r.json().then(function (body) {
            return { ok: r.ok, status: r.status, body: body };
          });
        })
        .then(function (res) {
          if (res.ok) renderView(res.body);
          else if (res.status === 404) forgetSession();
        })
        .catch(function () {});
    }, 400);
  }

  function stopPoll() {
    if (pollTimer) {
      clearInterval(pollTimer);
      pollTimer = null;
    }
  }

  function stopLive() {
    if (streamAbort) {
      streamAbort.abort();
      streamAbort = null;
    }
    stopPoll();
  }

  function forgetSession() {
    stopLive();
    sessionId = null;
    persistSession();
    resetLog();
    if (sendBtn) sendBtn.disabled = false;
    if (stopBtn) {
      stopBtn.hidden = true;
      stopBtn.disabled = false;
    }
    if (statusEl) {
      statusEl.hidden = true;
      statusEl.textContent = "";
    }
    refreshSessions();
  }

  function iconBtn(label, title, pathD) {
    var btn = document.createElement("button");
    btn.type = "button";
    btn.setAttribute("aria-label", label);
    btn.title = title;
    btn.innerHTML =
      '<svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" ' +
      'stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      pathD +
      "</svg>";
    return btn;
  }

  function renderSessionList(sessions) {
    if (!listEl) return;
    listEl.textContent = "";
    if (!sessions || !sessions.length) {
      var empty = document.createElement("p");
      empty.className = "chat-session-empty";
      empty.textContent = "No chats yet";
      listEl.appendChild(empty);
      return;
    }
    sessions.forEach(function (row) {
      var item = document.createElement("div");
      item.className =
        "chat-session-item" + (row.id === sessionId ? " chat-session--active" : "");
      var titleBtn = document.createElement("button");
      titleBtn.type = "button";
      titleBtn.className = "chat-session-title";
      titleBtn.textContent = row.title || "Chat";
      titleBtn.addEventListener("click", function () {
        openSession(row.id);
      });
      var actions = document.createElement("span");
      actions.className = "chat-session-actions";
      var renameBtn = iconBtn(
        "Rename",
        "Rename",
        '<path d="M12 20h9"/><path d="M16.5 3.5a2.1 2.1 0 0 1 3 3L7 19l-4 1 1-4Z"/>'
      );
      renameBtn.addEventListener("click", function (ev) {
        ev.stopPropagation();
        renameSession(row.id, row.title || "");
      });
      var delBtn = iconBtn(
        "Delete",
        "Delete",
        '<path d="M3 6h18"/><path d="M8 6V4h8v2"/><path d="M19 6l-1 14H6L5 6"/>'
      );
      delBtn.addEventListener("click", function (ev) {
        ev.stopPropagation();
        deleteSession(row.id, row.title || "Chat");
      });
      actions.appendChild(renameBtn);
      actions.appendChild(delBtn);
      item.appendChild(titleBtn);
      item.appendChild(actions);
      listEl.appendChild(item);
    });
  }

  function refreshSessions() {
    return fetch(basePath() + "/api/agent/sessions", { credentials: "same-origin" })
      .then(function (r) {
        return r.json().then(function (body) {
          return { ok: r.ok, body: body };
        });
      })
      .then(function (res) {
        if (res.ok) renderSessionList(res.body.sessions);
      })
      .catch(function () {});
  }

  function newChat() {
    stopLive();
    sessionId = null;
    persistSession();
    resetLog();
    if (sendBtn) sendBtn.disabled = false;
    if (stopBtn) {
      stopBtn.hidden = true;
      stopBtn.disabled = false;
    }
    if (statusEl) {
      statusEl.hidden = true;
      statusEl.textContent = "";
    }
    refreshSessions();
    if (inputEl) inputEl.focus();
  }

  function openSession(id) {
    if (!id || id === sessionId) return;
    stopLive();
    resetLog();
    sessionId = id;
    persistSession();
    restoreFromServer().then(function () {
      refreshSessions();
    });
  }

  function renameSession(id, currentTitle) {
    if (!window.WikiDialog) return;
    window.WikiDialog.prompt("Rename this chat to:", currentTitle).then(function (title) {
      if (title == null) return;
      title = String(title).trim();
      if (!title) return;
      fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(id) + "/title", {
        method: "POST",
        headers: csrfHeaders(),
        credentials: "same-origin",
        body: JSON.stringify({ title: title }),
      })
        .then(function (r) {
          return r.json().then(function (body) {
            return { ok: r.ok, body: body };
          });
        })
        .then(function (res) {
          if (!res.ok) {
            if (window.WikiDialog) {
              window.WikiDialog.alert((res.body && res.body.error) || "Rename failed");
            }
            return;
          }
          refreshSessions();
        })
        .catch(function (err) {
          if (window.WikiDialog) window.WikiDialog.alert(err.message || "Rename failed");
        });
    });
  }

  function deleteSession(id, title) {
    if (!window.WikiDialog) return;
    window.WikiDialog.confirm('Delete chat "' + title + '"? This cannot be undone.', {
      danger: true,
      okLabel: "Delete",
    }).then(function (ok) {
      if (!ok) return;
      fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(id), {
        method: "DELETE",
        headers: csrfHeaders(),
        credentials: "same-origin",
      })
        .then(function (r) {
          return r.json().then(function (body) {
            return { ok: r.ok, body: body };
          });
        })
        .then(function (res) {
          if (!res.ok) {
            if (window.WikiDialog) {
              window.WikiDialog.alert((res.body && res.body.error) || "Delete failed");
            }
            return;
          }
          if (sessionId === id) {
            stopLive();
            sessionId = null;
            persistSession();
            resetLog();
            if (sendBtn) sendBtn.disabled = false;
            if (stopBtn) {
              stopBtn.hidden = true;
              stopBtn.disabled = false;
            }
            if (statusEl) {
              statusEl.hidden = true;
              statusEl.textContent = "";
            }
          }
          refreshSessions();
        })
        .catch(function (err) {
          if (window.WikiDialog) window.WikiDialog.alert(err.message || "Delete failed");
        });
    });
  }

  function localPath() {
    var bp = basePath();
    var p = location.pathname;
    if (bp && p.indexOf(bp) === 0) p = p.slice(bp.length);
    return p || "/";
  }

  // The wiki page behind this panel — Chat's equivalent of Draft's
  // editor snapshot. The server cannot see the browser URL; this is
  // attached to every send so get_current_view / "this document" work.
  function currentView() {
    var path = localPath();
    var title = "";
    var h1 = document.querySelector("#app-content h1");
    if (h1) title = (h1.textContent || "").trim();
    if (path === "/" || path === "/search" || path === "/search/") {
      return { page: "search", path: "", title: title };
    }
    if (path === "/graph" || path === "/graph/") {
      return { page: "graph", path: "", title: title };
    }
    if (path === "/account" || path === "/account/") {
      return { page: "account", path: "", title: title };
    }
    if (path === "/folder" || path === "/folder/") {
      return { page: "folder", path: "", title: title };
    }
    if (path.indexOf("/folder/") === 0) {
      return { page: "folder", path: path.slice("/folder/".length), title: title };
    }
    if (path.indexOf("/d/") === 0) {
      return { page: "document", path: path.slice("/d/".length), title: title };
    }
    if (path.indexOf("/edit/") === 0) {
      return { page: "edit", path: path.slice("/edit/".length), title: title };
    }
    if (path.indexOf("/history/") === 0) {
      return { page: "history", path: path.slice("/history/".length), title: title };
    }
    return { page: "other", path: "", title: title };
  }

  function send() {
    var instruction = inputEl.value.trim();
    if (!instruction) return;
    sendBtn.disabled = true;
    var url = sessionId
      ? basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId) + "/messages"
      : basePath() + "/api/agent/sessions";
    var payload = { instruction: instruction, kind: "chat", view: currentView() };
    fetch(url, {
      method: "POST",
      headers: csrfHeaders(),
      credentials: "same-origin",
      body: JSON.stringify(payload),
    })
      .then(function (r) {
        return r.json().then(function (body) {
          return { ok: r.ok, status: r.status, body: body };
        });
      })
      .then(function (res) {
        if (!res.ok) {
          sendBtn.disabled = false;
          appendEvent({
            type: "error",
            data: { text: (res.body && res.body.error) || "request failed" },
          });
          return;
        }
        inputEl.value = "";
        renderView(res.body);
        refreshSessions();
      })
      .catch(function (err) {
        sendBtn.disabled = false;
        appendEvent({ type: "error", data: { text: err.message || "request failed" } });
      });
  }

  function stop() {
    if (!sessionId) return;
    stopBtn.disabled = true;
    fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId) + "/cancel", {
      method: "POST",
      headers: csrfHeaders(),
      credentials: "same-origin",
    })
      .then(function (r) {
        return r.json().then(function (body) {
          return { ok: r.ok, body: body };
        });
      })
      .then(function (res) {
        if (res.ok) {
          renderView(res.body);
          return;
        }
        stopBtn.disabled = false;
        appendEvent({
          type: "error",
          data: { text: (res.body && res.body.error) || "could not stop" },
        });
      })
      .catch(function (err) {
        stopBtn.disabled = false;
        appendEvent({ type: "error", data: { text: err.message || "could not stop" } });
      });
  }

  function open() {
    ensurePanel();
    applyGeom();
    panel.hidden = false;
    applySidebarWidth(savedSidebarWidth);
    persistOpen(true);
    keepOnScreen();
    refreshSessions();
    inputEl.focus();
  }

  function hide() {
    if (!panel) return;
    persistGeom();
    persistOpen(false);
    panel.hidden = true;
  }

  function toggle() {
    ensurePanel();
    if (panel.hidden) open();
    else hide();
  }

  function restoreFromServer() {
    if (!sessionId) return Promise.resolve();
    return fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId), {
      credentials: "same-origin",
    })
      .then(function (r) {
        return r.json().then(function (body) {
          return { ok: r.ok, status: r.status, body: body };
        });
      })
      .then(function (res) {
        if (res.ok) {
          resetLog();
          renderView(res.body);
          return;
        }
        if (res.status === 404) forgetSession();
      })
      .catch(function () {});
  }

  function boot() {
    if (booted) return;
    booted = true;
    sessionId = storageGet(STORE_ID) || null;
    var shouldOpen = storageGet(STORE_OPEN) === "1";
    ensurePanel();
    applyGeom();
    applySidebarWidth(savedSidebarWidth);
    var ready = Promise.all([
      sessionId ? restoreFromServer() : Promise.resolve(),
      refreshSessions(),
    ]);
    ready.then(function () {
      if (shouldOpen) open();
    });
  }

  window.addEventListener("pagehide", function () {
    persistGeom();
    if (panel) persistOpen(!panel.hidden);
    persistSession();
  });

  return {
    boot: boot,
    open: open,
    close: hide,
    toggle: toggle,
  };
})();
