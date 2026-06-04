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
            category TEXT NOT NULL DEFAULT 'life',
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS todos (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            category TEXT NOT NULL DEFAULT 'life',
            completed INTEGER NOT NULL DEFAULT 0,
            completed_at TEXT,
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

        CREATE TABLE IF NOT EXISTS shopping_items (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            title TEXT NOT NULL,
            completed INTEGER NOT NULL DEFAULT 0,
            completed_at TEXT,
            created_at TEXT NOT NULL,
            updated_at TEXT NOT NULL
        );
        """
    )
    ensure_column(conn, "events", "category", "TEXT NOT NULL DEFAULT 'life'")
    ensure_column(conn, "todos", "category", "TEXT NOT NULL DEFAULT 'life'")
    ensure_column(conn, "todos", "completed_at", "TEXT")
    ensure_column(conn, "shopping_items", "completed_at", "TEXT")
    conn.commit()
    if own_conn:
        conn.close()


def rows(conn, sql, args=()):
    return [dict(row) for row in conn.execute(sql, args).fetchall()]


def one(conn, sql, args=()):
    row = conn.execute(sql, args).fetchone()
    return dict(row) if row else None


def table_columns(conn, table):
    rows = conn.execute(f"PRAGMA table_info({table})").fetchall()
    return {row[1] for row in rows}


def ensure_column(conn, table, column, definition):
    columns = table_columns(conn, table)
    if column in columns:
        return
    conn.execute(f"ALTER TABLE {table} ADD COLUMN {column} {definition}")


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
    if not row.get("category"):
        row["category"] = "life"
    return row


def insert_event(conn, data):
    stamp = iso_now()
    cur = conn.execute(
        """
        INSERT INTO events (title, starts_at, participants, location, category, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?, ?)
        """,
        (
            data["title"].strip(),
            data["starts_at"],
            encode_participants(data.get("participants")),
            (data.get("location") or "").strip(),
            data.get("category") or "life",
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
        "category": data.get("category", current.get("category") or "life"),
    }
    conn.execute(
        """
        UPDATE events
        SET title = ?, starts_at = ?, participants = ?, location = ?, category = ?, updated_at = ?
        WHERE id = ?
        """,
        (
            merged["title"].strip(),
            merged["starts_at"],
            encode_participants(merged.get("participants")),
            (merged.get("location") or "").strip(),
            merged.get("category") or "life",
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return decode_event(one(conn, "SELECT * FROM events WHERE id = ?", (item_id,)))


def insert_todo(conn, data):
    stamp = iso_now()
    completed = bool(data.get("completed", False))
    completed_at = stamp if completed else None
    cur = conn.execute(
        """
        INSERT INTO todos (title, category, completed, completed_at, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?, ?)
        """,
        (
            data["title"].strip(),
            data.get("category") or "life",
            int(completed),
            completed_at,
            stamp,
            stamp,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM todos WHERE id = ?", (cur.lastrowid,))


def update_todo(conn, item_id, data):
    current = one(conn, "SELECT * FROM todos WHERE id = ?", (item_id,))
    if not current:
        return None
    next_completed = bool(data.get("completed", current["completed"]))
    completed_at = current.get("completed_at")
    if next_completed:
        if not current["completed"] or not completed_at:
            completed_at = iso_now()
    else:
        completed_at = None
    conn.execute(
        """
        UPDATE todos
        SET title = ?, category = ?, completed = ?, completed_at = ?, updated_at = ?
        WHERE id = ?
        """,
        (
            data.get("title", current["title"]).strip(),
            data.get("category", current.get("category") or "life"),
            int(next_completed),
            completed_at,
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM todos WHERE id = ?", (item_id,))


def insert_shopping(conn, data):
    stamp = iso_now()
    completed = bool(data.get("completed", False))
    completed_at = stamp if completed else None
    cur = conn.execute(
        """
        INSERT INTO shopping_items (title, completed, completed_at, created_at, updated_at)
        VALUES (?, ?, ?, ?, ?)
        """,
        (data["title"].strip(), int(completed), completed_at, stamp, stamp),
    )
    conn.commit()
    return one(conn, "SELECT * FROM shopping_items WHERE id = ?", (cur.lastrowid,))


def update_shopping(conn, item_id, data):
    current = one(conn, "SELECT * FROM shopping_items WHERE id = ?", (item_id,))
    if not current:
        return None
    next_completed = bool(data.get("completed", current["completed"]))
    completed_at = current.get("completed_at")
    if next_completed:
        if not current["completed"] or not completed_at:
            completed_at = iso_now()
    else:
        completed_at = None
    conn.execute(
        """
        UPDATE shopping_items
        SET title = ?, completed = ?, completed_at = ?, updated_at = ?
        WHERE id = ?
        """,
        (
            data.get("title", current["title"]).strip(),
            int(next_completed),
            completed_at,
            iso_now(),
            item_id,
        ),
    )
    conn.commit()
    return one(conn, "SELECT * FROM shopping_items WHERE id = ?", (item_id,))


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
