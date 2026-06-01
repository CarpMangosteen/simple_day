from datetime import date, datetime, time, timedelta, timezone
import json
import math


SH_TZ = timezone(timedelta(hours=8))


def now_in_timezone():
    return datetime.now(SH_TZ)


def iso_now():
    return now_in_timezone().isoformat(timespec="seconds")


def parse_datetime(value):
    if isinstance(value, datetime):
        dt = value
    else:
        text = str(value).strip()
        if not text:
            raise ValueError("empty datetime")
        if text.endswith("Z"):
            text = text[:-1] + "+00:00"
        if len(text) == 16 and "T" in text:
            text = text + ":00"
        dt = datetime.fromisoformat(text)
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=SH_TZ)
    return dt.astimezone(SH_TZ)


def parse_date(value):
    if isinstance(value, date) and not isinstance(value, datetime):
        return value
    return date.fromisoformat(str(value)[:10])


def normalize_participants(value):
    if value is None or value == "":
        return []
    if isinstance(value, list):
        return [str(item).strip() for item in value if str(item).strip()]
    if isinstance(value, str):
        try:
            decoded = json.loads(value)
            if isinstance(decoded, list):
                return normalize_participants(decoded)
        except json.JSONDecodeError:
            pass
        return [item.strip() for item in value.split(",") if item.strip()]
    return []


def compact_event_delta(target, now=None):
    now = now or now_in_timezone()
    seconds = int((target - now).total_seconds())
    if abs(seconds) < 60:
        return "now"
    overdue = seconds < 0
    seconds = abs(seconds)
    days = seconds // 86400
    seconds %= 86400
    hours = seconds // 3600
    seconds %= 3600
    minutes = max(1, int(math.ceil(seconds / 60.0))) if days == 0 else seconds // 60

    parts = []
    if days:
        parts.append("%dd" % days)
    if hours or days:
        parts.append("%dh" % hours)
    if not days:
        parts.append("%dm" % minutes)
    elif minutes:
        parts.append("%dm" % minutes)
    text = "".join(parts)
    return ("overdue " if overdue else "in ") + text


def compact_deadline_delta(target_date, now=None):
    now = now or now_in_timezone()
    days = (target_date - now.date()).days
    if days < 0:
        return "overdue %dd" % abs(days)
    if days == 0:
        return "today"
    if days == 1:
        return "in 1d"
    if days >= 14:
        return "in %dw" % int(math.ceil(days / 7.0))
    return "in %dd" % days


def _event_payload(row, now):
    starts_at = parse_datetime(row["starts_at"])
    return {
        "id": row.get("id"),
        "title": row.get("title", ""),
        "starts_at": starts_at.isoformat(timespec="seconds"),
        "time": starts_at.strftime("%H:%M"),
        "relative": compact_event_delta(starts_at, now),
        "with": normalize_participants(row.get("participants")),
        "where": row.get("location") or "",
    }


def compose_feed(events, todos, deadlines, now=None):
    now = now or now_in_timezone()

    active_events = []
    for row in events:
        try:
            event = dict(row)
            event["_starts_dt"] = parse_datetime(event["starts_at"])
            active_events.append(event)
        except (KeyError, ValueError):
            continue
    active_events.sort(key=lambda item: item["_starts_dt"])

    future_events = [row for row in active_events if row["_starts_dt"] >= now]
    today_events = [row for row in active_events if row["_starts_dt"].date() == now.date()]

    next_event = _event_payload(future_events[0], now) if future_events else None
    today_payload = [_event_payload(row, now) for row in today_events]

    todo_payload = []
    for row in todos:
        item = dict(row)
        title = str(item.get("title", "")).strip()
        if not title:
            continue
        completed = bool(int(item.get("completed") or 0))
        todo_payload.append({"id": item.get("id"), "title": title, "completed": completed})
    todo_payload.sort(key=lambda item: (item.get("completed", False), item.get("id") or 0))

    deadline_payload = []
    for row in deadlines:
        item = dict(row)
        if int(item.get("completed") or 0):
            continue
        title = str(item.get("title", "")).strip()
        if not title:
            continue
        try:
            due_date = parse_date(item["due_date"])
        except (KeyError, ValueError):
            continue
        deadline_payload.append(
            {
                "id": item.get("id"),
                "title": title,
                "due_date": due_date.isoformat(),
                "relative": compact_deadline_delta(due_date, now),
            }
        )
    deadline_payload.sort(key=lambda item: item["due_date"])

    return {
        "generated_at": now.isoformat(timespec="seconds"),
        "timezone": "Asia/Shanghai",
        "next": next_event,
        "today": today_payload,
        "todos": todo_payload,
        "deadlines": deadline_payload,
    }


def sample_feed(now=None):
    now = now or datetime.combine(date.today(), time(9, 0), tzinfo=SH_TZ)
    return compose_feed(
        [
            {
                "id": 1,
                "title": "设计评审",
                "starts_at": (now + timedelta(hours=7)).isoformat(timespec="seconds"),
                "participants": ["李小宇", "小姜"],
                "location": "腾讯会议",
            },
            {
                "id": 2,
                "title": "晨会",
                "starts_at": now.replace(hour=10, minute=0).isoformat(timespec="seconds"),
                "participants": [],
                "location": "",
            },
        ],
        [{"id": 1, "title": "整理 Simple Day UI 草稿", "completed": 0}],
        [
            {
                "id": 1,
                "title": "v1.0 simple 发布",
                "due_date": (now.date() + timedelta(days=2)).isoformat(),
                "completed": 0,
            }
        ],
        now=now,
    )
