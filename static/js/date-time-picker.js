// A single modal that replaces three inline native controls (Due date,
// Time, Repeats) on the edit form with one custom-built picker: a month
// calendar grid, hour/minute dropdowns, and a recurrence constructor
// (frequency + interval + an end condition) instead of hand-typing the
// `;interval=/;until=/;count=` DSL. Built from scratch rather than a
// vendored date-picker library (this project adds a new dependency only
// when it earns its keep — Toast UI Editor/Prism/mermaid already are
// three; a month grid is a small, well-understood piece of UI) and
// specifically to sidestep every native <input type="date"/"time">
// cross-browser quirk this session already hit: 12h-vs-24h formatting
// that depends on browser/OS locale in ways `lang` doesn't reliably
// override everywhere, and pop-up pickers that are OS/browser chrome —
// CSS (and this app's own automation tooling) cannot reach into them at
// all. A plain <dialog> with ordinary DOM controls sidesteps both
// categories of problem entirely: every pixel is this stylesheet's own.
//
// Public API: WikiDateTimePicker.open({due, recur}) -> Promise resolving
// to {due, recur} on Save, or null on Cancel/Escape — same shape as
// WikiDialog's own promise-based confirm/prompt. formatSummary(due,
// recur) renders the compact trigger-button label ("Oct 5, 2026 · 16:00
// · Weekly", "No due date") so edit.js never has to know this file's
// internal recurrence-DSL parsing.
window.WikiDateTimePicker = (function () {
  "use strict";

  var MONTH_NAMES = [
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
  ];
  var MONTH_SHORT = MONTH_NAMES.map(function (m) { return m.slice(0, 3); });
  var DAY_NAMES = ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"];
  var FREQ_LABELS = { daily: "day", weekly: "week", monthly: "month", yearly: "year" };

  function pad2(n) {
    return n < 10 ? "0" + n : String(n);
  }
  function isoDate(y, mZeroBased, d) {
    return y + "-" + pad2(mZeroBased + 1) + "-" + pad2(d);
  }
  function dateFromIso(iso) {
    var p = iso.split("-");
    return new Date(Number(p[0]), Number(p[1]) - 1, Number(p[2]));
  }
  function daysInMonth(y, mZeroBased) {
    return new Date(y, mZeroBased + 1, 0).getDate();
  }
  // Monday=0 .. Sunday=6, matching the Month view on /calendar.
  function mondayFirstWeekday(d) {
    return (d.getDay() + 6) % 7;
  }
  function todayIso() {
    var d = new Date();
    return isoDate(d.getFullYear(), d.getMonth(), d.getDate());
  }

  // "freq;interval=N;until=DATE;count=N" -> a plain object this file's
  // own constructor UI can populate itself from. Deliberately not the
  // same grammar as CalendarQueries.cpp's own parseRecurrenceRule (that
  // one validates and expands occurrences; this one only ever has to
  // round-trip what buildRecur below already produced, or what a human
  // hand-typed into front matter directly before this UI existed) — a
  // value this can't make sense of just falls back to "does not
  // repeat" in the constructor rather than erroring, matching
  // CalendarQueries' own "a typo degrades to a one-off" philosophy.
  function parseRecur(recur) {
    var out = { freq: "", interval: 1, endMode: "never", until: "", count: "" };
    if (!recur) return out;
    var parts = recur.split(";");
    if (FREQ_LABELS[parts[0]] === undefined) return out;
    out.freq = parts[0];
    for (var i = 1; i < parts.length; i++) {
      var eq = parts[i].indexOf("=");
      if (eq === -1) continue;
      var key = parts[i].slice(0, eq);
      var value = parts[i].slice(eq + 1);
      if (key === "interval") {
        var n = parseInt(value, 10);
        if (n > 0) out.interval = n;
      } else if (key === "until") {
        out.endMode = "until";
        out.until = value;
      } else if (key === "count") {
        var c = parseInt(value, 10);
        if (c > 0) {
          out.endMode = "count";
          out.count = c;
        }
      }
    }
    return out;
  }
  function buildRecur(state) {
    if (!state.freq) return "";
    var s = state.freq;
    if (state.interval > 1) s += ";interval=" + state.interval;
    if (state.endMode === "until" && state.until) {
      s += ";until=" + state.until;
    } else if (state.endMode === "count" && state.count) {
      s += ";count=" + state.count;
    }
    return s;
  }

  function splitDue(due) {
    if (!due) return { date: "", time: "" };
    var t = due.indexOf("T");
    return t === -1 ? { date: due, time: "" } : { date: due.slice(0, t), time: due.slice(t + 1) };
  }

  function formatSummary(due, recur) {
    if (!due) return "No due date";
    var parts = splitDue(due);
    var d = dateFromIso(parts.date);
    var label = MONTH_SHORT[d.getMonth()] + " " + d.getDate() + ", " + d.getFullYear();
    if (parts.time) label += " · " + parts.time;
    var r = parseRecur(recur);
    if (r.freq) {
      var unit = FREQ_LABELS[r.freq];
      var freqLabel = r.interval > 1
        ? "Every " + r.interval + " " + unit + "s"
        : r.freq.charAt(0).toUpperCase() + r.freq.slice(1);
      label += " · " + freqLabel;
    }
    return label;
  }

  function el(tag, attrs, children) {
    var e = document.createElement(tag);
    if (attrs) {
      Object.keys(attrs).forEach(function (k) {
        if (k === "text") e.textContent = attrs[k];
        else if (k === "html") e.innerHTML = attrs[k];
        else e.setAttribute(k, attrs[k]);
      });
    }
    (children || []).forEach(function (c) {
      if (c) e.appendChild(c);
    });
    return e;
  }

  // One month-grid calendar, used twice: the main Due date (full size)
  // and the Repeats "Until" end date (compact) -- same widget, so Until
  // never falls back to the native <input type="date"> this whole file
  // exists to avoid (locale-dependent formatting, an OS pop-up CSS can't
  // reach, an indicator icon that stayed stubbornly white against a
  // dark theme). Always renders 6 full weeks (42 cells), padding BOTH
  // ends with the adjacent month's days rather than just leading blanks
  // -- a 4-week October next to a 6-week November otherwise resizes the
  // whole dialog on every Prev/Next click, found live navigating months
  // in the Due calendar. Returns {element, setSelected}; the caller owns
  // when the widget is shown/hidden (e.g. only while the "Until" radio
  // is checked) and when it goes away (removed along with the rest of
  // the dialog's DOM on Cancel/Save, no separate teardown needed).
  function createCalendarWidget(opts) {
    opts = opts || {};
    var compact = !!opts.compact;
    var onSelect = opts.onSelect || function () {};
    var selected = opts.initialIso || null;
    var anchor = selected ? dateFromIso(selected) : new Date();

    var label = el("span", { class: "dtp-cal-label" });
    var prev = el("button", { type: "button", class: "dtp-cal-nav", "aria-label": "Previous month", text: "‹" });
    var next = el("button", { type: "button", class: "dtp-cal-nav", "aria-label": "Next month", text: "›" });
    var header = el("div", { class: "dtp-cal-header" }, [prev, label, next]);
    var grid = el("div", { class: compact ? "dtp-cal-grid dtp-cal-grid--compact" : "dtp-cal-grid" });
    var container = el("div", { class: compact ? "dtp-calendar dtp-calendar--compact" : "dtp-calendar" }, [header, grid]);

    function renderLabel(year, month) {
      // Month/Year dropdowns instead of a plain text label, same as
      // /calendar's own renderMonthLabel -- on both the full-size Due
      // calendar and the compact Until popup. Compact uses the 3-letter
      // month names ("Sep") and a tighter trigger class so the popup
      // (floating, not constrained by the inline form layout) stays
      // narrow instead of having to fit "September".
      var monthNames = compact ? MONTH_SHORT : MONTH_NAMES;
      var monthOptions = monthNames.map(function (name, m) {
        return { value: m, label: name };
      });
      var todayYear = new Date().getFullYear();
      var minYear = Math.min(todayYear - 10, year);
      var maxYear = Math.max(todayYear + 10, year);
      var yearOptions = [];
      for (var y = minYear; y <= maxYear; y++) {
        yearOptions.push({ value: y, label: String(y) });
      }
      var triggerClass = "calendar-label-select dtp-cal-label-select" +
        (compact ? " dtp-cal-label-select--compact" : "");
      var monthDd = WikiDropdown.create({
        options: monthOptions,
        value: month,
        ariaLabel: "Month",
        triggerClass: triggerClass,
        onChange: function (v) {
          anchor = new Date(Number(yearDd.getValue()), Number(v), 1);
          render();
        },
      });
      var yearDd = WikiDropdown.create({
        options: yearOptions,
        value: year,
        ariaLabel: "Year",
        triggerClass: triggerClass,
        onChange: function (v) {
          anchor = new Date(Number(v), Number(monthDd.getValue()), 1);
          render();
        },
      });
      label.innerHTML = "";
      label.appendChild(monthDd.element);
      label.appendChild(yearDd.element);
    }
    function render() {
      var year = anchor.getFullYear();
      var month = anchor.getMonth();
      renderLabel(year, month);
      grid.innerHTML = "";
      DAY_NAMES.forEach(function (d) {
        grid.appendChild(el("div", { class: "dtp-cal-weekday", text: d }));
      });
      var firstWeekday = mondayFirstWeekday(new Date(year, month, 1));
      var total = daysInMonth(year, month);
      var today = todayIso();
      // Leading days from the previous month, so week 1 is never a
      // half-empty row -- purely visual filler, never selectable.
      var prevMonthDays = daysInMonth(year, month - 1);
      for (var lead = firstWeekday; lead > 0; lead--) {
        grid.appendChild(el("div", { class: "dtp-cal-cell dtp-cal-cell-pad", text: String(prevMonthDays - lead + 1) }));
      }
      var dayCell = function (day) {
        var iso = isoDate(year, month, day);
        var cls = "dtp-cal-cell dtp-cal-day";
        if (iso === today) cls += " dtp-cal-today";
        if (iso === selected) cls += " dtp-cal-selected";
        var cell = el("button", { type: "button", class: cls, text: String(day) });
        cell.addEventListener("click", function () {
          selected = iso;
          render();
          onSelect(iso);
        });
        grid.appendChild(cell);
      };
      for (var day = 1; day <= total; day++) dayCell(day);
      // Trailing filler to always reach 42 cells (6 full weeks) -- the
      // fixed-height point of this whole function.
      var usedCells = firstWeekday + total;
      var trailing = 42 - usedCells;
      for (var t = 1; t <= trailing; t++) {
        grid.appendChild(el("div", { class: "dtp-cal-cell dtp-cal-cell-pad", text: String(t) }));
      }
    }
    prev.addEventListener("click", function () {
      anchor = new Date(anchor.getFullYear(), anchor.getMonth() - 1, 1);
      render();
    });
    next.addEventListener("click", function () {
      anchor = new Date(anchor.getFullYear(), anchor.getMonth() + 1, 1);
      render();
    });
    render();

    return {
      element: container,
      setSelected: function (iso) {
        selected = iso;
        anchor = iso ? dateFromIso(iso) : anchor;
        render();
      },
    };
  }

  function open(current) {
    return new Promise(function (resolve) {
      var parts = splitDue(current.due || "");
      var selectedDate = parts.date || null; // ISO date string, or null = no due date
      var selectedTime = parts.time || ""; // "HH:MM" or "" for all-day
      var recurState = parseRecur(current.recur || "");
      var finished = false;

      var dialog = el("dialog", { class: "wiki-dialog date-time-picker-dialog" });
      dialog.setAttribute("aria-labelledby", "dtp-title");

      function finish(result) {
        if (finished) return;
        finished = true;
        if (dialog.open) dialog.close();
        if (dialog.parentNode) dialog.parentNode.removeChild(dialog);
        resolve(result);
      }

      // --- Calendar widget (Due date) -------------------------------------
      var dueCalendar = createCalendarWidget({
        initialIso: selectedDate,
        onSelect: function (iso) {
          selectedDate = iso;
          updateSummaryPreview();
        },
      });

      // --- Time row (hour/minute dropdowns + an all-day toggle) ----------
      var allDayCheckbox = el("input", { type: "checkbox", id: "dtp-all-day" });
      allDayCheckbox.checked = !selectedTime;
      var hourOptions = [];
      for (var h = 0; h < 24; h++) hourOptions.push({ value: pad2(h), label: pad2(h) });
      var minuteOptions = [];
      for (var m = 0; m < 60; m++) minuteOptions.push({ value: pad2(m), label: pad2(m) });
      var hourDd = WikiDropdown.create({
        options: hourOptions,
        value: selectedTime ? selectedTime.slice(0, 2) : "00",
        ariaLabel: "Hour",
        triggerClass: "dtp-time-select",
        onChange: function () {
          updateSummaryPreview();
        },
      });
      var minuteDd = WikiDropdown.create({
        options: minuteOptions,
        value: selectedTime ? selectedTime.slice(3, 5) : "00",
        ariaLabel: "Minute",
        triggerClass: "dtp-time-select",
        onChange: function () {
          updateSummaryPreview();
        },
      });
      var timeFields = el("span", { class: "dtp-time-fields" }, [hourDd.element, el("span", { text: ":" }), minuteDd.element]);
      function syncTimeVisibility() {
        timeFields.hidden = allDayCheckbox.checked;
      }
      allDayCheckbox.addEventListener("change", syncTimeVisibility);
      syncTimeVisibility();

      var timeRow = el("div", { class: "dtp-time-row" }, [
        el("label", { class: "dtp-all-day-label" }, [allDayCheckbox, el("span", { text: " All day" })]),
        timeFields,
      ]);

      // --- Repeats constructor --------------------------------------------
      var freqDd = WikiDropdown.create({
        options: [
          { value: "", label: "Does not repeat" },
          { value: "daily", label: "Daily" },
          { value: "weekly", label: "Weekly" },
          { value: "monthly", label: "Monthly" },
          { value: "yearly", label: "Yearly" },
        ],
        value: recurState.freq,
        ariaLabel: "Repeats",
        onChange: function () {
          syncRepeatVisibility();
          updateSummaryPreview();
        },
      });

      var intervalInput = el("input", { type: "number", min: "1", class: "dtp-interval-input" });
      intervalInput.value = String(recurState.interval);
      var intervalUnitLabel = el("span", { class: "dtp-interval-unit" });

      var endNever = el("input", { type: "radio", name: "dtp-end", value: "never" });
      var endUntil = el("input", { type: "radio", name: "dtp-end", value: "until" });
      var endCount = el("input", { type: "radio", name: "dtp-end", value: "count" });
      [endNever, endUntil, endCount].forEach(function (r) {
        r.checked = r.value === recurState.endMode;
      });
      // Until is a compact trigger button ("Dec 31, 2026" / "No end
      // date") that opens the SAME calendar widget as the Due date, as
      // a floating popup anchored under it -- not a permanently-expanded
      // inline calendar (too tall for something used this rarely), and
      // not a native <input type="date"> (exactly what this whole file
      // exists to replace for Due; Until falling back to it would bring
      // the white-icon-on-dark-theme problem right back for one field).
      var untilIso = recurState.until || null;
      function formatShortDate(iso) {
        if (!iso) return "No end date";
        var d = dateFromIso(iso);
        return MONTH_SHORT[d.getMonth()] + " " + d.getDate() + ", " + d.getFullYear();
      }
      var untilTrigger = el("button", {
        type: "button", class: "dtp-until-trigger", text: formatShortDate(untilIso),
      });
      var untilPopup = el("div", { class: "dtp-until-popup" });
      untilPopup.hidden = true;
      var untilCalendar = createCalendarWidget({
        compact: true,
        initialIso: untilIso,
        onSelect: function (iso) {
          untilIso = iso;
          untilTrigger.textContent = formatShortDate(iso);
          untilPopup.hidden = true;
          updateSummaryPreview();
        },
      });
      untilPopup.appendChild(untilCalendar.element);
      // Clicks anywhere inside the popup (month nav, a day cell) must
      // not reach the form-level listener below that closes it on an
      // outside click -- otherwise navigating to the next month would
      // immediately close the very popup just being navigated.
      //
      // preventDefault matters here for a second, less obvious reason:
      // this whole popup lives inside the "Until" radio's own <label>
      // (see endRow below). Clicking a non-interactive descendant of a
      // <label> -- a plain <div>, e.g. .dtp-cal-weekday -- makes the
      // browser synthesize a SEPARATE click directly on the label's
      // associated control (the radio) as a native default action; that
      // synthetic click starts its own fresh bubble path from the radio
      // and reaches the form listener untouched by stopPropagation here,
      // since it isn't propagation of THIS click at all. preventDefault
      // cancels that default action. (A dropdown option inside this
      // popup is the same kind of non-interactive <div> and hits this
      // exact problem too, but its own click handler stops propagation
      // before the event ever reaches this listener -- its matching
      // preventDefault lives in dropdown.js instead. A day cell or the
      // month-nav arrows, both real <button>s, are exempt: per spec a
      // label only delegates to its control for non-form-control
      // descendants.) Found live: picking a month from the Month/Year
      // dropdowns closed the whole popup instead of just updating it --
      // invisible before those dropdowns existed because picking a day
      // already closes the popup on purpose (onSelect below), so the
      // same stray synthetic click was firing unnoticed.
      untilPopup.addEventListener("click", function (ev) {
        ev.stopPropagation();
        ev.preventDefault();
      });
      untilTrigger.addEventListener("click", function (ev) {
        ev.stopPropagation();
        if (!untilPopup.hidden) {
          untilPopup.hidden = true;
          return;
        }
        // position: fixed, placed via the trigger's own viewport
        // coordinates -- NOT position: absolute anchored to a
        // position: relative ancestor. An absolutely-positioned popup
        // is still clipped by the nearest ancestor with overflow !=
        // visible, which the dialog itself needs for its own content
        // (a tall Repeats section can already make the dialog taller
        // than the viewport) -- that clipped the popup's bottom half
        // and looked like two disconnected floating boxes, not one
        // picker. position: fixed escapes that clipping entirely (it's
        // positioned against the viewport, not any scrolling ancestor)
        // while staying a real DOM descendant of the <dialog> -- native
        // <dialog> promotes itself to the browser's top layer, and
        // z-index inside that layer only has to beat this dialog's OWN
        // other content, which a plain z-index:1 already does.
        var rect = untilTrigger.getBoundingClientRect();
        untilPopup.style.top = rect.bottom + 4 + "px";
        untilPopup.style.left = rect.left + "px";
        untilPopup.hidden = false;
      });
      var untilWrap = el("div", { class: "dtp-until-wrap" }, [untilTrigger, untilPopup]);

      var countInput = el("input", { type: "number", min: "1", class: "dtp-count-input" });
      countInput.value = recurState.count ? String(recurState.count) : "";

      var endRow = el("div", { class: "dtp-end-row" }, [
        el("label", { class: "dtp-end-option" }, [endNever, el("span", { text: " Forever" })]),
        el("label", { class: "dtp-end-option" }, [endUntil, el("span", { text: " Until " }), untilWrap]),
        el("label", { class: "dtp-end-option" }, [endCount, el("span", { text: " After " }), countInput, el("span", { text: " occurrences" })]),
      ]);
      [endNever, endUntil, endCount].forEach(function (r) {
        r.addEventListener("change", function () {
          untilTrigger.disabled = !endUntil.checked;
          untilPopup.hidden = true;
        });
      });
      untilTrigger.disabled = recurState.endMode !== "until";

      var intervalRow = el("div", { class: "dtp-interval-row" }, [
        el("span", { text: "Every " }), intervalInput, intervalUnitLabel,
      ]);
      var repeatDetail = el("div", { class: "dtp-repeat-detail" }, [intervalRow, endRow]);

      function syncRepeatVisibility() {
        var freq = freqDd.getValue();
        repeatDetail.hidden = !freq;
        if (freq) {
          var unit = FREQ_LABELS[freq];
          var n = parseInt(intervalInput.value, 10) || 1;
          intervalUnitLabel.textContent = " " + unit + (n === 1 ? "" : "s");
        }
      }
      intervalInput.addEventListener("input", syncRepeatVisibility);
      syncRepeatVisibility();

      var repeatSection = el("div", { class: "dtp-repeat" }, [
        el("label", { class: "dtp-freq-label" }, [el("span", { text: "Repeats " }), freqDd.element]),
        repeatDetail,
      ]);

      // --- Live summary preview, actions ---------------------------------
      var summaryPreview = el("p", { class: "dtp-summary-preview" });
      function currentState() {
        var due = "";
        if (selectedDate) {
          due = allDayCheckbox.checked ? selectedDate : selectedDate + "T" + hourDd.getValue() + ":" + minuteDd.getValue();
        }
        var endMode = endUntil.checked ? "until" : endCount.checked ? "count" : "never";
        var recur = buildRecur({
          freq: freqDd.getValue(),
          interval: parseInt(intervalInput.value, 10) || 1,
          endMode: endMode,
          until: untilIso || "",
          count: parseInt(countInput.value, 10) || 0,
        });
        return { due: due, recur: due ? recur : "" };
      }
      function updateSummaryPreview() {
        var s = currentState();
        summaryPreview.textContent = formatSummary(s.due, s.recur);
      }

      var clearBtn = el("button", { type: "button", class: "dtp-clear", text: "Clear due date" });
      clearBtn.addEventListener("click", function () {
        selectedDate = null;
        selectedTime = "";
        freqDd.setValue("");
        syncRepeatVisibility();
        dueCalendar.setSelected(null);
        updateSummaryPreview();
      });
      var cancelBtn = el("button", { type: "button", text: "Cancel" });
      cancelBtn.addEventListener("click", function () {
        finish(null);
      });
      var saveBtn = el("button", { type: "submit", class: "wiki-dialog-ok", text: "Save" });

      var form = el("form", { method: "dialog" }, [
        dueCalendar.element,
        timeRow,
        repeatSection,
        summaryPreview,
        el("div", { class: "wiki-dialog-actions" }, [clearBtn, cancelBtn, saveBtn]),
      ]);
      form.addEventListener("submit", function (ev) {
        ev.preventDefault();
        finish(currentState());
      });
      // Any click elsewhere in the form closes an open Until popup --
      // the standard "click outside to dismiss" a floating picker
      // needs, scoped to this dialog instance rather than a
      // document-level listener that would outlive it.
      form.addEventListener("click", function () {
        untilPopup.hidden = true;
      });
      dialog.addEventListener("cancel", function (ev) {
        ev.preventDefault();
        finish(null);
      });

      dialog.appendChild(el("h3", { id: "dtp-title", class: "dtp-title", text: "Calendar event" }));
      dialog.appendChild(form);
      document.body.appendChild(dialog);

      // One delegated listener on the form for the remaining native
      // controls (the all-day checkbox, the end-mode radios, the
      // interval/count numbers), rather than wiring updateSummaryPreview
      // onto each individually -- the per-control approach already
      // missed the three end-mode radios (Forever/Until/After) the
      // first time around, caught live: switching between them left the
      // preview line showing stale recurrence text. The three dropdowns
      // (Repeats, Hour, Minute -- WikiDropdown, not native <select>) and
      // both calendar widgets dispatch no native change/click bubble a
      // form-level listener could catch, so each calls
      // updateSummaryPreview directly from its own onChange/onSelect.
      form.addEventListener("change", updateSummaryPreview);
      form.addEventListener("input", updateSummaryPreview);
      updateSummaryPreview();
      dialog.showModal();
    });
  }

  return { open: open, formatSummary: formatSummary };
})();
