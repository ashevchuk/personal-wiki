// Floating Draft panel on the edit page. Talks to /api/agent/sessions —
// wiki-server runs the tool loop against the vault and a cloud
// chat-completions API. MCP is not in this path.
window.WikiAgent = (function () {
  "use strict";

  var MIN_W = 280;
  var MIN_H = 220;

  var basePath = function () {
    return window.WikiCommon.basePath();
  };
  var getCookie = function (name) {
    return window.WikiCommon.getCookie(name);
  };

  var panel = null;
  var logEl = null;
  var inputEl = null;
  var sendBtn = null;
  var stopBtn = null;
  var revertBtn = null;
  var statusEl = null;
  var pendingEl = null;
  var selectionEl = null;
  var selectionTextEl = null;
  var sessionId = null;
  var lastSentBody = "";
  var pendingDraft = null;
  var undoState = null;
  var pollTimer = null;
  var hooks = null;
  var seenEvents = 0;

  function csrfHeaders() {
    return {
      "Content-Type": "application/json",
      "X-CSRF-Token": getCookie("wiki_csrf_token"),
    };
  }

  function clamp(n, lo, hi) {
    return Math.max(lo, Math.min(hi, n));
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
    var w = r.width;
    var h = r.height;
    var left = clamp(r.left, 8, Math.max(8, window.innerWidth - w - 8));
    var top = clamp(r.top, 8, Math.max(8, window.innerHeight - h - 8));
    panel.style.left = left + "px";
    panel.style.top = top + "px";
    panel.style.right = "auto";
    panel.style.bottom = "auto";
    panel.style.width = w + "px";
    panel.style.height = h + "px";
  }

  function bindPointerDrag(el, onMove, shouldIgnore) {
    el.addEventListener("pointerdown", function (ev) {
      if (ev.button !== 0) return;
      if (shouldIgnore && shouldIgnore(ev.target)) return;
      ev.preventDefault();
      pinToPixels();
      el.setPointerCapture(ev.pointerId);
      document.body.classList.add("agent-panel-dragging");
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
        document.body.classList.remove("agent-panel-dragging");
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
    panel.hidden = true;
    panel.innerHTML =
      '<div class="agent-panel-header">' +
      "<strong>Draft</strong>" +
      '<span class="agent-panel-header-actions">' +
      '<button type="button" class="agent-panel-help-btn" aria-label="How to use Draft" title="How to use Draft" aria-expanded="false">' +
      '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      '<path d="M9.1 9a3 3 0 0 1 5.83 1c0 2-3 3-3 4"/>' +
      '<circle cx="12" cy="17" r="0.85" fill="currentColor" stroke="none"/>' +
      "</svg></button>" +
      '<button type="button" class="agent-panel-close" aria-label="Close" title="Close">' +
      '<svg width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">' +
      '<path d="M18 6 6 18M6 6l12 12"/>' +
      "</svg></button>" +
      "</span></div>" +
      '<div class="agent-help" id="agent-help" hidden>' +
      "<p>Searches the vault, then fills this editor. <strong>Save</strong> is still the only write to disk.</p>" +
      "<ul>" +
      "<li>New note or a full rewrite: describe it and Send (Ctrl/Cmd+Enter).</li>" +
      "<li>Insert at the caret: click in the editor with no selection. The caret itself disappears when this panel takes focus (Toast UI); a chip remembers the insert point, same idea as a selection.</li>" +
      "<li>Change a fragment: select it in the editor first. This panel remembers the selection after the editor loses focus — a chip appears; Clear drops it and the highlight in the editor.</li>" +
      "<li>Follow-ups like \"add a paragraph\" or \"fix that span\" change only that part.</li>" +
      "<li>If the request is unclear, the agent asks here. Answer in this box; the editor will not change until you do.</li>" +
      "<li>Stop cancels a run. Revert undoes the last applied draft in the editor (Toast UI cannot).</li>" +
      "<li>Close hides the panel; Save keeps this session. View opens the saved document. Leaving the page drops the session.</li>" +
      "<li>If you typed while it was working, a full rewrite asks Apply anyway / Keep mine.</li>" +
      "</ul></div>" +
      '<div class="agent-panel-log" id="agent-log"></div>' +
      '<p class="agent-status" id="agent-status" hidden></p>' +
      '<div class="agent-panel-pending" id="agent-pending" hidden>' +
      "<p>The editor changed after this draft was sent. Applying it would overwrite your edits.</p>" +
      '<div class="agent-panel-actions">' +
      '<button type="button" id="agent-keep">Keep mine</button>' +
      '<button type="button" id="agent-apply-anyway">Apply anyway</button>' +
      "</div></div>" +
      '<p class="agent-selection" id="agent-selection" hidden>' +
      '<span class="agent-selection-text"></span>' +
      '<button type="button" class="agent-selection-clear">Clear</button></p>' +
      "<textarea id=\"agent-input\" rows=\"4\" placeholder=\"Describe the note to draft…\"></textarea>" +
      '<div class="agent-panel-actions">' +
      '<button type="button" id="agent-stop" hidden>Stop</button>' +
      '<button type="button" id="agent-revert" hidden>Revert last draft</button>' +
      '<button type="button" id="agent-send">Send</button>' +
      "</div>" +
      '<div class="agent-panel-resize" aria-hidden="true"></div>';
    document.body.appendChild(panel);
    logEl = panel.querySelector("#agent-log");
    inputEl = panel.querySelector("#agent-input");
    sendBtn = panel.querySelector("#agent-send");
    stopBtn = panel.querySelector("#agent-stop");
    revertBtn = panel.querySelector("#agent-revert");
    statusEl = panel.querySelector("#agent-status");
    pendingEl = panel.querySelector("#agent-pending");
    selectionEl = panel.querySelector("#agent-selection");
    selectionTextEl = panel.querySelector(".agent-selection-text");
    panel.addEventListener(
      "pointerdown",
      function (ev) {
        if (ev.target.closest(".agent-selection-clear")) return;
        if (hooks && hooks.captureSelection) hooks.captureSelection();
        refreshSelectionChip();
      },
      true
    );
    panel.querySelector(".agent-panel-close").addEventListener("click", function (ev) {
      ev.stopPropagation();
      hide();
    });
    var helpBtn = panel.querySelector(".agent-panel-help-btn");
    var helpEl = panel.querySelector("#agent-help");
    helpBtn.addEventListener("click", function (ev) {
      ev.stopPropagation();
      var open = helpEl.hidden;
      helpEl.hidden = !open;
      helpBtn.setAttribute("aria-expanded", open ? "true" : "false");
    });
    sendBtn.addEventListener("click", send);
    stopBtn.addEventListener("click", stop);
    revertBtn.addEventListener("click", revertLastDraft);
    panel.querySelector("#agent-apply-anyway").addEventListener("click", applyPending);
    panel.querySelector("#agent-keep").addEventListener("click", keepMine);
    panel.querySelector(".agent-selection-clear").addEventListener("click", function (ev) {
      ev.stopPropagation();
      if (hooks && hooks.clearSelection) hooks.clearSelection();
      refreshSelectionChip();
    });
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
    });
    window.addEventListener("resize", function () {
      if (panel && !panel.hidden) keepOnScreen();
    });
  }

  function currentBody() {
    return hooks && hooks.currentBody ? hooks.currentBody() : "";
  }

  function hidePending() {
    pendingDraft = null;
    if (pendingEl) pendingEl.hidden = true;
  }

  function showPending() {
    if (pendingEl) pendingEl.hidden = false;
  }

  function selectionPreview(text) {
    var oneLine = String(text || "").replace(/\s+/g, " ").trim();
    if (oneLine.length > 72) oneLine = oneLine.slice(0, 71) + "…";
    return oneLine;
  }

  // The insert point is the *end* of this prefix. Showing the start of
  // the document (selectionPreview) made the chip look unrelated to the
  // caret. Keep the last ~48 visible characters, then │.
  function caretPreview(text) {
    var oneLine = String(text || "").replace(/\s+/g, " ").trim();
    if (oneLine.length > 80) oneLine = "…" + oneLine.slice(-80);
    return oneLine;
  }

  function fillCaretChip(label, tail) {
    selectionEl.classList.add("agent-selection--caret");
    selectionTextEl.textContent = "";
    var lab = document.createElement("span");
    lab.className = "agent-selection-label";
    lab.textContent = label;
    selectionTextEl.appendChild(lab);
    if (!tail) return;
    var wrap = document.createElement("span");
    wrap.className = "agent-caret-tail";
    var inner = document.createElement("span");
    inner.className = "agent-caret-tail-inner";
    inner.textContent = tail;
    wrap.appendChild(inner);
    selectionTextEl.appendChild(wrap);
  }

  function refreshSelectionChip() {
    if (!selectionEl) return;
    var text = hooks && hooks.currentSelection ? hooks.currentSelection() : "";
    if (text) {
      selectionEl.hidden = false;
      selectionEl.classList.remove("agent-selection--caret");
      selectionTextEl.textContent = "Selection: " + selectionPreview(text);
      return;
    }
    var caret = hooks && hooks.currentCaret ? hooks.currentCaret() : null;
    if (caret) {
      selectionEl.hidden = false;
      var prev = caretPreview(caret.preview || caret.before);
      if (!caret.known && !prev) {
        fillCaretChip("Insert at caret");
      } else if (!prev) {
        fillCaretChip("Insert at caret", "start of document");
      } else {
        fillCaretChip("Insert at caret", prev + "│");
      }
      return;
    }
    selectionEl.hidden = true;
    selectionEl.classList.remove("agent-selection--caret");
  }

  function syncSelection() {
    if (!panel || panel.hidden) return;
    refreshSelectionChip();
  }

  function refreshRevert() {
    if (revertBtn) revertBtn.hidden = !undoState;
  }

  function rememberUndo() {
    if (!hooks || !hooks.editorState) return;
    undoState = hooks.editorState();
    refreshRevert();
  }

  function revertLastDraft() {
    if (!undoState || !hooks || !hooks.restoreEditorState) return;
    hooks.restoreEditorState(undoState);
    undoState = null;
    lastSentBody = currentBody();
    refreshRevert();
    refreshSelectionChip();
    appendEvent({
      type: "assistant",
      data: { text: "Reverted the last draft in the editor." },
    });
  }

  function applyFull(d) {
    if (!d) return;
    rememberUndo();
    if (hooks && hooks.applyDraft) hooks.applyDraft(d);
    hidePending();
    refreshSelectionChip();
  }

  function applyPending() {
    var d = pendingDraft;
    if (!d) return;
    applyFull(d);
    appendEvent({ type: "draft", data: { title: d.title, path: d.path } });
  }

  function keepMine() {
    hidePending();
    appendEvent({
      type: "assistant",
      data: { text: "Kept your editor text. The draft was not applied." },
    });
  }

  function editorChangedSinceSend() {
    return currentBody() !== lastSentBody;
  }

  function applyEdit(data) {
    if (!hooks) return;
    var prev = hooks.editorState ? hooks.editorState() : null;
    if (data.op === "append" && hooks.appendToBody) {
      hooks.appendToBody(data.text || "");
      undoState = prev;
      refreshRevert();
      lastSentBody = currentBody();
      refreshSelectionChip();
      return;
    }
    if (data.op === "insert" && hooks.insertInBody) {
      var inserted = hooks.insertInBody(data.after || "", data.text || "");
      if (!inserted) {
        appendEvent({
          type: "error",
          data: { text: "Could not insert at the caret — the editor moved." },
        });
        return;
      }
      undoState = prev;
      refreshRevert();
      lastSentBody = currentBody();
      refreshSelectionChip();
      return;
    }
    if (data.op === "replace" && hooks.replaceInBody) {
      var ok = hooks.replaceInBody(data.find || "", data.replacement || "");
      if (!ok) {
        appendEvent({
          type: "error",
          data: { text: "Could not apply replace — that text is no longer in the editor." },
        });
        return;
      }
      undoState = prev;
      refreshRevert();
      lastSentBody = currentBody();
      refreshSelectionChip();
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
      p.textContent = data.text || "";
    } else if (type === "tool") {
      var extra = data.detail ? " " + data.detail : "";
      if (typeof data.count === "number") extra += " (" + data.count + ")";
      p.textContent = (data.name || "tool") + extra;
    } else if (type === "draft") {
      p.textContent = "Filled the editor: " + (data.title || data.path || "");
    } else if (type === "edit") {
      if (data.op === "append") p.textContent = "Appended to the editor.";
      else if (data.op === "insert") p.textContent = "Inserted at the caret.";
      else if (data.op === "replace") p.textContent = "Updated text in the editor.";
      else p.textContent = "Updated the editor.";
    } else if (type === "error") {
      p.className = "agent-error";
      p.textContent = data.text || "error";
    } else if (type === "cancelled") {
      p.textContent = "Stopped.";
    } else if (type === "done") {
      return;
    } else {
      p.textContent = type;
    }
    logEl.appendChild(p);
    logEl.scrollTop = logEl.scrollHeight;
  }

  function setRunning(running) {
    sendBtn.disabled = running;
    stopBtn.hidden = !running;
    stopBtn.disabled = !running;
    statusEl.hidden = !running;
    statusEl.textContent = running ? "Working…" : "";
    if (running) startPoll();
    else stopPoll();
  }

  function renderView(view) {
    if (!view) return;
    sessionId = view.id;
    var events = view.events || [];
    for (var i = seenEvents; i < events.length; i++) {
      var ev = events[i];
      if (ev.type === "draft") {
        if (editorChangedSinceSend()) {
          pendingDraft = view.draft;
          showPending();
          appendEvent({
            type: "assistant",
            data: { text: "Draft ready, but the editor changed after this turn was sent." },
          });
        } else {
          applyFull(view.draft);
          appendEvent(ev);
        }
      } else if (ev.type === "edit") {
        applyEdit(ev.data || {});
        appendEvent(ev);
      } else {
        appendEvent(ev);
      }
    }
    seenEvents = events.length;
    setRunning(view.status === "running");
  }

  function startPoll() {
    if (pollTimer) return;
    pollTimer = setInterval(function () {
      if (!sessionId) return;
      fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId), {
        credentials: "same-origin",
      })
        .then(function (r) {
          return r.json().then(function (body) {
            return { ok: r.ok, body: body };
          });
        })
        .then(function (res) {
          if (res.ok) renderView(res.body);
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

  function snapshotPayload(instruction) {
    var snap = hooks && hooks.snapshot ? hooks.snapshot() : {};
    snap.instruction = instruction;
    lastSentBody = snap.body || "";
    return snap;
  }

  function send() {
    var instruction = inputEl.value.trim();
    if (!instruction) return;
    sendBtn.disabled = true;
    hidePending();
    var payload = snapshotPayload(instruction);
    if (hooks && hooks.clearSelection) hooks.clearSelection();
    refreshSelectionChip();
    var url = sessionId
      ? basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId) + "/messages"
      : basePath() + "/api/agent/sessions";
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

  function open(nextHooks) {
    hooks = nextHooks || {};
    ensurePanel();
    if (hooks.captureSelection) hooks.captureSelection();
    refreshSelectionChip();
    panel.hidden = false;
    inputEl.focus();
  }

  function hide() {
    if (panel) panel.hidden = true;
  }

  function endSession() {
    stopPoll();
    if (sessionId) {
      fetch(basePath() + "/api/agent/sessions/" + encodeURIComponent(sessionId), {
        method: "DELETE",
        headers: csrfHeaders(),
        credentials: "same-origin",
        keepalive: true,
      }).catch(function () {});
    }
    sessionId = null;
    lastSentBody = "";
    pendingDraft = null;
    undoState = null;
    seenEvents = 0;
    if (logEl) logEl.innerHTML = "";
    hidePending();
    refreshRevert();
    hide();
  }

  window.addEventListener("pagehide", endSession);

  return {
    open: open,
    close: hide,
    endSession: endSession,
    syncSelection: syncSelection,
  };
})();
