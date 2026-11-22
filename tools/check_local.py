"""Local end-to-end test: synthetic authenticated Hub -> real relay -> shared UI."""

import argparse
import asyncio
import contextlib
import json
import os
from pathlib import Path
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError
from urllib.request import Request, build_opener, ProxyHandler

from fixture_data import fixture
from watch_relay import Feed, SNAPSHOT_PATH, hub_reader, serve, snapshot


@contextlib.contextmanager
def running(server):
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        yield server
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)


def upstream(now):
    responses = fixture(now)

    class MockHub(BaseHTTPRequestHandler):
        available = True

        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.headers.get("Authorization") != "Bearer fixture-hub":
                self.send_error(401)
                return
            if self.path not in responses:
                self.send_error(404)
                return
            if not type(self).available:
                self.send_error(503)
                return
            body = json.dumps(responses[self.path]).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    return ThreadingHTTPServer(("127.0.0.1", 0), MockHub), MockHub


def relay_checks():
    now = int(time.time())
    clock = [now]
    mock, handler = upstream(now)
    direct = build_opener(ProxyHandler({}))
    with running(mock):
        source = hub_reader(f"http://127.0.0.1:{mock.server_port}", "fixture-hub", allow_http=True)
        with running(serve(("127.0.0.1", 0), Feed(source, now=lambda: clock[0]), "fixture-watch")) as relay:
            base = f"http://127.0.0.1:{relay.server_port}"

            def get(path=SNAPSHOT_PATH, auth=True):
                headers = {"Authorization": "Bearer fixture-watch"} if auth else {}
                with direct.open(Request(base + path, headers=headers), timeout=3) as response:
                    return response.read().decode("ascii")

            def expect_error(status, path=SNAPSHOT_PATH, auth=True):
                try:
                    get(path, auth)
                except HTTPError as error:
                    assert error.code == status, error.code
                else:
                    raise AssertionError(f"Expected HTTP {status}")

            expect_error(401, auth=False)
            expect_error(404, "/api/stats")
            body = get()
            parsed = json.loads(body)
            assert parsed["today"]["tokens"] == 8_940_200
            assert parsed["month"]["cost"] == 136.11
            assert [row["label"] for row in parsed["tools"]] == ["codex", "claude", "hermes"]
            assert [row["pct"] for row in parsed["limits"]] == [76, 53, 18]
            assert len(parsed["trend"]) == 30 and parsed["trend"][-5]["tokens"] is None
            assert [row["stale"] for row in parsed["devices"]] == [False, True]
            assert "synthetic-private-host" not in body
            assert parsed["devices"][0]["label"] == "Device 1"
            assert len(body) < 8192

            handler.available = False
            assert get() == body  # Recent valid result is reused during the TTL.
            clock[0] += 31
            expect_error(502)
            handler.available = True
            recovered = get()
            assert json.loads(recovered)["asOf"] > parsed["asOf"]

        named_feed = Feed(source, now=lambda: clock[0], show_device_names=True)
        with running(serve(("127.0.0.1", 0), named_feed, "fixture-watch")) as relay:
            req = Request(f"http://127.0.0.1:{relay.server_port}{SNAPSHOT_PATH}",
                          headers={"Authorization": "Bearer fixture-watch"})
            with direct.open(req, timeout=3) as response:
                named = json.load(response)["devices"]
            assert [row["label"] for row in named] == ["synthetic-private-host", "Device 2"]
            assert named[0]["named"] and "named" not in named[1]
        return body, recovered


def sim_status(sim):
    line = sim.console("tm status")
    assert line.startswith("@tm "), line
    return json.loads(line[4:])


def gadget_language(sim):
    reply = sim.console("get language")
    assert reply.startswith("@value "), reply
    return json.loads(reply[7:])["value"]


def swipe_left(sim):
    sim.touch(True, 373, 230)
    sim.run_for(0.08)
    sim.touch(True, 93, 230)
    sim.run_for(0.08)
    sim.touch(False)
    sim.run_for(0.08)


def tap_at(sim, x, y):
    sim.touch(True, x, y)
    sim.run_for(0.08)
    sim.touch(False)
    sim.run_for(0.08)


