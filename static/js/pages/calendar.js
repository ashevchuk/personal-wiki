// Calendar page — GET /api/calendar?start=&end= (CalendarQueries.cpp),
// rendered as month/week/day views. Recurring events are already expanded
// into concrete per-day occurrences server-side; this file only groups
// what it's given by date and draws cells, same "server decides what's
// visible, client just renders it" split as query-block.js.
window.WikiPages = window.WikiPages || {};

(function () {
  "use strict";

  var basePath = WikiCommon.basePath;
  var escapeHtml = WikiCommon.escapeHtml;
  var encodeVaultPath = WikiCommon.encodeVaultPath;

  var MONTH_NAMES = [
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December",
  ];
  // Monday-first weeks throughout this file.
  var DAY_NAMES = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];
  var DAY_NAMES_LONG = [
    "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday", "Sunday",
  ];

  function pad2(n) {
    return n < 10 ? "0" + n : String(n);
  }

  function isoDate(y, mZeroBased, d) {
    return y + "-" + pad2(mZeroBased + 1) + "-" + pad2(d);
  }

  function dateFromIso(iso) {
    var parts = iso.split("-");
    return new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2]));
  }

  function isoFromDate(d) {
    return isoDate(d.getFullYear(), d.getMonth(), d.getDate());
  }

  function addDays(d, n) {
    var copy = new Date(d.getFullYear(), d.getMonth(), d.getDate());
    copy.setDate(copy.getDate() + n);
    return copy;
  }

  function daysInMonth(y, mZeroBased) {
    return new Date(y, mZeroBased + 1, 0).getDate();
  }

  // Monday=0 .. Sunday=6, from JS's native Sunday=0 .. Saturday=6.
  function mondayFirstWeekday(d) {
    return (d.getDay() + 6) % 7;
  }

  // The Monday on or before `d`.
  function startOfWeek(d) {
    return addDays(d, -mondayFirstWeekday(d));
  }

  function todayIso() {
    return isoFromDate(new Date());
  }

  // All-day events (time === "") sort first -- an empty string compares
  // less than any "HH:MM" one -- then ascending by time.
  function eventsFor(eventsByDate, iso) {
    var events = (eventsByDate[iso] || []).slice();
    events.sort(function (a, b) {
      return (a.time || "").localeCompare(b.time || "");
    });
    return events;
  }

  function eventLabel(ev) {
    return ev.time ? ev.time + " " + ev.title : ev.title;
  }

  function renderEventLinks(events) {
    var html = "";
    for (var i = 0; i < events.length; i++) {
      var ev = events[i];
      var label = eventLabel(ev);
      html +=
        '<a class="calendar-event" href="' +
        basePath() +
        "/d/" +
        encodeVaultPath(ev.path) +
        '" title="' +
        escapeHtml(label) +
        '">' +
        escapeHtml(label) +
        "</a>";
    }
    return html;
  }

  // Always 6 full weeks (42 cells) -- a 4-week February next to a
  // 6-week October otherwise resizes this whole page by two row-heights
  // on every Prev/Next click (found live, the same fixed-height fix
  // already applied to the Due-date picker's own calendar widget in
  // date-time-picker.js). Leading/trailing filler cells show the
  // adjacent month's real day numbers, dimmed -- purely visual, never
  // a link -- rather than blank boxes, matching that same widget.
  function renderMonthGrid(anchor, eventsByDate) {
    var year = anchor.getFullYear();
    var month = anchor.getMonth();
    var firstWeekday = mondayFirstWeekday(new Date(year, month, 1));
    var totalDays = daysInMonth(year, month);
    var today = todayIso();

    var html = '<div class="calendar-grid">';
    for (var i = 0; i < DAY_NAMES.length; i++) {
      html += '<div class="calendar-weekday">' + DAY_NAMES[i] + "</div>";
    }
    var prevMonthDays = daysInMonth(year, month - 1);
    for (var lead = firstWeekday; lead > 0; lead--) {
      html +=
        '<div class="calendar-cell calendar-cell-pad">' +
        (prevMonthDays - lead + 1) +
        "</div>";
    }
    for (var day = 1; day <= totalDays; day++) {
      var date = isoDate(year, month, day);
      var events = eventsFor(eventsByDate, date);
      var cellClass = "calendar-cell" + (date === today ? " calendar-cell-today" : "");
      html +=
        '<div class="' +
        cellClass +
        '"><div class="calendar-day-num">' +
        day +
        "</div>" +
        renderEventLinks(events) +
        "</div>";
    }
    var trailing = 42 - (firstWeekday + totalDays);
    for (var t = 1; t <= trailing; t++) {
      html += '<div class="calendar-cell calendar-cell-pad">' + t + "</div>";
    }
    html += "</div>";
    return html;
  }

  function renderWeekGrid(anchor, eventsByDate) {
    var monday = startOfWeek(anchor);
    var today = todayIso();

    var html = '<div class="calendar-grid calendar-grid-week">';
    for (var i = 0; i < DAY_NAMES.length; i++) {
      html += '<div class="calendar-weekday">' + DAY_NAMES[i] + "</div>";
    }
    for (var d = 0; d < 7; d++) {
      var date = isoFromDate(addDays(monday, d));
      var events = eventsFor(eventsByDate, date);
      var cellClass = "calendar-cell" + (date === today ? " calendar-cell-today" : "");
      html +=
        '<div class="' +
        cellClass +
        '"><div class="calendar-day-num">' +
        Number(date.slice(8, 10)) +
        "</div>" +
        renderEventLinks(events) +
        "</div>";
    }
    html += "</div>";
    return html;
  }

  // An hour-by-hour day planner (00:00-23:00), the way any real calendar
  // app's day view works -- a flat list loses exactly the thing a day
  // view is for: seeing at a glance where the gaps are. All-day events
  // (no due TIME, just a date) get their own section above the grid
  // instead of a fake "00:00" slot -- they aren't scheduled at any
  // particular hour, placing them in the grid would be a made-up fact.
  function renderDayView(anchor, eventsByDate) {
    var date = isoFromDate(anchor);
    var events = eventsFor(eventsByDate, date);
    var heading = DAY_NAMES_LONG[mondayFirstWeekday(anchor)] + ", " + date;

    var allDay = [];
    var byHour = {};
    for (var i = 0; i < events.length; i++) {
      var ev = events[i];
      if (!ev.time) {
        allDay.push(ev);
        continue;
      }
      var hour = Number(ev.time.slice(0, 2));
      if (!byHour[hour]) byHour[hour] = [];
      byHour[hour].push(ev);
    }

    var html = '<div class="calendar-day-view">';
    html += '<div class="calendar-day-view-heading">' + escapeHtml(heading) + "</div>";
    if (allDay.length > 0) {
      html +=
        '<div class="calendar-allday"><div class="calendar-allday-label">All day</div>' +
        renderEventLinks(allDay) +
        "</div>";
    }
    html += '<div class="calendar-hour-grid">';
    for (var h = 0; h < 24; h++) {
      html +=
        '<div class="calendar-hour-row">' +
        '<div class="calendar-hour-label">' +
        pad2(h) +
        ":00</div>" +
        '<div class="calendar-hour-events">' +
        renderEventLinks(byHour[h] || []) +
        "</div>" +
        "</div>";
    }
    html += "</div></div>";
    return html;
  }

  // Returns { start, end (ISO, inclusive), label } for the current
  // view mode/anchor -- the only three things load() needs to fetch and
  // caption the right range.
  function computeRange(viewMode, anchor) {
    if (viewMode === "day") {
      var iso = isoFromDate(anchor);
      return { start: iso, end: iso, label: DAY_NAMES_LONG[mondayFirstWeekday(anchor)] + ", " + iso };
    }
    if (viewMode === "week") {
      var monday = startOfWeek(anchor);
      var sunday = addDays(monday, 6);
      return {
        start: isoFromDate(monday),
        end: isoFromDate(sunday),
        label: isoFromDate(monday) + " – " + isoFromDate(sunday),
      };
    }
    // month
    var year = anchor.getFullYear();
    var month = anchor.getMonth();
    return {
      start: isoDate(year, month, 1),
      end: isoDate(year, month, daysInMonth(year, month)),
      label: MONTH_NAMES[month] + " " + year,
    };
  }

  window.WikiPages.renderCalendar = function (container, session) {
    document.getElementById("page-title").textContent = "Calendar — wiki";

    // Same gate every admin-only page module uses (see account.js) — a
    // direct hit on /calendar while logged out bounces to /login rather
    // than rendering a toolbar whose first fetch would just 401.
    // GET /api/calendar itself also hard-gates this server-side
    // (CalendarRoutes.cpp) — this is belt-and-suspenders for a page
    // that would otherwise flash empty before that fetch even returns.
    if (!session.authenticated) {
      window.location.href = basePath() + "/login";
      return;
    }

    var anchor = new Date();
    var viewMode = "month"; // "month" | "week" | "day"

    container.innerHTML =
      "<h1>Calendar</h1>" +
      '<div class="calendar-toolbar">' +
      '<button type="button" id="cal-prev" class="btn">&lsaquo; Prev</button>' +
      '<span id="cal-label" class="calendar-label"></span>' +
      '<button type="button" id="cal-next" class="btn">Next &rsaquo;</button>' +
      '<button type="button" id="cal-today" class="btn">Today</button>' +
      '<span class="calendar-view-toggle">' +
      '<button type="button" id="cal-view-month" class="btn">Month</button>' +
      '<button type="button" id="cal-view-week" class="btn">Week</button>' +
      '<button type="button" id="cal-view-day" class="btn">Day</button>' +
      "</span>" +
      "</div>" +
      '<div id="cal-body"><p>Loading…</p></div>';

    var label = document.getElementById("cal-label");
    var body = document.getElementById("cal-body");

    // Month/Year dropdowns instead of a plain text label -- only in
    // Month view, where "jump to a specific month" is one change event
    // instead of many Prev/Next clicks; Week/Day keep the plain label
    // (their own range text, e.g. a date span or a weekday+date, isn't
    // a single month to pick from a dropdown). Year range is just
    // "today ± 10" plus the current anchor's own year if it's ever
    // navigated outside that window (Today always re-centers it).
    function renderMonthLabel() {
      var curYear = anchor.getFullYear();
      var curMonth = anchor.getMonth();
      var monthOptions = MONTH_NAMES.map(function (name, m) {
        return { value: m, label: name };
      });
      var todayYear = new Date().getFullYear();
      var minYear = Math.min(todayYear - 10, curYear);
      var maxYear = Math.max(todayYear + 10, curYear);
      var yearOptions = [];
      for (var y = minYear; y <= maxYear; y++) {
        yearOptions.push({ value: y, label: String(y) });
      }
      function jump(nextMonth, nextYear) {
        anchor = new Date(nextYear, nextMonth, 1);
        load();
      }
      var monthDd = WikiDropdown.create({
        options: monthOptions,
        value: curMonth,
        ariaLabel: "Month",
        triggerClass: "calendar-label-select",
        onChange: function (v) {
          jump(Number(v), Number(yearDd.getValue()));
        },
      });
      var yearDd = WikiDropdown.create({
        options: yearOptions,
        value: curYear,
        ariaLabel: "Year",
        triggerClass: "calendar-label-select",
        onChange: function (v) {
          jump(Number(monthDd.getValue()), Number(v));
        },
      });
      label.innerHTML = "";
      label.appendChild(monthDd.element);
      label.appendChild(yearDd.element);
    }
    var viewButtons = {
      month: document.getElementById("cal-view-month"),
      week: document.getElementById("cal-view-week"),
      day: document.getElementById("cal-view-day"),
    };

    function updateViewButtons() {
      Object.keys(viewButtons).forEach(function (mode) {
        viewButtons[mode].setAttribute("aria-pressed", mode === viewMode ? "true" : "false");
      });
    }

    function load() {
      updateViewButtons();
      var range = computeRange(viewMode, anchor);
      if (viewMode === "month") {
        renderMonthLabel();
      } else {
        label.textContent = range.label;
      }
      fetch(
        basePath() + "/api/calendar?start=" + range.start + "&end=" + range.end,
        { credentials: "same-origin" }
      )
        .then(function (resp) {
          if (!resp.ok) throw new Error("HTTP " + resp.status);
          return resp.json();
        })
        .then(function (data) {
          var byDate = {};
          var events = data.events || [];
          for (var i = 0; i < events.length; i++) {
            var ev = events[i];
            if (!byDate[ev.date]) byDate[ev.date] = [];
            byDate[ev.date].push(ev);
          }
          if (viewMode === "day") {
            body.innerHTML = renderDayView(anchor, byDate);
          } else if (viewMode === "week") {
            body.innerHTML = renderWeekGrid(anchor, byDate);
          } else {
            body.innerHTML = renderMonthGrid(anchor, byDate);
          }
        })
        .catch(function (err) {
          body.innerHTML =
            '<p class="calendar-error">Failed to load calendar: ' +
            escapeHtml(err.message) +
            "</p>";
        });
    }

    function step(direction) {
      if (viewMode === "day") {
        anchor = addDays(anchor, direction);
      } else if (viewMode === "week") {
        anchor = addDays(anchor, direction * 7);
      } else {
        anchor = new Date(anchor.getFullYear(), anchor.getMonth() + direction, 1);
      }
      load();
    }

    document.getElementById("cal-prev").addEventListener("click", function () {
      step(-1);
    });
    document.getElementById("cal-next").addEventListener("click", function () {
      step(1);
    });
    document.getElementById("cal-today").addEventListener("click", function () {
      anchor = new Date();
      load();
    });
    Object.keys(viewButtons).forEach(function (mode) {
      viewButtons[mode].addEventListener("click", function () {
        viewMode = mode;
        load();
      });
    });

    load();
  };
})();
