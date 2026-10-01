# Calendar

Any document can carry a `due` date and, optionally, a recurrence rule — the
`/calendar` page shows every one you can see, laid out as a month grid.
There's no separate "event" document type: a calendar entry is just a
document with `due` set, same as a bookmark is just a document with
`type: bookmark`.

## Setting a due date

The edit form has two extra fields, right under Title/Tags:

- **Due** — a plain date picker. Clear it to remove the document from the
  calendar entirely.
- **Repeats** — the recurrence rule (see below). Meaningless without a due
  date set; a repeat rule with no due date is just inert, not an error.

Both round-trip through front matter as plain text (`due: 2026-03-02`,
`recur: weekly`) — hand-editing the file directly works the same as using
the form.

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
`{"events": [{"path", "title", "visibility", "date"}, ...]}`, one entry per
occurrence (a weekly event spanning the window appears once per week, not
once per document).

## MCP

`due`/`recur` are plain optional string arguments on the existing
`create_document`/`update_document` tools (stdio and remote HTTP both) —
no new tool was needed. `update_document`'s usual partial-update rule
applies: omit a field to keep its current value, or pass `""` to clear it.
There's no calendar-specific MCP tool yet; an agent that wants a date range
of events today has to read documents directly rather than calling
`/api/calendar`.
