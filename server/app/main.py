from pathlib import Path
import os

from fastapi import Depends, FastAPI, Header, HTTPException, Query
from fastapi.responses import FileResponse
from pydantic import BaseModel, Field

from . import db
from .feed import compose_feed, parse_date, parse_datetime


APP_DIR = Path(__file__).resolve().parent
STATIC_DIR = APP_DIR / "static"

app = FastAPI(title="Simple Day", version="1.0.0")


class LoginRequest(BaseModel):
    password: str


class EventIn(BaseModel):
    title: str
    starts_at: str
    participants: list[str] = Field(default_factory=list)
    location: str = ""
    category: str = "life"


class EventPatch(BaseModel):
    title: str | None = None
    starts_at: str | None = None
    participants: list[str] | None = None
    location: str | None = None
    category: str | None = None


class TodoIn(BaseModel):
    title: str
    completed: bool = False
    category: str = "life"


class TodoPatch(BaseModel):
    title: str | None = None
    completed: bool | None = None
    category: str | None = None


class DeviceTodoPatch(BaseModel):
    completed: bool


class ShoppingIn(BaseModel):
    title: str
    completed: bool = False


class ShoppingPatch(BaseModel):
    title: str | None = None
    completed: bool | None = None


class DeviceShoppingPatch(BaseModel):
    completed: bool


class DeadlineIn(BaseModel):
    title: str
    due_date: str
    completed: bool = False


class DeadlinePatch(BaseModel):
    title: str | None = None
    due_date: str | None = None
    completed: bool | None = None


def admin_token():
    return os.getenv("SIMPLE_DAY_ADMIN_TOKEN", "change-me-admin")


def device_token():
    return os.getenv("SIMPLE_DAY_DEVICE_TOKEN", "change-me-device")


def model_dict(model, exclude_unset=False):
    return model.model_dump(exclude_unset=exclude_unset)


def require_admin(authorization: str = Header(default="")):
    expected = "Bearer " + admin_token()
    if authorization != expected:
        raise HTTPException(status_code=401, detail="invalid admin token")
    return True


def get_conn():
    conn = db.connect()
    try:
        db.init_db(conn)
        yield conn
    finally:
        conn.close()


def validate_title(title):
    if not title or not title.strip():
        raise HTTPException(status_code=422, detail="title is required")
    return title.strip()


def normalize_category(value):
    text = str(value or "life").strip().lower()
    if text not in {"life", "project"}:
        raise HTTPException(status_code=422, detail="category must be life or project")
    return text


@app.on_event("startup")
def startup():
    db.init_db()


@app.get("/")
def index():
    return FileResponse(STATIC_DIR / "index.html")


@app.post("/api/login")
def login(payload: LoginRequest):
    if payload.password != admin_token():
        raise HTTPException(status_code=401, detail="wrong password")
    return {"ok": True, "token": payload.password}


@app.get("/api/items")
def list_items(conn=Depends(get_conn), _=Depends(require_admin)):
    events = [db.decode_event(row) for row in db.rows(conn, "SELECT * FROM events ORDER BY starts_at, id")]
    todos = db.rows(conn, "SELECT * FROM todos ORDER BY completed, id")
    deadlines = db.rows(conn, "SELECT * FROM deadlines ORDER BY completed, due_date, id")
    shopping = db.rows(conn, "SELECT * FROM shopping_items ORDER BY completed, id")
    return {"events": events, "todos": todos, "deadlines": deadlines, "shopping": shopping}


@app.post("/api/events")
def create_event(payload: EventIn, conn=Depends(get_conn), _=Depends(require_admin)):
    validate_title(payload.title)
    payload.category = normalize_category(payload.category)
    try:
        parse_datetime(payload.starts_at)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail="starts_at must be an ISO datetime") from exc
    return db.insert_event(conn, model_dict(payload))


@app.patch("/api/events/{item_id}")
def patch_event(item_id: int, payload: EventPatch, conn=Depends(get_conn), _=Depends(require_admin)):
    data = model_dict(payload, exclude_unset=True)
    if "title" in data:
        validate_title(data["title"])
    if "category" in data:
        data["category"] = normalize_category(data["category"])
    if "starts_at" in data:
        try:
            parse_datetime(data["starts_at"])
        except ValueError as exc:
            raise HTTPException(status_code=422, detail="starts_at must be an ISO datetime") from exc
    updated = db.update_event(conn, item_id, data)
    if not updated:
        raise HTTPException(status_code=404, detail="event not found")
    return updated


@app.delete("/api/events/{item_id}")
def delete_event(item_id: int, conn=Depends(get_conn), _=Depends(require_admin)):
    conn.execute("DELETE FROM events WHERE id = ?", (item_id,))
    conn.commit()
    return {"ok": True}


