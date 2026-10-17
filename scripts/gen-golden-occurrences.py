#!/usr/bin/env python3
"""Writes tests/fixtures/ical/golden_occurrences.txt: iCalendar events with the occurrences an independent implementation
(python-dateutil's rrule plus zoneinfo, the same IANA release as third_party/tzdata) expects. The test
tests/unit/test_occurrences_golden.cpp feeds every case to parse_ical + expand and compares. Needs python3-dateutil.
The cases use reserved example domains only. Run: scripts/gen-golden-occurrences.py"""
import datetime as dt
import os
import sys
from zoneinfo import ZoneInfo

from dateutil import rrule as rr

UTC = dt.timezone.utc
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests/fixtures/ical/golden_occurrences.txt")

cases = []


def fmt_local(d):
    return d.strftime("%Y%m%dT%H%M%S")


class Case:
    def __init__(self, name, kind, zone, start, end, rrule="", user="Europe/Berlin", exdates=(), rdates=(), overrides=(),
                 window=("2026-01-01", "2028-01-01"), exdate_zone=None):
        self.name, self.kind, self.zone, self.start, self.end = name, kind, zone, start, end
        self.rrule, self.user, self.exdates, self.rdates, self.overrides = rrule, user, list(exdates), list(rdates), list(overrides)
        self.window = window


def prop(name, value, params=""):
    return "%s%s:%s" % (name, params, value)


def ics_for(c):
    lines = ["BEGIN:VCALENDAR", "VERSION:2.0", "PRODID:-//example.org//golden//EN", "BEGIN:VEVENT", "UID:%s@example.org" % c.name,
             "DTSTAMP:20260101T000000Z", "SUMMARY:%s" % c.name]
    def dtprop(name, v):
        if c.kind == "tz":
            return prop(name, fmt_local(v), ";TZID=%s" % c.zone)
        if c.kind == "utc":
            return prop(name, fmt_local(v) + "Z")
        if c.kind == "floating":
            return prop(name, fmt_local(v))
        return prop(name, v.strftime("%Y%m%d"), ";VALUE=DATE")
    lines.append(dtprop("DTSTART", c.start))
    lines.append(dtprop("DTEND", c.end))
    if c.rrule:
        lines.append("RRULE:" + c.rrule)
    for e in c.exdates:
        lines.append(dtprop("EXDATE", e))
    for r in c.rdates:
        lines.append(dtprop("RDATE", r))
    lines.append("END:VEVENT")
    for (rid, new_start, new_end, cancelled) in c.overrides:
        lines += ["BEGIN:VEVENT", "UID:%s@example.org" % c.name, "DTSTAMP:20260101T000000Z", "SUMMARY:%s moved" % c.name]
        lines.append(dtprop("RECURRENCE-ID", rid))
        if cancelled:
            lines.append("STATUS:CANCELLED")
        lines.append(dtprop("DTSTART", new_start))
        lines.append(dtprop("DTEND", new_end))
        lines.append("END:VEVENT")
    lines.append("END:VCALENDAR")
    return lines


def to_utc(c, naive):
    if c.kind == "tz":
        return naive.replace(tzinfo=ZoneInfo(c.zone), fold=0).astimezone(UTC)
    if c.kind == "utc":
        return naive.replace(tzinfo=UTC)
    return naive.replace(tzinfo=ZoneInfo(c.user), fold=0).astimezone(UTC)   # floating: the wall clock of the user's zone


def starts_of(c):
    """naive wall-clock starts of the series, in the event's own zone"""
    if not c.rrule:
        base = [c.start]
    else:
        rule = c.rrule
        parts = dict(p.split("=", 1) for p in rule.split(";"))
        if "UNTIL" in parts and parts["UNTIL"].endswith("Z") and c.kind in ("tz",):
            u = dt.datetime.strptime(parts["UNTIL"], "%Y%m%dT%H%M%SZ").replace(tzinfo=UTC).astimezone(ZoneInfo(c.zone)).replace(tzinfo=None)
            parts["UNTIL"] = u.strftime("%Y%m%dT%H%M%S")
        elif "UNTIL" in parts and parts["UNTIL"].endswith("Z"):
            parts["UNTIL"] = parts["UNTIL"][:-1]
        elif "UNTIL" in parts and len(parts["UNTIL"]) == 8:
            parts["UNTIL"] = parts["UNTIL"] + "T235959"
        rule = ";".join("%s=%s" % kv for kv in parts.items())
        base = list(rr.rrulestr("RRULE:" + rule, dtstart=c.start))
    key = lambda d: d.date() if c.kind == "date" else d
    ex = {key(e) for e in c.exdates}
    base = [b for b in base if key(b) not in ex]
    base += [r for r in c.rdates if r not in base]
    cancelled_or_moved = {key(o[0]) for o in c.overrides}
    base = [b for b in base if key(b) not in cancelled_or_moved]
    return sorted(base)


