# Calendar

Any document can carry a `due` date and, optionally, a recurrence rule — the
`/calendar` page shows every one you can see, laid out as a month grid.
There's no separate "event" document type: a calendar entry is just a
document with `due` set, same as a bookmark is just a document with
`type: bookmark`.

## Setting a due date

The edit form has three extra fields, right under Title/Tags:

- **Due** — a plain date picker. Clear it to remove the document from the
  calendar entirely.
- **Time** — optional. Leave it blank for an all-day event; set it for a
  specific time (`16:00 Meeting with Google`, not just "sometime on the
  5th"). Front matter stores this as part of `due` itself
  (`due: 2026-10-05T16:00`), not a separate field — a bare `due:
  2026-10-05` with no `T` is the all-day case, same as every document
  saved before this field existed.
- **Repeats** — a dropdown for the four common frequencies, plus
  "Custom…" for the full `;interval=`/`;until=`/`;count=` syntax (see
  below) when the dropdown alone isn't enough. Meaningless without a due
  date set; a repeat rule with no due date is just inert, not an error.

All of this round-trips through front matter as plain text (`due:
2026-10-05T16:00`, `recur: weekly`) — hand-editing the file directly works
the same as using the form.

## Recurrence rules

One bare frequency word, then optional `;`-separated `key=value` modifiers:

```
daily | weekly | monthly | yearly
;interval=N       -- every N periods instead of every 1 (default 1)
;until=YYYY-MM-DD -- stop generating occurrences after this date
;count=N          -- stop after N occurrences
```

`until` and `count` can both be given — whichever is hit first stops the
series; this isn't a conflict, just two independent bounds. Examples:

| Rule | Meaning |
| --- | --- |
| `weekly` | Every week, forever |
| `monthly;interval=2` | Every other month |
| `weekly;until=2026-12-31` | Every week through the end of 2026 |
| `yearly;count=5` | The next 5 anniversaries, then stop |

A monthly/yearly rule anchored on a day that doesn't exist in a later month
(`due: 2026-01-31`, `recur: monthly`) clamps to that month's last day —
Jan 31 → Feb 28 → Mar 31 → Apr 30 — the same way every calendar app
handles it, not a skipped occurrence. A `recur` value that doesn't parse
(a typo) degrades to treating the document as a one-off on its own `due`
date, rather than losing the event from the calendar entirely.

## Day view

`/calendar`'s Day view is an hour-by-hour planner (00:00–23:00), not a
flat list — a timed event (`due` with a `T...` time) lands in its own
hour's row; an all-day event (no time) gets its own section above the
grid instead of a made-up "00:00" slot. Month and Week views show the
same events as compact chips, time-prefixed when there is one
(`16:00 Meeting with Google`), sorted all-day-first then by time.

## Multiple calendars

There's no separate "calendar" entity to create — "multiple calendars" is
the same tag/folder filtering every other list in this app already uses.
Keep work and personal events apart with `tag: work`/`tag: personal`, or by
folder (`calendars/work/`, `calendars/personal/`), and scope a view to just
one with `folder=`/`tag=` on `GET /api/calendar`. The month-grid page itself
shows everything visible to the current viewer; a folder/tag picker on that
page is a possible follow-up, not built yet.

## Visibility

Fail-safe-private, same as everything else: a private document's due dates
(recurring or not) never appear in `/api/calendar` for an anonymous caller,
even if the request's date range would otherwise include them.

## `GET /api/calendar`

```
GET /api/calendar?start=2026-03-01&end=2026-03-31
GET /api/calendar?start=2026-03-01&end=2026-03-31&folder=work/&tag=standup
```

`start`/`end` (both required, inclusive, `YYYY-MM-DD`) bound the window;
recurring series anchored before `start` still expand into it correctly —
the expansion isn't limited to occurrences generated after the document's
own `due` date happens to fall in range. Returns
`{"events": [{"path", "title", "visibility", "date", "time"}, ...]}`, one
entry per occurrence (a weekly event spanning the window appears once per
week, not once per document). `time` is `"HH:MM"` for a timed event, `""`
for an all-day one — on a recurring series it's the same clock time on
every occurrence, only `date` moves.

## MCP

`due`/`recur` are plain optional string arguments on the existing
`create_document`/`update_document` tools (stdio and remote HTTP both) —
no new tool was needed for writing. `update_document`'s usual partial-update
rule applies: omit a field to keep its current value, or pass `""` to clear
it.

`get_calendar_events(start, end, folder?, tags?)` is a dedicated read tool
(stdio, remote HTTP, and the in-app Draft/Chat agent, all three — same
`CalendarQueries::eventsBetween` engine as `/calendar` and
`GET /api/calendar`), so an agent asked "what's due today" calls this
directly instead of reading documents one at a time and parsing front
matter itself. Recurring series come back already expanded into concrete
per-day occurrences, exactly like the HTTP API. See `docs/mcp.md`'s tool
list for the full parameter shape.
