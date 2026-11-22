"""Bounded, read-only adapter for a Javis603/token-monitor Node Hub."""

import argparse
import datetime as dt
import hmac
import ipaddress
import json
import os
import re
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

SNAPSHOT_PATH = "/watch/v1/snapshot"
MAX_SNAPSHOT_BYTES = 8192
MAX_HUB_BYTES = 8 * 1024 * 1024
LEGACY_QUOTA_ROWS = 12
MAX_QUOTA_ROWS = 24


def obj(value):
    return value if isinstance(value, dict) else {}


def items(value):
    return value if isinstance(value, list) else []


def number(value, *, maximum=9_007_199_254_740_991):
    if type(value) in (int, float) and 0 <= value <= maximum:
        return value
    return None


def label(value, fallback="", length=22):
    if not isinstance(value, str):
        return fallback
    cleaned = " ".join(value.encode("ascii", "replace").decode("ascii").split())
    return (cleaned[:length] or fallback)


def epoch(value):
    if not isinstance(value, str):
        return None
    try:
        stamp = dt.datetime.fromisoformat(value.replace("Z", "+00:00"))
        return int(stamp.timestamp()) if stamp.utcoffset() is not None else None
    except (ValueError, OverflowError, OSError):
        return None


def top(period, key, costs):
    rows = []
    for name, raw in obj(period.get(key)).items():
        total = number(obj(raw).get("totalTokens", raw))
        if total is None:
            continue
        cost = number(obj(period.get(costs)).get(name))
        rows.append({"label": label(name, "Other"), "tokens": total, "cost": cost})
    return sorted(rows, key=lambda row: (-row["tokens"], row["label"]))[:4]


def totals(period):
    return {"tokens": number(period.get("totalTokens")), "cost": number(period.get("costUsd"))}


def quota_period(window):
    kind = str(window.get("kind") or "").strip().lower()
    title = str(window.get("label") or "").lower()
    duration = number(window.get("windowMinutes"), maximum=527_040)
    minutes = int(duration) if duration and duration >= 1 else None
    if kind == "credits":
        return "unknown", None
    if kind in ("monthly", "month") or (
        kind == "billing" and ("month" in title or minutes and 40_320 <= minutes <= 44_640)
    ):
        return "month", minutes
    if kind == "billing":
        return "billing", minutes
    if kind in ("weekly", "week"):
        return "week", minutes
    if kind in ("daily", "day"):
        return "day", minutes
    if kind == "session":
        return "session", minutes
    hours = re.fullmatch(r"(\d{1,2})(?:h|\s*[- ]\s*hours?)", kind or title)
    if hours and 0 < int(hours[1]) < 24:
        return "session", int(hours[1]) * 60
    if re.search(r"\bmonthly\b", title) or minutes in (40_320, 43_200, 44_640):
        return "month", minutes
    if re.search(r"\bweekly\b", title) or minutes == 10_080:
        return "week", minutes
    if re.search(r"\bdaily\b", title) or minutes == 1440:
        return "day", minutes
    if minutes:
        return ("session" if minutes < 1440 else "cycle"), minutes
    return "unknown", None


def quota_rows(stats, now):
    rows, unit_counts = [], {}
    for raw_provider in items(obj(stats.get("limits")).get("providers")):
        provider = obj(raw_provider)
        name = label(provider.get("provider"), "Provider", 16)
        unit_counts[name] = unit_counts.get(name, 0) + 1
        account = unit_counts[name]
        updated = epoch(provider.get("updatedAt"))
        stale = (
            provider.get("status") not in (None, "ok")
            or provider.get("stale") is True
            or (updated is not None and now - updated > 1800)
        )
        for raw in items(provider.get("windows")):
            window = obj(raw)
            reset = epoch(window.get("resetsAt"))
            remaining = number(window.get("remainingPercent"), maximum=100)
            used = number(window.get("usedPercent"), maximum=100)
            if remaining is None and used is not None:
                remaining = 100 - used
            window_name = label(window.get("label") or window.get("kind"), "Usage", 14)
            if remaining is None and reset is None:
                continue
            period, minutes = quota_period(window)
            row = {
                "label": label(f"{name} #{account} {window_name}", "Quota", 26),
                "pct": remaining,
                "reset": reset,
                "stale": stale or (reset is not None and reset <= now),
                "period": period,
            }
            if minutes is not None:
                row["minutes"] = minutes
            rows.append(row)
            if len(rows) == MAX_QUOTA_ROWS:
                return rows
    return rows


def device_rows(stats, now, *, show_device_names=False):
    rows = []
    stale_after = number(stats.get("staleAfterMs"))
    for index, raw in enumerate(items(stats.get("devices"))[:12], start=1):
        device = obj(raw)
        updated = epoch(device.get("receivedAt") or device.get("updatedAt"))
        today = obj(obj(device.get("periods")).get("today") or device.get("today"))
        row = {
            "label": f"Device {index}",
            "tokens": number(today.get("totalTokens")),
            "updated": updated,
            "stale": (
                device.get("stale") is True
                or updated is None
                or (stale_after is not None and (now - updated) * 1000 > stale_after)
            ),
        }
        if show_device_names:
            hostname = device.get("hostname")
            if isinstance(hostname, str):
                name = " ".join("".join(ch if ch.isprintable() else " " for ch in hostname).split())[:22].rstrip()
                if name:
                    row.update(label=name, named=True)
        rows.append(row)
    return rows


