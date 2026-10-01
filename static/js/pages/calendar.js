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

  function eventsFor(eventsByDate, iso) {
    return eventsByDate[iso] || [];
  }

  function renderEventLinks(events) {
    var html = "";
    for (var i = 0; i < events.length; i++) {
      var ev = events[i];
      html +=
        '<a class="calendar-event" href="' +
        basePath() +
        "/d/" +
        encodeVaultPath(ev.path) +
        '" title="' +
        escapeHtml(ev.title) +
        '">' +
        escapeHtml(ev.title) +
        "</a>";
    }
    return html;
  }

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
    for (var blank = 0; blank < firstWeekday; blank++) {
      html += '<div class="calendar-cell calendar-cell-empty"></div>';
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
    var totalCells = firstWeekday + totalDays;
    var trailing = (7 - (totalCells % 7)) % 7;
    for (var t = 0; t < trailing; t++) {
      html += '<div class="calendar-cell calendar-cell-empty"></div>';
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

  function renderDayList(anchor, eventsByDate) {
    var date = isoFromDate(anchor);
    var events = eventsFor(eventsByDate, date);
    var heading = DAY_NAMES_LONG[mondayFirstWeekday(anchor)] + ", " + date;

    var html = '<div class="calendar-day-view">';
    html += '<div class="calendar-day-view-heading">' + escapeHtml(heading) + "</div>";
    if (events.length === 0) {
      html += '<p class="query-empty">Nothing scheduled.</p>';
    } else {
      html += '<ul class="calendar-day-view-list">';
      for (var i = 0; i < events.length; i++) {
        html += "<li>" + renderEventLinks([events[i]]) + "</li>";
      }
      html += "</ul>";
    }
    html += "</div>";
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
      label.textContent = range.label;
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
            body.innerHTML = renderDayList(anchor, byDate);
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