def expected(c):
    w_from = dt.datetime.strptime(c.window[0], "%Y-%m-%d").replace(tzinfo=UTC)
    w_to = dt.datetime.strptime(c.window[1], "%Y-%m-%d").replace(tzinfo=UTC)
    user = ZoneInfo(c.user)
    out = []
    dur_days = (c.end.date() - c.start.date()).days if c.kind == "date" else None
    dur = None if c.kind == "date" else (to_utc(c, c.end) - to_utc(c, c.start))
    def add(start_naive, end_naive):
        if c.kind == "date":
            s = start_naive.replace(tzinfo=user, hour=0, minute=0)
            e = end_naive.replace(tzinfo=user, hour=0, minute=0)
            if s.astimezone(UTC) < w_to and e.astimezone(UTC) > w_from:
                out.append("D%s|%s" % (start_naive.strftime("%Y-%m-%d"), end_naive.strftime("%Y-%m-%d")))
            return
        s = to_utc(c, start_naive)
        e = to_utc(c, end_naive) if end_naive is not None else s
        if s < w_to and (e > w_from or (e == s and s >= w_from)):
            out.append("%s|%s" % (s.astimezone(user).strftime("%Y-%m-%dT%H:%M"), e.astimezone(user).strftime("%Y-%m-%dT%H:%M")))
    for s in starts_of(c):
        if c.kind == "date":
            add(s, s + dt.timedelta(days=dur_days))
        else:
            su = to_utc(c, s)
            eu = su + dur
            if su < w_to and (eu > w_from or (eu == su and su >= w_from)):
                out.append("%s|%s" % (su.astimezone(user).strftime("%Y-%m-%dT%H:%M"), eu.astimezone(user).strftime("%Y-%m-%dT%H:%M")))
    for (rid, ns, ne, cancelled) in c.overrides:
        if cancelled:
            continue
        if c.kind == "date":
            add(ns, ne)
        else:
            su, eu = to_utc(c, ns), to_utc(c, ne)
            if su < w_to and (eu > w_from or (eu == su and su >= w_from)):
                out.append("%s|%s" % (su.astimezone(user).strftime("%Y-%m-%dT%H:%M"), eu.astimezone(user).strftime("%Y-%m-%dT%H:%M")))
    return sorted(out)


D = lambda *a: dt.datetime(*a)

