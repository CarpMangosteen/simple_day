import sys
import unittest
from datetime import datetime, timedelta
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from app.feed import SH_TZ, compact_deadline_delta, compact_event_delta, compose_feed


class FeedTests(unittest.TestCase):
    def test_compose_feed_orders_next_and_filters_completed(self):
        now = datetime(2026, 5, 31, 9, 0, tzinfo=SH_TZ)
        events = [
            {
                "id": 1,
                "title": "晚饭",
                "starts_at": (now + timedelta(hours=10)).isoformat(),
                "participants": "[]",
                "location": "",
                "category": "life",
            },
            {
                "id": 2,
                "title": "设计评审",
                "starts_at": (now + timedelta(hours=7)).isoformat(),
                "participants": '["李小宇","小姜"]',
                "location": "腾讯会议",
                "category": "project",
            },
        ]
        todos = [
            {"id": 1, "title": "life open", "completed": 0, "category": "life"},
            {
                "id": 2,
                "title": "project done",
                "completed": 1,
                "completed_at": now.isoformat(timespec="seconds"),
                "category": "project",
            },
            {
                "id": 3,
                "title": "life old",
                "completed": 1,
                "completed_at": (now - timedelta(days=1)).isoformat(timespec="seconds"),
                "category": "life",
            },
        ]
        shopping = [
            {"id": 1, "title": "milk", "completed": 0},
        ]
        deadlines = [
            {"id": 1, "title": "v1.0 simple 发布", "due_date": "2026-06-02", "completed": 0},
            {"id": 2, "title": "done", "due_date": "2026-06-01", "completed": 1},
        ]

        feed = compose_feed(events, todos, shopping, deadlines, now=now)

        self.assertEqual(feed["life"]["next"]["title"], "晚饭")
        self.assertEqual(feed["project"]["next"]["title"], "设计评审")
        self.assertEqual(feed["project"]["next"]["time"], "16:00")
        self.assertEqual(feed["project"]["next"]["with"], ["李小宇", "小姜"])
        self.assertEqual([item["title"] for item in feed["life"]["today"]], ["晚饭"])
        self.assertEqual(feed["life"]["todos"], [{"id": 1, "title": "life open", "completed": False}])
        self.assertEqual(feed["project"]["todos"], [{"id": 2, "title": "project done", "completed": True}])
        self.assertEqual(feed["life"]["shopping"], [{"id": 1, "title": "milk", "completed": False}])
        self.assertEqual(feed["deadlines"][0]["relative"], "in 2d")

    def test_relative_time_formats(self):
        now = datetime(2026, 5, 31, 9, 0, tzinfo=SH_TZ)
        self.assertEqual(compact_event_delta(now + timedelta(days=1, hours=3, minutes=16), now), "in 1d3h16m")
        self.assertEqual(compact_deadline_delta(now.date() + timedelta(days=21), now), "in 3w")


if __name__ == "__main__":
    unittest.main()
