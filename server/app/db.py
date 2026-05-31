from pathlib import Path
import json
import os
import sqlite3

from .feed import iso_now


DEFAULT_DB = Path(__file__).resolve().parents[1] / "data" / "simple_day.sqlite"


def db_path():
    return Path(os.getenv("SIMPLE_DAY_DB", str(DEFAULT_DB)))


def connect():
    path = db_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(str(path), check_same_thread=False)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA foreign_keys = ON")
    return conn


def init_db(conn=None):
    own_conn = conn is None
    conn = conn or connect()
    conn.executescript(
        """
        CREATE TABLE IF NOT EXISTS events (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            starts_at TEXT NOT NULL,
            participants TEXT NOT NULL DEFAULT '[]',
            location TEXT NOT NULL DEFAULT '',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS todos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS deadlines (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            due_date TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
        """
    )
    conn.commit()
    if own_conn:
        conn.close()


def rows(conn, sql, args=()):
    return [dict(row) for row in conn.execute(sql, args).fetchall()]


def one(conn, sql, args=()):
    row = conn.execute(sql, args).fetchone()
    return dict(row) if row else None


def encode_participants(value):
    if value is None:
        value = []
    if isinstance(value, str):
        value = [item.strip() for item in value.split(",") if item.strip()]
    return json.dumps([str(item).strip() for item in value if str(item).strip()], ensure_ascii=False)


def decode_event(row):
    row = dict(row)
    try:
        row["participants"] = json.loads(row.get("participants") or "[]")
    except json.JSONDecodeError:
        row["participants"] = []
    return row


def insert_event(conn, data):
    stamp = iso_now()
    cur = conn.execute(
        """
        INSERT INTO events (title, starts_at, participants, location, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            data["title"].strip(),
            data["starts_at"],
            encode_participants(data.get("participants")),
            (data.get("location") or "").strip(),
            stamp,
            stamp,
        ),
    )
    conn.commit()
    return decode_event(one(conn, "SELECT * FROM events WHERE id = ?", (cur.lastrowid,)))


def update_event(conn, item_id, data):
    current = one(conn, "SELECT * FROM events WHERE id = ?", (item_id,))
    if not current:
        return None
    merged = {
        "title": data.get("title", current["title"]),
        "starts_at": data.get("starts_at", current["starts_at"]),
        "participants": data.get("participants", json.loads(current["participants"] or "[]")),
        "location": data.get("location", current["location"]),
    }
    conn.execute(
        """
        UPDATE events
        SET title = ?, starts_at = ?, participants = ?, location = ?, updated_at = ?
        WHERE id = ?
        """,
        (
            merged["title"].strip(),
            merged["starts_at"],
            encode_participants(merged.get("participants")),
            (merged.get("location") or "").strip(),
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return decode_event(one(conn, "SELECT * FROM events WHERE id = ?", (item_id,)))


def insert_todo(conn, data):
    stamp = iso_now()
    cur = conn.execute(
        "INSERT INTO todos (title, completed, created_at, updated_at) VALUES (?, ?, ?, ?)",
        (data["title"].strip(), int(bool(data.get("completed", False))), stamp, stamp),
    )
    conn.commit()
    return one(conn, "SELECT * FROM todos WHERE id = ?", (cur.lastrowid,))


def update_todo(conn, item_id, data):
    current = one(conn, "SELECT * FROM todos WHERE id = ?", (item_id,))
    if not current:
        return None
    conn.execute(
        "UPDATE todos SET title = ?, completed = ?, updated_at = ? WHERE id = ?",
        (
            data.get("title", current["title"]).strip(),
            int(bool(data.get("completed", current["completed"]))),
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM todos WHERE id = ?", (item_id,))


def insert_deadline(conn, data):
    stamp = iso_now()
    cur = conn.execute(
        "INSERT INTO deadlines (title, due_date, completed, created_at, updated_at) VALUES (?, ?, ?, ?, ?)",
        (
            data["title"].strip(),
            data["due_date"],
            int(bool(data.get("completed", False))),
            stamp,
            stamp,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM deadlines WHERE id = ?", (cur.lastrowid,))


def update_deadline(conn, item_id, data):
    current = one(conn, "SELECT * FROM deadlines WHERE id = ?", (item_id,))
    if not current:
        return None
    conn.execute(
        "UPDATE deadlines SET title = ?, due_date = ?, completed = ?, updated_at = ? WHERE id = ?",
        (
            data.get("title", current["title"]).strip(),
            data.get("due_date", current["due_date"]),
            int(bool(data.get("completed", current["completed"]))),
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM deadlines WHERE id = ?", (item_id,))
