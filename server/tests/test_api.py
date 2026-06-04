import os
import sqlite3
import sys
import unittest
from datetime import datetime, timedelta
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

os.environ["SIMPLE_DAY_ADMIN_TOKEN"] = "admin-test"
os.environ["SIMPLE_DAY_DEVICE_TOKEN"] = "device-test"

from fastapi.testclient import TestClient

from app.feed import SH_TZ
from app import db
from app.main import app, get_conn


class ApiTests(unittest.TestCase):
    def setUp(self):
        self.conn = sqlite3.connect(":memory:", check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        db.init_db(self.conn)

        def override_conn():
            yield self.conn

        app.dependency_overrides[get_conn] = override_conn
        self.client = TestClient(app)
        self.admin_headers = {"Authorization": "Bearer admin-test"}

    def tearDown(self):
        app.dependency_overrides.clear()
        self.conn.close()

    def test_admin_crud_and_device_feed(self):
        future = datetime.now(SH_TZ) + timedelta(days=1)
        future_start = future.replace(hour=16, minute=0, second=0, microsecond=0)
        future_deadline = (future.date() + timedelta(days=1)).isoformat()

        denied = self.client.get("/api/items")
        self.assertEqual(denied.status_code, 401)

        login = self.client.post("/api/login", json={"password": "admin-test"})
        self.assertEqual(login.status_code, 200)

        event = self.client.post(
            "/api/events",
            headers=self.admin_headers,
            json={
                "title": "设计评审",
                "starts_at": future_start.isoformat(timespec="minutes"),
                "participants": ["李小宇", "小姜"],
                "location": "腾讯会议",
                "category": "project",
            },
        )
        self.assertEqual(event.status_code, 200)

        todo = self.client.post(
            "/api/todos",
            headers=self.admin_headers,
            json={"title": "整理草稿", "category": "life"},
        )
        self.assertEqual(todo.status_code, 200)

        shopping = self.client.post(
            "/api/shopping",
            headers=self.admin_headers,
            json={"title": "牛奶"},
        )
        self.assertEqual(shopping.status_code, 200)

        deadline = self.client.post(
            "/api/deadlines",
            headers=self.admin_headers,
            json={"title": "v1.0 simple 发布", "due_date": future_deadline},
        )
        self.assertEqual(deadline.status_code, 200)

        bad_feed = self.client.get("/api/device/feed?token=wrong")
        self.assertEqual(bad_feed.status_code, 401)

        feed = self.client.get("/api/device/feed?token=device-test")
        self.assertEqual(feed.status_code, 200)
        payload = feed.json()
        self.assertEqual(payload["project"]["next"]["title"], "设计评审")
        self.assertEqual(payload["life"]["todos"][0]["title"], "整理草稿")
        self.assertEqual(payload["life"]["shopping"][0]["title"], "牛奶")
        self.assertEqual(payload["deadlines"][0]["title"], "v1.0 simple 发布")


if __name__ == "__main__":
    unittest.main()
