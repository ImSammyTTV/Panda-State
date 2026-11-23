# The add-on store

Everything in this folder appears in the **Store** on the bar's own page
(firmware 0.6.0 and later). The page reads `index.json` from here; when you
pick an add-on, the bar is given that file's address, fetches it from GitHub
each time it starts, and keeps nothing else.

You can also **upload** a file like these from the page (up to 4,000
characters), or post to the bar from a program of your own (see the main
README's API section).

## An add-on file

```json
{
  "id": "aurora",
  "name": "Aurora",
  "description": "Greens and violets drifting slowly along the bar.",
  "author": "you",
  "look": { "colour": "#22C55E", "colour2": "#8B5CF6", "effect": "aurora", "speed": 30 }
}
```

- `id`: 1-15 characters, `a-z`, `0-9`, `-` and `_`.
- `name`, `description`, `author`: shown on the page.
- Then **one** of these:

| Key | What it shows |
|---|---|
| `look` | One of the bar's effects: `{"colour", "colour2", "effect", "speed" 1-100, "progress" 0-100}` |
| `meters` | One to four bars sharing the strip: `[{"value": 0-100, "colour": "#RRGGBB"}]` |
| `frame` | All 25 LEDs: a list of 25 `#RRGGBB`, or one string of 150 hex digits |
| `frames` | An animation: `{"fps": 1-20, "list": ["150 hex digits", …]}`, up to 30 frames, looped |
| `source` | A live meter from a web address: `{"url", "path", "min", "max", "colour", "every"}`. With `"base"`, a second path, the meter shows the percentage change from `base` to `path` (base needs firmware 0.7.7 or later) |
| `countdown` | A meter that empties from `from` to `to`, then shows its `done` effect for `doneHours`: `{"from": "YYYY-MM-DDTHH:MM", "to": "...", "colour": "#RRGGBB", "done": "rainbow", "doneHours": 0-168}`. Times are local, in the bar's own time zone. Needs firmware 0.7.7 or later |
| `filament` | The colour of the filament the printer is using: `{"effect": "solid", "speed": 1-100}`. Between prints, the last one used, breathing slowly. Needs firmware 0.7.3 or later and a Bambu printer |
| `printer` | Something the printer reports, as a meter: `{"show": "remain", "colour": "#RRGGBB", "min": 0, "max": 100}`. `show` is `remain` (left on the loaded spool, in its own colour unless `colour` is given), `humidity` (how damp the AMS is: green, amber or red unless `colour` is given; `min`/`max` are the humidity percentages it spans, 10 and 50 by default), `timeleft` (drains as the print's time estimate counts down), `heating` (fills as the nozzle and bed reach temperature) or `chamber` (fills from `min` to `max` °C, 20 and 45 by default). `timeleft`, `heating` and `chamber` show during prints whatever "Show it" is set to. Needs firmware 0.7.16 or later and the bar's own printer link; `remain`, `humidity` and `chamber` are Bambu only |
| `maintenance` | A reminder after so many hours of printing: `{"hours": 1-10000, "task": "…", "colour": "#RRGGBB"}`. Once due, the bar breathes in the colour until **Done** is pressed on the page. Needs firmware 0.7.16 or later |

Effects: solid, breathe, fill, flash, chase, rainbow, gradient, waves, scroll,
theatre, scanner, meteor, fire, twinkle, sparkle, rain, aurora, and from
firmware 0.7.15 heartbeat, strobe, candle, ripple and stack, and from 0.7.21
fade, plasma, lava, juggle, bounce, dissolve, fireworks, lightning, highlight and drip.

### Live meters (`source`)

The bar fetches `url` every `every` seconds (30 to 86,400), finds a number in
the JSON answer by `path`, and shows it as a meter from `min` (empty) to `max`
(full).

- `path` is dot-separated keys and list positions, e.g. `data.0.price`. Leave
  it out if the answer is just a number.
- HTTPS addresses must use a certificate from one of the common public
  authorities (Let's Encrypt, DigiCert, Google, Amazon, Sectigo, GlobalSign
  and the like); anything else is refused.
- The answer must be under 32 KB, and no sign-in or API key is possible, so use
  public, key-free addresses.

### Add-ons for a place

Weather and air quality depend on where the bar is. In a store add-on, write
`{lat}`, `{lon}` and `{place}` where the latitude, longitude and town's name
belong, and give its `index.json` entry `"needs": "place"`. The bar's page then
shows **Use for my town**: it asks for the town, finds it with Open-Meteo's
free place search, and gives the bar a copy with the place filled in.

### Add-ons that ask a question

Two more `needs` work the same way:

- `"needs": "symbol"` shows **Choose a stock**: it asks for a stock code and
  fills in `{symbol}`.
- `"needs": "countdown"` shows **Set the time**: it asks what the countdown is
  for and when it ends, and fills in `{label}`, `{from}` (now) and `{to}`.

## Adding one to the store

1. Add your `.json` file to this folder.
2. Add an entry for it to `index.json`: `file`, `name`, `description`,
   `author`, `kind` (`look`, `animation`, `live meter`, `countdown`,
   `printer` or `maintenance`),
   `preview` (25 colours as 150 hex digits for the little strip on the store
   card) and, for an add-on that asks for something, its `needs`.
3. Open a pull request.

Add-ons run on the bar itself, so keep them small and kind to the services they
read: no faster than every 30 seconds, and only public data.