cases += [
    Case("daily-count", "tz", "Europe/Berlin", D(2026, 5, 4, 10), D(2026, 5, 4, 11), "FREQ=DAILY;COUNT=5"),
    Case("daily-interval-until", "tz", "Europe/Berlin", D(2026, 5, 1, 9), D(2026, 5, 1, 9, 30), "FREQ=DAILY;INTERVAL=3;UNTIL=20260520"),
    Case("weekly-default-day", "tz", "Europe/Berlin", D(2026, 6, 3, 14), D(2026, 6, 3, 15), "FREQ=WEEKLY;COUNT=6"),
    Case("weekly-mo-th-until", "tz", "Europe/Berlin", D(2026, 6, 1, 8, 30), D(2026, 6, 1, 9, 30), "FREQ=WEEKLY;BYDAY=MO,TH;UNTIL=20260630T235959Z"),
    Case("weekly-interval2-wkst-su", "tz", "Europe/Berlin", D(2026, 6, 7, 18), D(2026, 6, 7, 19), "FREQ=WEEKLY;INTERVAL=2;BYDAY=SU,MO,WE;WKST=SU;COUNT=9"),
    Case("weekly-interval2-wkst-mo", "tz", "Europe/Berlin", D(2026, 6, 7, 18), D(2026, 6, 7, 19), "FREQ=WEEKLY;INTERVAL=2;BYDAY=SU,MO,WE;WKST=MO;COUNT=9"),
    Case("monthly-31st", "tz", "Europe/Berlin", D(2026, 1, 31, 12), D(2026, 1, 31, 13), "FREQ=MONTHLY;COUNT=7"),
    Case("monthly-last-day", "tz", "Europe/Berlin", D(2026, 1, 31, 12), D(2026, 1, 31, 13), "FREQ=MONTHLY;BYMONTHDAY=-1;COUNT=8"),
    Case("monthly-second-tuesday", "tz", "Europe/Berlin", D(2026, 1, 13, 19), D(2026, 1, 13, 21), "FREQ=MONTHLY;BYDAY=2TU;COUNT=12"),
    Case("monthly-last-friday", "tz", "Europe/Berlin", D(2026, 1, 30, 16), D(2026, 1, 30, 17), "FREQ=MONTHLY;BYDAY=-1FR;COUNT=12"),
    Case("monthly-last-weekday-setpos", "tz", "Europe/Berlin", D(2026, 1, 30, 9), D(2026, 1, 30, 10), "FREQ=MONTHLY;BYDAY=MO,TU,WE,TH,FR;BYSETPOS=-1;COUNT=6"),
    Case("monthly-1st-15th-interval2", "tz", "Europe/Berlin", D(2026, 1, 1, 7), D(2026, 1, 1, 8), "FREQ=MONTHLY;INTERVAL=2;BYMONTHDAY=1,15;COUNT=10"),
    Case("monthly-every-monday-of-month", "tz", "Europe/Berlin", D(2026, 3, 2, 7), D(2026, 3, 2, 8), "FREQ=MONTHLY;BYDAY=MO;COUNT=9"),
    Case("yearly-leap-day", "tz", "Europe/Berlin", D(2024, 2, 29, 12), D(2024, 2, 29, 13), "FREQ=YEARLY;COUNT=4", window=("2024-01-01", "2040-01-01")),
    Case("yearly-last-sunday-march", "tz", "Europe/Berlin", D(2026, 3, 29, 3), D(2026, 3, 29, 4), "FREQ=YEARLY;BYMONTH=3;BYDAY=-1SU;COUNT=4"),
    Case("yearly-4th-thursday-november", "tz", "America/New_York", D(2026, 11, 26, 12), D(2026, 11, 26, 13), "FREQ=YEARLY;BYMONTH=11;BYDAY=4TH;COUNT=3", user="America/New_York"),
    Case("yearly-two-months-day10", "tz", "Europe/Berlin", D(2026, 1, 10, 12), D(2026, 1, 10, 13), "FREQ=YEARLY;BYMONTH=1,7;BYMONTHDAY=10;COUNT=5"),
    Case("yearly-20th-monday-of-year", "tz", "Europe/Berlin", D(2026, 5, 18, 12), D(2026, 5, 18, 13), "FREQ=YEARLY;BYDAY=20MO;COUNT=3"),
    Case("dst-march-same-local-time", "tz", "Europe/Berlin", D(2026, 3, 16, 10), D(2026, 3, 16, 11), "FREQ=WEEKLY;COUNT=5"),
    Case("dst-march-seen-from-new-york", "tz", "Europe/Berlin", D(2026, 3, 16, 10), D(2026, 3, 16, 11), "FREQ=WEEKLY;COUNT=5", user="America/New_York"),
    Case("dst-october-same-local-time", "tz", "Europe/Berlin", D(2026, 10, 12, 10), D(2026, 10, 12, 11), "FREQ=WEEKLY;COUNT=4"),
    Case("dst-gap-berlin-0230", "tz", "Europe/Berlin", D(2026, 3, 27, 2, 30), D(2026, 3, 27, 3, 30), "FREQ=DAILY;COUNT=5"),
    Case("dst-overlap-berlin-0230", "tz", "Europe/Berlin", D(2026, 10, 24, 2, 30), D(2026, 10, 24, 3, 30), "FREQ=DAILY;COUNT=4"),
    Case("dst-gap-new-york-0230", "tz", "America/New_York", D(2026, 3, 7, 2, 30), D(2026, 3, 7, 3, 30), "FREQ=DAILY;COUNT=3", user="America/New_York"),
    Case("dst-overlap-new-york-0130", "tz", "America/New_York", D(2026, 10, 31, 1, 30), D(2026, 10, 31, 2, 30), "FREQ=DAILY;COUNT=3", user="America/New_York"),
    Case("dst-sydney-southern", "tz", "Australia/Sydney", D(2026, 3, 30, 9), D(2026, 3, 30, 10), "FREQ=WEEKLY;COUNT=4", user="Australia/Sydney"),
    Case("dst-sydney-october", "tz", "Australia/Sydney", D(2026, 9, 28, 9), D(2026, 9, 28, 10), "FREQ=WEEKLY;COUNT=4", user="Australia/Sydney"),
    Case("allday-daily", "date", None, D(2026, 8, 3), D(2026, 8, 4), "FREQ=DAILY;COUNT=4"),
    Case("allday-weekly-two-days", "date", None, D(2026, 8, 7), D(2026, 8, 9), "FREQ=WEEKLY;COUNT=4"),
    Case("allday-yearly-birthday", "date", None, D(2020, 6, 15), D(2020, 6, 16), "FREQ=YEARLY", window=("2026-01-01", "2030-01-01")),
    Case("floating-weekly-berlin", "floating", None, D(2026, 9, 7, 7), D(2026, 9, 7, 8), "FREQ=WEEKLY;COUNT=4"),
    Case("floating-weekly-tokyo", "floating", None, D(2026, 9, 7, 7), D(2026, 9, 7, 8), "FREQ=WEEKLY;COUNT=4", user="Asia/Tokyo"),
    Case("utc-weekly-across-october", "utc", None, D(2026, 10, 12, 8), D(2026, 10, 12, 9), "FREQ=WEEKLY;COUNT=4"),
    Case("exdate-two-instances", "tz", "Europe/Berlin", D(2026, 5, 4, 10), D(2026, 5, 4, 11), "FREQ=DAILY;COUNT=6", exdates=[D(2026, 5, 5, 10), D(2026, 5, 7, 10)]),
    Case("exdate-all-day", "date", None, D(2026, 8, 3), D(2026, 8, 4), "FREQ=DAILY;COUNT=5", exdates=[D(2026, 8, 5)]),
    Case("rdate-added", "tz", "Europe/Berlin", D(2026, 5, 4, 10), D(2026, 5, 4, 11), "FREQ=WEEKLY;COUNT=2", rdates=[D(2026, 5, 20, 16)]),
    Case("count-includes-excluded", "tz", "Europe/Berlin", D(2026, 5, 4, 10), D(2026, 5, 4, 11), "FREQ=DAILY;COUNT=4", exdates=[D(2026, 5, 4, 10)]),
    Case("override-moved-and-cancelled", "tz", "Europe/Berlin", D(2026, 5, 4, 10), D(2026, 5, 4, 11), "FREQ=DAILY;COUNT=5",
         overrides=[(D(2026, 5, 5, 10), D(2026, 5, 5, 15), D(2026, 5, 5, 16), False), (D(2026, 5, 6, 10), D(2026, 5, 6, 10), D(2026, 5, 6, 10), True)]),
    Case("february-30th-dropped", "tz", "Europe/Berlin", D(2026, 1, 30, 12), D(2026, 1, 30, 13), "FREQ=MONTHLY;BYMONTHDAY=30;COUNT=5"),
    Case("window-only-expands-the-window", "tz", "Europe/Berlin", D(2020, 1, 1, 12), D(2020, 1, 1, 13), "FREQ=DAILY", window=("2026-06-01", "2026-06-05")),
    Case("weekly-bymonth-limit", "tz", "Europe/Berlin", D(2026, 6, 1, 12), D(2026, 6, 1, 13), "FREQ=WEEKLY;BYMONTH=6,8;BYDAY=MO;UNTIL=20260831"),
    Case("daily-byday-limit", "tz", "Europe/Berlin", D(2026, 6, 1, 12), D(2026, 6, 1, 13), "FREQ=DAILY;BYDAY=MO,WE,FR;COUNT=8"),
    Case("single-timed", "tz", "Europe/Berlin", D(2026, 10, 14, 12), D(2026, 10, 14, 13), ""),
    Case("single-kolkata-seen-from-berlin", "tz", "Asia/Kolkata", D(2026, 10, 14, 9), D(2026, 10, 14, 10), ""),
]

with open(OUT, "w") as f:
    f.write("# Generated by scripts/gen-golden-occurrences.py from python-dateutil and zoneinfo (IANA %s). Do not edit by hand.\n" % "2026c")
    for c in cases:
        f.write("### %s\nuser: %s\nwindow: %s %s\n--- ics\n" % (c.name, c.user, c.window[0], c.window[1]))
        for l in ics_for(c):
            f.write(l + "\n")
        f.write("--- expect\n")
        for e in expected(c):
            f.write(e + "\n")
        f.write("--- end\n")
print("wrote %d cases to %s" % (len(cases), OUT))