@app.post("/api/todos")
def create_todo(payload: TodoIn, conn=Depends(get_conn), _=Depends(require_admin)):
    validate_title(payload.title)
    payload.category = normalize_category(payload.category)
    return db.insert_todo(conn, model_dict(payload))


@app.patch("/api/todos/{item_id}")
def patch_todo(item_id: int, payload: TodoPatch, conn=Depends(get_conn), _=Depends(require_admin)):
    data = model_dict(payload, exclude_unset=True)
    if "title" in data:
        validate_title(data["title"])
    if "category" in data:
        data["category"] = normalize_category(data["category"])
    updated = db.update_todo(conn, item_id, data)
    if not updated:
        raise HTTPException(status_code=404, detail="todo not found")
    return updated


@app.delete("/api/todos/{item_id}")
def delete_todo(item_id: int, conn=Depends(get_conn), _=Depends(require_admin)):
    conn.execute("DELETE FROM todos WHERE id = ?", (item_id,))
    conn.commit()
    return {"ok": True}


@app.post("/api/shopping")
def create_shopping(payload: ShoppingIn, conn=Depends(get_conn), _=Depends(require_admin)):
    validate_title(payload.title)
    return db.insert_shopping(conn, model_dict(payload))


@app.patch("/api/shopping/{item_id}")
def patch_shopping(item_id: int, payload: ShoppingPatch, conn=Depends(get_conn), _=Depends(require_admin)):
    data = model_dict(payload, exclude_unset=True)
    if "title" in data:
        validate_title(data["title"])
    updated = db.update_shopping(conn, item_id, data)
    if not updated:
        raise HTTPException(status_code=404, detail="shopping item not found")
    return updated


@app.delete("/api/shopping/{item_id}")
def delete_shopping(item_id: int, conn=Depends(get_conn), _=Depends(require_admin)):
    conn.execute("DELETE FROM shopping_items WHERE id = ?", (item_id,))
    conn.commit()
    return {"ok": True}


@app.post("/api/deadlines")
def create_deadline(payload: DeadlineIn, conn=Depends(get_conn), _=Depends(require_admin)):
    validate_title(payload.title)
    try:
        parse_date(payload.due_date)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail="due_date must be YYYY-MM-DD") from exc
    return db.insert_deadline(conn, model_dict(payload))


@app.patch("/api/deadlines/{item_id}")
def patch_deadline(item_id: int, payload: DeadlinePatch, conn=Depends(get_conn), _=Depends(require_admin)):
    data = model_dict(payload, exclude_unset=True)
    if "title" in data:
        validate_title(data["title"])
    if "due_date" in data:
        try:
            parse_date(data["due_date"])
        except ValueError as exc:
            raise HTTPException(status_code=422, detail="due_date must be YYYY-MM-DD") from exc
    updated = db.update_deadline(conn, item_id, data)
    if not updated:
        raise HTTPException(status_code=404, detail="deadline not found")
    return updated


@app.delete("/api/deadlines/{item_id}")
def delete_deadline(item_id: int, conn=Depends(get_conn), _=Depends(require_admin)):
    conn.execute("DELETE FROM deadlines WHERE id = ?", (item_id,))
    conn.commit()
    return {"ok": True}


@app.get("/api/device/feed")
def device_feed(token: str = Query(default=""), conn=Depends(get_conn)):
    if token != device_token():
        raise HTTPException(status_code=401, detail="invalid device token")
    events = db.rows(conn, "SELECT * FROM events ORDER BY starts_at, id")
    todos = db.rows(conn, "SELECT * FROM todos ORDER BY id")
    shopping = db.rows(conn, "SELECT * FROM shopping_items ORDER BY id")
    deadlines = db.rows(conn, "SELECT * FROM deadlines ORDER BY due_date, id")
    return compose_feed(events, todos, shopping, deadlines)


@app.patch("/api/device/todos/{item_id}")
def patch_device_todo(
    item_id: int,
    payload: DeviceTodoPatch,
    token: str = Query(default=""),
    conn=Depends(get_conn),
):
    if token != device_token():
        raise HTTPException(status_code=401, detail="invalid device token")
    updated = db.update_todo(conn, item_id, {"completed": payload.completed})
    if not updated:
        raise HTTPException(status_code=404, detail="todo not found")
    return updated


@app.patch("/api/device/shopping/{item_id}")
def patch_device_shopping(
    item_id: int,
    payload: DeviceShoppingPatch,
    token: str = Query(default=""),
    conn=Depends(get_conn),
):
    if token != device_token():
        raise HTTPException(status_code=401, detail="invalid device token")
    updated = db.update_shopping(conn, item_id, {"completed": payload.completed})
    if not updated:
        raise HTTPException(status_code=404, detail="shopping item not found")
    return updated