def quota_split(sim, output):
    now = int(time.time())
    data = fixture(now)
    stats = data["/api/stats"]
    stats["limits"]["providers"] = [{
        "provider": "opencode",
        "windows": [
            {"kind": "session", "windowMinutes": 300, "remainingPercent": 100},
            {"kind": "weekly", "windowMinutes": 10080, "remainingPercent": 70},
            {"kind": "billing", "windowMinutes": 43200, "remainingPercent": 85},
            {"kind": "other", "remainingPercent": 99},
        ],
    }, {
        "provider": "cursor",
        "windows": [{"kind": "session", "remainingPercent": 25}],
    }, {
        "provider": "gemini",
        "stale": True,
        "windows": [{"kind": "session", "remainingPercent": 1}],
    }]
    payload = snapshot(stats, data["/api/history"], now)
    assert [(row["period"], row["pct"]) for row in payload["limits"]] == [
        ("session", 100), ("week", 70), ("month", 85), ("unknown", 99), ("session", 25), ("session", 1),
    ]
    assert sim.console("tm inject " + json.dumps(payload, separators=(",", ":"))) == "@ok snapshot"
    assert sim_status(sim)["ring_provider"] == ""

    def select_provider(y, label):
        tap_at(sim, 350, 91)
        assert sim_status(sim)["page"] == "Settings"
        tap_at(sim, 290, 310)
        assert sim_status(sim)["page"] == "Ring"
        tap_at(sim, 210, y)
        assert sim_status(sim)["page"] == "Settings"
        assert sim_status(sim)["ring_provider"] == label
        tap_at(sim, 245, 367)
        assert sim_status(sim)["page"] == "Quotas"

    select_provider(179, "Cursor")
    sim.screenshot(output / "11-ring-short.png")
    tap_at(sim, 156, 132)
    state = sim_status(sim)
    assert state["quota_view"] == "short" and state["quota_count"] == 3
    # At 9 o'clock a 25% fill reaches the perimeter, but a stale 1% fill does not.
    offset = 11 * 3
    short_color = sim.rgb888(233, 234)[offset:offset + 3]
    assert short_color[2] > short_color[0] > short_color[1] > 100  # Cursor purple
    tap_at(sim, 300, 132)
    state = sim_status(sim)
    assert state["quota_view"] == "cycle" and state["quota_count"] == 2
    assert state["ring_provider"] == "Cursor" and max(sim.rgb888(233, 234)[offset:offset + 3]) < 100
    select_provider(257, "OpenCode")
    cycle_color = sim.rgb888(233, 234)[offset:offset + 3]
    assert cycle_color[0] > cycle_color[2] > cycle_color[1] > 100  # OpenCode pink
    sim.screenshot(output / "12-ring-cycle.png")
    select_provider(140, "")
    assert sim.rgb888(233, 234)[offset:offset + 3] == b"\0\0\0"
    select_provider(257, "OpenCode")
    assert sim.console("tm inject-error") == "@ok simulated relay failure"
    assert max(sim.rgb888(233, 234)[offset:offset + 3]) < 100  # Cached data leaves an empty track.
    for item in payload["limits"]:
        item.pop("period")
    payload.pop("limitsMore")
    assert sim.console("tm inject " + json.dumps(payload, separators=(",", ":"))) == "@ok snapshot"
    assert sim_status(sim)["quota_count"] == 2


def settings_ui(sim, output):
    swipe_left(sim)
    assert sim_status(sim)["page"] == "Quotas"
    assert sim_status(sim)["language"] == "en"
    assert sim_status(sim)["interval_seconds"] == 60
    tap_at(sim, 350, 91)
    assert sim_status(sim)["page"] == "Settings"
    english = sim.screenshot(output / "8-settings-en.png").read_bytes()
    tap_at(sim, 305, 150)
    assert sim_status(sim)["language"] == "zh"
    assert gadget_language(sim) == "zh"
    chinese = sim.screenshot(output / "9-settings-zh.png").read_bytes()
    assert chinese != english
    for interval in (120, 300, 900, 30, 60, 120):
        before = sim_status(sim)["refresh_requested"]
        tap_at(sim, 302, 220)
        state = sim_status(sim)
        assert state["interval_seconds"] == interval and state["refresh_requested"] == before + 1
    for invalid in ("0", "61", "4294967296", "60s"):
        assert sim.console(f"tm interval {invalid}").startswith("@error")
    assert sim_status(sim)["interval_seconds"] == 120
    tap_at(sim, 240, 360)
    assert sim_status(sim)["page"] == "Quotas"
    sim.screenshot(output / "10-quotas-zh.png")
    tap_at(sim, 350, 91)
    before = sim_status(sim)["refresh_requested"]
    tap_at(sim, 305, 255)
    assert sim_status(sim)["page"] == "Quotas" and sim_status(sim)["refresh_requested"] == before + 1


