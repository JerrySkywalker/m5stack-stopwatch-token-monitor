# Watch snapshot v1

`GET /watch/v1/snapshot` returns one JSON object (maximum 8192 bytes). The
device sends an Authorization Bearer header only over HTTPS. Browser and
device traffic do not need to access the Hub's larger response structure.

| Field | Meaning |
| --- | --- |
| `version` | 1 |
| `asOf` | Unix epoch seconds of the adapter's successful snapshot |
| `today`, `month` | Objects with `tokens` and USD `cost` |
| `tools`, `models` | Up to four rows of `label`, `tokens`, `cost` ranked by today's usage |
| `limits` | Up to 12 rows of `label`, remaining `pct`, epoch-seconds `reset`, `stale` |
| `limitsMore` | Optional extension of up to 12 additional quota rows |
| `trend` | Up to 30 ascending daily points with `day` (MM-DD), `tokens`, `cost` |
| `devices` | Up to 12 rows of `label`, today's `tokens`, epoch-seconds `updated`, `stale` |

An unavailable reading is `null`, not zero. Unavailable trends remain blank;
individual missing days are preserved as gaps. The adapter aliases devices
and accounts. The watch keeps the previous valid snapshot after transport or
schema errors and labels it cached until the next successful refresh.
History is cached by the adapter for five minutes and uses UTC dates.

Quota rows may also report `period` and `minutes`. The short-term tab shows
`session` and `day` windows. The cycle tab shows `week`, `month`, `billing`
and other known longer `cycle` windows. These are separate provider-reported
percentages. Legacy snapshots can be classified from explicit period labels.
Windows with an unknown period stay out of both tabs.
