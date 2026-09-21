// In-theme replacements for window.alert / confirm / prompt. The native
// ones are the host OS's chrome (the screenshot of folder Rename/Move
// was a grey "10.100.100.2 says" box on top of the green page) — they
// cannot pick up --fg/--panel-bg, and a themed wiki that otherwise
// never leaves its own palette shouldn't punch a hole there for the
// one confirm/prompt an admin hits. <dialog>.showModal() is the
// browser's own modal primitive (focus trap, Escape, inert backdrop);
// we only restyle it. Messages go in textContent, never innerHTML —
// document titles and error.what() can contain "<".
window.WikiDialog = (function () {
  "use strict";

  var active = null;

  function finish(result) {
    if (!active) return;
    var slot = active;
    active = null;
    if (slot.dialog.open) slot.dialog.close();
    if (slot.dialog.parentNode) slot.dialog.parentNode.removeChild(slot.dialog);
    slot.resolve(result);
  }

  function open(opts) {
    return new Promise(function (resolve) {
      if (active) {
        var prevKind = active.kind;
        finish(prevKind === "prompt" ? null : prevKind === "confirm" ? false : undefined);
      }

      var dialog = document.createElement("dialog");
      dialog.className = "wiki-dialog" + (opts.danger ? " wiki-dialog--danger" : "");

      var msg = document.createElement("p");
      msg.className = "wiki-dialog-msg";
      msg.id = "wiki-dialog-msg";
      msg.textContent = opts.message || "";
      dialog.setAttribute("aria-labelledby", "wiki-dialog-msg");
      dialog.appendChild(msg);

      var form = document.createElement("form");
      form.method = "dialog";

      var input = null;
      if (opts.kind === "prompt") {
        input = document.createElement("input");
        input.type = "text";
        input.className = "wiki-dialog-input";
        input.value = opts.defaultValue || "";
        input.setAttribute("aria-label", opts.message || "Value");
        form.appendChild(input);
      }

      var actions = document.createElement("div");
      actions.className = "wiki-dialog-actions";

      if (opts.kind !== "alert") {
        var cancel = document.createElement("button");
        cancel.type = "button";
        cancel.textContent = "Cancel";
        cancel.addEventListener("click", function () {
          finish(opts.kind === "prompt" ? null : false);
        });
        actions.appendChild(cancel);
      }

      var ok = document.createElement("button");
      ok.type = "submit";
      ok.className = "wiki-dialog-ok";
      ok.textContent = opts.okLabel || "OK";
      actions.appendChild(ok);
      form.appendChild(actions);
      dialog.appendChild(form);

      form.addEventListener("submit", function (ev) {
        ev.preventDefault();
        if (opts.kind === "prompt") finish(input.value);
        else if (opts.kind === "confirm") finish(true);
        else finish();
      });

      dialog.addEventListener("cancel", function (ev) {
        ev.preventDefault();
        finish(opts.kind === "prompt" ? null : opts.kind === "confirm" ? false : undefined);
      });

      document.body.appendChild(dialog);
      active = { dialog: dialog, resolve: resolve, kind: opts.kind };
      dialog.showModal();
      if (input) {
        input.focus();
        input.select();
      } else {
        ok.focus();
      }
    });
  }

  return {
    alert: function (message) {
      return open({ kind: "alert", message: message });
    },
    confirm: function (message, opts) {
      opts = opts || {};
      return open({
        kind: "confirm",
        message: message,
        danger: !!opts.danger,
        okLabel: opts.okLabel || "OK",
      });
    },
    prompt: function (message, defaultValue) {
      return open({
        kind: "prompt",
        message: message,
        defaultValue: defaultValue || "",
      });
    },
  };
})();
