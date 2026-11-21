"""Synthetic Hub v0.92.0 fields. Contains no personal telemetry."""

from datetime import datetime, timedelta, timezone


def fixture(now):
    stamp = datetime.fromtimestamp(now, timezone.utc)
    day_rows = []
    for days in range(29, -1, -1):
        if days == 4:
            continue  # Unknown day must remain a gap.
        tokens = 800_000 + ((days * 482_702) % 3_170_000)
        day_rows.append(
            {
                "date": (stamp.date() - timedelta(days=days)).isoformat(),
                "tokens": tokens,
                "cost": round(tokens / 720_000, 2),
            }
        )
    return {
        "/api/stats": {
            "periods": {
                "today": {
                    "totalTokens": 8_940_200,
                    "costUsd": 4.82,
                    "clients": {"codex": 6_900_000, "claude": 1_600_000, "hermes": 440_200},
                    "models": {"model-a": 6_900_000, "model-b": 2_040_200},
                },
                "month": {"totalTokens": 274_100_000, "costUsd": 136.11},
            },
            "limits": {
                "providers": [
                    {
                        "provider": "Codex",
                        "status": "ok",
                        "updatedAt": stamp.isoformat(),
                        "windows": [
                            {"kind": "5h", "remainingPercent": 76,
                             "resetsAt": (stamp + timedelta(minutes=84)).isoformat()},
                            {"kind": "weekly", "usedPercent": 47,
                             "resetsAt": (stamp + timedelta(days=2)).isoformat()},
                        ],
                    },
                    {
                        "provider": "Claude",
                        "status": "ok",
                        "updatedAt": stamp.isoformat(),
                        "windows": [{"kind": "5h", "remainingPercent": 18}],
                    },
                ]
            },
            "staleAfterMs": 300_000,
            "devices": [
                {
                    "hostname": "synthetic-private-host",
                    "receivedAt": stamp.isoformat(),
                    "periods": {"today": {"totalTokens": 7_051_300}},
                },
                {
                    "receivedAt": (stamp - timedelta(hours=2)).isoformat(),
                    "periods": {"today": {"totalTokens": 1_888_900}},
                },
            ],
        },
        "/api/history": {"daily": day_rows},
    }
