// A custom dropdown replacing every native <select> in this app's own
// pages (calendar's Month/Year, the date-time picker's Repeats/Hour/
// Minute). Not a cosmetic preference: a native <select>'s open option
// list is OS/browser chrome, the same category of thing as a native
// <input type="date">'s pop-up calendar this project already replaced
// wholesale in date-time-picker.js — it ignores color-scheme reliably
// enough to render in the wrong theme regardless of CSS (see edit.css's
// own comment on that), and this app's own browser-automation tooling
// cannot see into it at all to verify it visually. A plain button +
// floating list is ordinary DOM this stylesheet fully controls, exactly
// like date-time-picker.js's own Until popup.
window.WikiDropdown = (function () {
  "use strict";

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === "text") e.textContent = attrs[k];
        else e.setAttribute(k, attrs[k]);
      });
    }
    (children || []).forEach(function (c) {
      if (c) e.appendChild(c);
    });
    return e;
  }

  // opts: {options: [{value, label}], value: initial value (matched by
  // String() equality), onChange: function(value), ariaLabel: string,
  // triggerClass/popupClass: extra CSS classes}. Returns {element,
  // setValue(value), getValue()} — element is the trigger button;
  // mount it wherever a <select> would have gone.
  function create(opts) {
    opts = opts || {};
    var options = opts.options || [];
    var value = opts.value !== undefined ? String(opts.value) : "";
    var onChange = opts.onChange || function () {};

    var trigger = el("button", {
      type: "button",
      class: "wiki-dd-trigger" + (opts.triggerClass ? " " + opts.triggerClass : ""),
      "aria-label": opts.ariaLabel || "",
      "aria-haspopup": "listbox",
      "aria-expanded": "false",
    });
    var popup = el("div", {
      class: "wiki-dd-popup" + (opts.popupClass ? " " + opts.popupClass : ""),
      role: "listbox",
    });
    popup.hidden = true;

    function labelFor(v) {
      var match = options.filter(function (o) {
        return String(o.value) === v;
      })[0];
      return match ? match.label : "";
    }

    function renderPopup() {
      popup.innerHTML = "";
      options.forEach(function (o) {
        var isSelected = String(o.value) === value;
        var item = el("div", {
          class: "wiki-dd-option" + (isSelected ? " wiki-dd-option-selected" : ""),
          role: "option",
          "aria-selected": isSelected ? "true" : "false",
          text: o.label,
        });
        item.addEventListener("click", function (ev) {
          ev.stopPropagation();
          // preventDefault, not just stopPropagation: this option is a
          // plain, non-interactive <div>, and a dropdown instance can
          // end up nested inside an unrelated <label> (the date-time
          // picker's compact Until popup lives inside the "Until"
          // radio's own <label>). Clicking any non-form-control
          // descendant of a <label> makes the browser synthesize a
          // SECOND click directly on that label's associated control as
          // a native default action -- a separate event with its own
          // fresh bubble path that stopPropagation here never touches.
          // Only preventDefault cancels that default action; calling it
          // here is a harmless no-op everywhere this dropdown ISN'T
          // inside a <label> (a plain <div> click has no default action
          // of its own to cancel).
          ev.preventDefault();
          value = String(o.value);
          trigger.textContent = labelFor(value);
          close();
          onChange(value);
        });
        popup.appendChild(item);
      });
    }

    function open() {
      renderPopup();
      var rect = trigger.getBoundingClientRect();
      popup.style.top = rect.bottom + 4 + "px";
      popup.style.left = rect.left + "px";
      popup.hidden = false;
      trigger.setAttribute("aria-expanded", "true");
    }
    function close() {
      popup.hidden = true;
      trigger.setAttribute("aria-expanded", "false");
    }

    trigger.textContent = labelFor(value);
    trigger.addEventListener("click", function (ev) {
      ev.stopPropagation();
      if (popup.hidden) open();
      else close();
    });
    popup.addEventListener("click", function (ev) {
      ev.stopPropagation();
    });
    // Close on any click elsewhere in the document -- the standard
    // "click outside to dismiss" a floating picker needs. One
    // document-level listener per dropdown instance is fine at this
    // app's scale (a handful of dropdowns ever open on one page at
    // once); it's removed along with everything else when the
    // instance's own elements are GC'd (no page here ever creates and
    // discards thousands of these).
    document.addEventListener("click", close);

    var wrap = el("span", { class: "wiki-dd" }, [trigger, popup]);

    return {
      element: wrap,
      setValue: function (v) {
        value = String(v);
        trigger.textContent = labelFor(value);
      },
      getValue: function () {
        return value;
      },
    };
  }

  return { create: create };
})();