def same_ui(sim, body, recovered, output):
    assert sim.console("tm inject " + body) == "@ok snapshot"
    labels = ("Overview", "Quotas", "Trend", "Devices")
    frames = []
    for index, name in enumerate(labels, start=1):
        state = sim_status(sim)
        assert state["page"] == name and state["data"] and state["visible"], state
        image = sim.screenshot(output / f"{index}-{name.lower()}.png")
        frames.append(image.read_bytes())
        swipe_left(sim)
    assert len(set(frames)) == len(labels)
    assert sim_status(sim)["page"] == "Overview"

    assert sim.console("tm inject {broken}") == "@error invalid snapshot"
    state = sim_status(sim)
    assert state["data"] and state["visible"]
    stale = sim.screenshot(output / "5-cached.png").read_bytes()
    assert stale not in frames
    assert sim.console("tm inject-error") == "@ok simulated relay failure"
    assert sim_status(sim)["status"] == 503 and sim_status(sim)["data"]
    assert sim.console("tm inject " + recovered) == "@ok snapshot"
    assert sim_status(sim)["status"] == 200

    assert sim.console("tm endpoint http://192.168.1.2/watch/v1/snapshot").startswith("@error")
    assert sim.console("tm lab http://203.0.113.1/watch/v1/snapshot").startswith("@error")
    assert sim.console("tm lab http://192.168.1.2/watch/v1/snapshot") == "@ok Token Monitor"
    assert sim.console("tm token local-test").startswith("@error")
    assert sim.console("tm endpoint https://relay.example/watch/v1/snapshot") == "@ok Token Monitor"
    assert sim.console("tm token local-test") == "@ok Token Monitor"
    assert sim.console("tm endpoint https://other.example/watch/v1/snapshot").startswith("@error")
    assert "local-test" not in sim.console("tm status")


def standalone(body, recovered, output):
    from hermes_gadget.sim import Simulator
    from tempfile import TemporaryDirectory

    with TemporaryDirectory(prefix="stopwatch-tests-") as directory:
        state_dir = Path(directory)
        sim = Simulator(board="sim-466x466-round", state_dir=state_dir)
        try:
            sim.start()
            assert sim.console("tm mode watch") == "@ok simulator mode"
            assert sim.wait_for(lambda: sim_status(sim)["visible"], timeout=8)
            same_ui(sim, body, recovered, output)
            before = sim_status(sim)["refresh_requested"]
            sim.tap("talk")
            sim.run_for(0.10)
            assert sim_status(sim)["refresh_requested"] == before + 1
            settings_ui(sim, output)
            quota_split(sim, output)
        finally:
            sim.close()
        restarted = Simulator(board="sim-466x466-round", state_dir=state_dir)
        try:
            restarted.start()
            assert restarted.console("tm mode watch") == "@ok simulator mode"
            state = sim_status(restarted)
            assert state["language"] == "zh" and state["interval_seconds"] == 120
            assert state["relay_configured"] and state["authenticated"]
            assert state["ring_provider"] == "OpenCode"
            assert restarted.console("tm language en") == "@ok Token Monitor"
            assert restarted.console("tm interval 60") == "@ok Token Monitor"
            state = sim_status(restarted)
            assert state["language"] == "en" and state["interval_seconds"] == 60
            assert restarted.console("tm language zh") == "@ok Token Monitor"
            assert restarted.console("tm interval 900") == "@ok Token Monitor"
            assert restarted.console("factory-reset").startswith("@ok factory-reset")
        finally:
            restarted.close()
        reset = Simulator(board="sim-466x466-round", state_dir=state_dir)
        try:
            reset.start()
            assert reset.console("tm mode watch") == "@ok simulator mode"
            state = sim_status(reset)
            assert state["language"] == "en" and state["interval_seconds"] == 60
            assert state["ring_provider"] == ""
            assert not state["relay_configured"] and not state["authenticated"]
        finally:
            reset.close()

        # Upgrade the old split settings, then persist changes through the shared setting.
        (state_dir / "nvs.json").write_text(json.dumps({"tm_lang": "zh", "language": "en"}), encoding="utf-8")
        upgraded = Simulator(board="sim-466x466-round", state_dir=state_dir)
        try:
            upgraded.start()
            assert upgraded.console("tm mode watch") == "@ok simulator mode"
            assert sim_status(upgraded)["language"] == gadget_language(upgraded) == "zh"
            assert "tm_lang" not in upgraded.storage.data
            assert upgraded.console("set language en") == "@ok language"
            assert sim_status(upgraded)["language"] == "en"
        finally:
            upgraded.close()