def trend_rows(history, now):
    by_day = {}
    today = dt.datetime.fromtimestamp(now, dt.timezone.utc).date()
    for raw in items(obj(history).get("daily")):
        row = obj(raw)
        day = row.get("date")
        try:
            parsed = dt.date.fromisoformat(day)
        except (ValueError, TypeError):
            continue
        if 0 <= (today - parsed).days < 30:
            by_day[parsed] = {
                "day": parsed.isoformat()[5:],
                "tokens": number(row.get("tokens", row.get("totalTokens"))),
                "cost": number(row.get("cost", row.get("costUsd"))),
            }
    if not by_day:
        return []
    return [
        by_day.get(day, {"day": day.isoformat()[5:], "tokens": None, "cost": None})
        for day in (today - dt.timedelta(days=x) for x in range(29, -1, -1))
    ]


def snapshot(stats, history, now, *, show_device_names=False):
    stats = obj(stats)
    periods = obj(stats.get("periods"))
    if not isinstance(periods.get("today"), dict):
        raise ValueError("Hub response has no periods.today object")
    today = periods["today"]
    limits = quota_rows(stats, now)
    return {
        "version": 1,
        "asOf": now,
        "today": totals(today),
        "month": totals(obj(periods.get("month"))),
        "tools": top(today, "clients", "clientCosts"),
        "models": top(today, "models", "modelCosts"),
        "limits": limits[:LEGACY_QUOTA_ROWS],
        "limitsMore": limits[LEGACY_QUOTA_ROWS:],
        "trend": trend_rows(history, now),
        "devices": device_rows(stats, now, show_device_names=show_device_names),
    }


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def hub_reader(base, secret, allow_http=False):
    url = urllib.parse.urlsplit(base)
    if (
        not url.hostname
        or url.username
        or url.password
        or url.query
        or url.fragment
        or (url.scheme != "https" and not (allow_http and url.scheme == "http"))
    ):
        raise ValueError("Specify a base Hub HTTPS URL without credentials or a query")
    opener = urllib.request.build_opener(NoRedirect())

    def read(path):
        req = urllib.request.Request(
            base.rstrip("/") + path,
            headers={"Authorization": f"Bearer {secret}", "Accept": "application/json"},
        )
        with opener.open(req, timeout=8) as response:
            body = response.read(MAX_HUB_BYTES + 1)
        if len(body) > MAX_HUB_BYTES:
            raise ValueError("Hub response exceeded the configured size limit")
        payload = json.loads(body)
        if not isinstance(payload, dict):
            raise ValueError("Hub response must be a JSON object")
        return payload

    return read


class Feed:
    def __init__(self, read, *, now=time.time, show_device_names=False):
        self.read = read
        self.now = now
        self.show_device_names = show_device_names
        self.body = None
        self.history = {}
        self.next_stats = self.next_history = 0
        self.lock = threading.Lock()

    def get(self):
        with self.lock:
            now = int(self.now())
            if now < self.next_stats and self.body is not None:
                return self.body
            stats = self.read("/api/stats")
            if now >= self.next_history:
                try:
                    self.history = self.read("/api/history")
                    self.next_history = now + 300
                except (ValueError, OSError):
                    self.next_history = now + 30
            body = json.dumps(
                snapshot(stats, self.history, now, show_device_names=self.show_device_names),
                separators=(",", ":"),
                allow_nan=False,
            ).encode("ascii")
            if len(body) > MAX_SNAPSHOT_BYTES:
                raise ValueError("Watch snapshot exceeded the configured size limit")
            self.body = body
            self.next_stats = now + 30
            return body


def serve(address, feed, secret=None):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path != SNAPSHOT_PATH:
                self.send_error(404)
                return
            if secret:
                supplied = self.headers.get("Authorization", "").encode("utf-8")
                if not hmac.compare_digest(supplied, f"Bearer {secret}".encode("utf-8")):
                    self.send_error(401)
                    return
            try:
                body = feed.get()
            except (ValueError, OSError):
                self.send_error(502, "Token Monitor snapshot unavailable")
                return
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return ThreadingHTTPServer(address, Handler)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=17322)
    parser.add_argument("--fixture", action="store_true", help="Serve synthetic test data")
    parser.add_argument("--show-device-names", action="store_true", help="Include Hub hostnames in the protected snapshot")
    args = parser.parse_args()
    try:
        loopback = ipaddress.ip_address(args.host).is_loopback
    except ValueError:
        parser.error("--host must be an IP address")
    if ":" in args.host:
        parser.error("--host must be an IPv4 address")
    watch_secret = os.environ.get("STOPWATCH_DEVICE_TOKEN")
    if not loopback and not watch_secret and not args.fixture:
        parser.error("A non-loopback upstream relay requires STOPWATCH_DEVICE_TOKEN")
    if args.show_device_names and not watch_secret and not args.fixture:
        parser.error("Real device names require STOPWATCH_DEVICE_TOKEN")
    if args.fixture:
        from fixture_data import fixture
        read = lambda path: fixture(int(time.time()))[path]
    else:
        base = os.environ.get("STOPWATCH_HUB_URL")
        secret = os.environ.get("TOKEN_MONITOR_SECRET")
        if not base or not secret:
            parser.error("Set STOPWATCH_HUB_URL and TOKEN_MONITOR_SECRET in this process")
        read = hub_reader(base, secret)
    with serve((args.host, args.port), Feed(read, show_device_names=args.show_device_names), watch_secret) as server:
        print(f"Listening on {args.host}:{server.server_port}{SNAPSHOT_PATH}", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
