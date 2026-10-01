// Calendar page — GET /api/calendar?start=&end= (CalendarQueries.cpp),
// rendered as a month grid. Recurring events are already expanded into
// concrete per-day occurrences server-side; this file only groups what
// it's given by date and draws cells, same "server decides what's
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
  // Monday-first weeks.
  var DAY_NAMES = ["Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"];

  function pad2(n) {
    return n < 10 ? "0" + n : String(n);
  }

  function isoDate(y, mZeroBased, d) {
    return y + "-" + pad2(mZeroBased + 1) + "-" + pad2(d);
  }

  function daysInMonth(y, mZeroBased) {
    return new Date(y, mZeroBased + 1, 0).getDate();
  }

  // Monday=0 .. Sunday=6, from JS's native Sunday=0 .. Saturday=6.
  function mondayFirstWeekday(y, mZeroBased, d) {
    var native = new Date(y, mZeroBased, d).getDay();
    return (native + 6) % 7;
  }

  function todayIso() {
    var now = new Date();
    return isoDate(now.getFullYear(), now.getMonth(), now.getDate());
  }

  function renderGrid(year, monthZeroBased, eventsByDate) {
    var firstWeekday = mondayFirstWeekday(year, monthZeroBased, 1);
    var totalDays = daysInMonth(year, monthZeroBased);
    var today = todayIso();

    var html = '<div class="calendar-grid">';
    for (var i = 0; i < DAY_NAMES.length; i++) {
      html += '<div class="calendar-weekday">' + DAY_NAMES[i] + "</div>";
    }
    for (var blank = 0; blank < firstWeekday; blank++) {
      html += '<div class="calendar-cell calendar-cell-empty"></div>';
    }
    for (var day = 1; day <= totalDays; day++) {
      var date = isoDate(year, monthZeroBased, day);
      var events = eventsByDate[date] || [];
      var cellClass =
        "calendar-cell" + (date === today ? " calendar-cell-today" : "");
      html += '<div class="' + cellClass + '"><div class="calendar-day-num">' + day + "</div>";
      for (var i2 = 0; i2 < events.length; i2++) {
        var ev = events[i2];
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
      html += "</div>";
    }
    // Trailing blanks so the grid always ends on a full week row.
    var totalCells = firstWeekday + totalDays;
    var trailing = (7 - (totalCells % 7)) % 7;
    for (var t = 0; t < trailing; t++) {
      html += '<div class="calendar-cell calendar-cell-empty"></div>';
    }
    html += "</div>";
    return html;
  }

  window.WikiPages.renderCalendar = function (container, session) {
    document.getElementById("page-title").textContent = "Calendar — wiki";

    var now = new Date();
    var year = now.getFullYear();
    var month = now.getMonth(); // 0-based

    container.innerHTML =
      '<h1>Calendar</h1>' +
      '<div class="calendar-toolbar">' +
      '<button type="button" id="cal-prev" class="btn">&lsaquo; Prev</button>' +
      '<span id="cal-label" class="calendar-label"></span>' +
      '<button type="button" id="cal-next" class="btn">Next &rsaquo;</button>' +
      '<button type="button" id="cal-today" class="btn">Today</button>' +
      "</div>" +
      '<div id="cal-body"><p>Loading…</p></div>';

    var label = document.getElementById("cal-label");
    var body = document.getElementById("cal-body");

    function load() {
      label.textContent = MONTH_NAMES[month] + " " + year;
      var start = isoDate(year, month, 1);
      var end = isoDate(year, month, daysInMonth(year, month));
      fetch(
        basePath() + "/api/calendar?start=" + start + "&end=" + end,
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
          body.innerHTML = renderGrid(year, month, byDate);
        })
        .catch(function (err) {
          body.innerHTML =
            '<p class="calendar-error">Failed to load calendar: ' +
            escapeHtml(err.message) +
            "</p>";
        });
    }

    document.getElementById("cal-prev").addEventListener("click", function () {
      month -= 1;
      if (month < 0) {
        month = 11;
        year -= 1;
      }
      load();
    });
    document.getElementById("cal-next").addEventListener("click", function () {
      month += 1;
      if (month > 11) {
        month = 0;
        year += 1;
      }
      load();
    });
    document.getElementById("cal-today").addEventListener("click", function () {
      var n = new Date();
      year = n.getFullYear();
      month = n.getMonth();
      load();
    });

    load();
  };
})();