def combined(url, body, output, publish_reply):
    from hermes_gadget.sim import Simulator

    sim = Simulator(url=url, board="sim-466x466-round", state_dir=output / "personal-nvs")
    try:
        sim.start()
        assert sim.wait_screen("ready", timeout=12)
        assert sim.console("tm mode combined") == "@ok simulator mode"
        assert sim.console("tm inject " + body) == "@ok snapshot"
        swipe_left(sim)
        assert sim_status(sim)["page"] == "Quotas"
        sim.press("talk")
        assert sim.wait_screen("listening", timeout=5)
        assert not sim_status(sim)["visible"]
        sim.screenshot(output / "6-personal-listening.png")
        sim.run_for(0.45)
        sim.release("talk")
        assert sim.wait_screen("ready", timeout=20)
        assert sim_status(sim)["page"] == "Quotas" and sim_status(sim)["visible"]
        sim.screenshot(output / "7-personal-return.png")
        assert sim.console("set language en") == "@ok language"
        assert sim_status(sim)["language"] == "en"
        tap_at(sim, 350, 91)
        tap_at(sim, 305, 150)
        assert sim_status(sim)["language"] == gadget_language(sim) == "zh"
        sim.screenshot(output / "13-shared-language-zh.png")
        assert sim.console("tm back") == "@ok Token Monitor"
        assert sim.console("settings") == "@ok settings"
        assert sim.wait_screen("settings", timeout=4)
        sim.screenshot(output / "14-hermes-settings-zh.png")
        # In the round simulator, Language is five offered rows before Volume.
        for _ in range(5):
            sim.tap("up")
            sim.run_for(0.08)
        sim.run_for(0.30)
        sim.screenshot(output / "15-hermes-language-zh.png")
        sim.tap("talk")
        assert sim_status(sim)["language"] == gadget_language(sim) == "en"
        sim.tap("talk")
        assert sim_status(sim)["language"] == gadget_language(sim) == "zh"
        assert sim.console("settings close") == "@ok settings closed"
        chinese = (
            "腕表已连上本地网关。\n"
            "这是一条中文回复。用量监控会保留你选择的额度页，向上滑动可以查看后续内容。\n"
            "圆屏会自动换行，数字和中文标点也可以一起显示。今天的示例额度是 70%。\n"
            "这是继续下滑后看到的内容。屏幕上会保留前后句的顺序，方便在腕表上读完一段回复。"
        )
        publish_reply(chinese)
        assert sim.wait_for(lambda: (sim.last_received("reply") or {}).get("text") == chinese, timeout=6)
        assert sim.wait_screen("responding", timeout=4)
        assert not sim_status(sim)["visible"]
        first_frame = sim.screenshot(output / "16-hermes-reply-zh.png").read_bytes()
        sim.touch(True, 235, 341)
        sim.run_for(0.08)
        sim.touch(True, 235, 142)
        sim.run_for(0.08)
        sim.touch(False)
        sim.run_for(0.08)
        assert sim.screenshot(output / "17-hermes-reply-scroll-zh.png").read_bytes() != first_frame
    finally:
        sim.close()


async def gateway(body, output):
    from hermes_gadget.devserver import serve as devserver
    from hermes_gadget_plugin import textfmt

    loop = asyncio.get_running_loop()
    ready = loop.create_future()
    task = asyncio.create_task(devserver(
        "127.0.0.1", 0, "/gadget", output / "gateway",
        require_pairing=False, loopback=True, token=None,
        interactive=False, ready=ready,
    ))
    try:
        hub = await asyncio.wait_for(ready, 6)

        async def send(text):
            session, = hub.sessions.values()
            assert session.charset == "cjk"
            shaped = textfmt.for_device(text, session.charset)
            assert shaped == text
            turn = "local-chinese-preview"
            await session.turn_start(turn)
            await session.send_reply(shaped, turn=turn)

        def publish_reply(text):
            asyncio.run_coroutine_threadsafe(send(text), loop).result(timeout=6)

        await asyncio.to_thread(combined, f"ws://127.0.0.1:{hub.bound_port}/gadget", body, output, publish_reply)
    finally:
        task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await task


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/sim-check"))
    parser.add_argument("--relay-only", action="store_true")
    args = parser.parse_args()
    # Keep localhost requests out of any desktop proxy.
    for env in ("NO_PROXY", "no_proxy"):
        os.environ[env] = ",".join(
            val for val in (os.environ.get(env), "127.0.0.1", "localhost") if val
        )
    body, recovered = relay_checks()
    print("PASS: local Hub API -> relay auth, snapshot, outage, recovery", flush=True)
    if not args.relay_only:
        output = args.output.resolve()
        output.mkdir(parents=True, exist_ok=True)
        standalone(body, recovered, output)
        print("PASS: standalone round screen, gestures, controls and cached data", flush=True)
        asyncio.run(gateway(body, output))
        print("PASS: personal round screen, shared locale, CJK reply and return to the same page", flush=True)
        print(f"Screenshots: {output}")


if __name__ == "__main__":
    main()
