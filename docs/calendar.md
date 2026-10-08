# Calendar

Any document can carry a `due` date and, optionally, a recurrence rule. The
`/calendar` page shows every one you're allowed to see, laid out as a month
grid. There's no separate "event" document type — a calendar entry is just
a document with `due` set, the same way a bookmark is just a document with
`type: bookmark`.

## Setting a due date

The edit form has three extra fields, right under Title/Tags:

- **Due** — a plain date picker. Clearing it removes the document from the
  calendar entirely.
- **Time** — optional. Leave it blank for an all-day event, or set it for a
  specific time (e.g. "16:00 Meeting with Google" rather than just
  "sometime on the 5th"). This is stored as part of `due` itself
  (`due: 2026-10-05T16:00`), not as a separate field. A bare
  `due: 2026-10-05` with no `T` is the all-day case — the same shape every
  document saved before this field existed already has.
- **Repeats** — a dropdown with the four common frequencies, plus
  "Custom…" for the full syntax below when the dropdown isn't enough. It
  has no effect without a due date set; a repeat rule with no due date is
  just inert, not an error.

All of this round-trips through front matter as plain text
(`due: 2026-10-05T16:00`, `recur: weekly`), so hand-editing the file
directly works exactly the same as using the form.

## Recurrence rules

A recurrence rule is one bare frequency word, optionally followed by
`;`-separated `key=value` modifiers:

```
daily | weekly | monthly | yearly
;interval=N       -- every N periods instead of every 1 (default 1)
;until=YYYY-MM-DD -- stop generating occurrences after this date
;count=N          -- stop after N occurrences
```

`until` and `count` can both be given at once — they're two independent
bounds, and whichever is hit first stops the series. Examples:

| Rule | Meaning |
| --- | --- |
| `weekly` | Every week, forever |
| `monthly;interval=2` | Every other month |
| `weekly;until=2026-12-31` | Every week through the end of 2026 |
| `yearly;count=5` | The next 5 anniversaries, then stop |

Two edge cases:

- A monthly or yearly rule anchored on a day that doesn't exist in a later
  month (`due: 2026-01-31`, `recur: monthly`) clamps to that month's last
  day — Jan 31 → Feb 28 → Mar 31 → Apr 30 — the same way most calendar
  apps handle it. It's not a skipped occurrence.
- A `recur` value that fails to parse (a typo) doesn't drop the event from
  the calendar. It degrades to treating the document as a one-off on its
  own `due` date instead.

## Day view

`/calendar`'s Day view is an hour-by-hour planner (00:00–23:00), not a flat
list. A timed event (`due` with a `T...` time) lands in its own hour's
row; an all-day event (no time) gets its own section above the grid,
instead of being placed in a made-up "00:00" slot. Month and Week views
show the same events as compact chips — time-prefixed when there is one
(`16:00 Meeting with Google`), sorted all-day events first, then by time.

## Multiple calendars

There's no separate "calendar" entity to create. "Multiple calendars" is
just the same tag/folder filtering every other list in this app already
uses: keep work and personal events apart with `tag: work`/`tag: personal`,
or by folder (`calendars/work/`, `calendars/personal/`), and scope a view
to just one with `folder=`/`tag=` on `GET /api/calendar`. The month-grid
page itself currently shows everything visible to the viewer — a
folder/tag picker on that page would be a reasonable follow-up, but it
isn't built yet.

## Visibility

The calendar is admin-only, full stop — it does *not* follow the
fail-safe-private-per-document rule the rest of the app uses (search, nav,
query blocks), where a public document stays visible to anonymous callers
and only private ones are held back. The reasoning: the calendar is the
admin's actual schedule, and even a *public* document's due date going
through `/api/calendar` would reveal when things on it happen — not just
that the document itself is public.

In practice: `GET /api/calendar` returns a plain `401` to an
unauthenticated caller, never a filtered or partial event list. The
`/calendar` page itself redirects an anonymous visitor to `/login` rather
than rendering an empty toolbar. The sidebar's Calendar icon stays hidden
until `GET /api/session` confirms an authenticated caller — the same rule
the Account link follows.

## `GET /api/calendar`

```
GET /api/calendar?start=2026-03-01&end=2026-03-31
GET /api/calendar?start=2026-03-01&end=2026-03-31&folder=work/&tag=standup
```

`start`/`end` (both required, inclusive, `YYYY-MM-DD`) bound the window. A
recurring series anchored before `start` still expands correctly into that
window — the expansion isn't limited to occurrences that happen to be
generated after the document's own `due` date falls in range.

The response shape is
`{"events": [{"path", "title", "visibility", "date", "time"}, ...]}`, one
entry per *occurrence* — a weekly event spanning the window appears once
per week, not once per document. `time` is `"HH:MM"` for a timed event and
`""` for an all-day one; on a recurring series, the clock time stays the
same across every occurrence and only `date` moves.

## MCP

`due` and `recur` are plain optional string arguments on the existing
`create_document`/`update_document` tools (both stdio and remote HTTP) —
no new tool was needed just for writing. The usual
`update_document` partial-update rule applies: omit a field to keep its
current value, or pass `""` to clear it.

`get_calendar_events(start, end, folder?, tags?)` is a dedicated read
tool, available over stdio, remote HTTP, and the in-app Draft/Chat agent —
all three go through the same `CalendarQueries::eventsBetween` engine that
`/calendar` and `GET /api/calendar` use. That means an agent asked "what's
due today" can call this directly, instead of reading documents one at a
time and parsing front matter itself. Recurring series come back already
expanded into concrete per-day occurrences, exactly like the HTTP API. See
`docs/mcp.md`'s tool list for the full parameter shape.
