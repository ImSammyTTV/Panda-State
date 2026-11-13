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
| `source` | A live meter from a web address: `{"url", "path", "min", "max", "colour", "every"}` |
| `filament` | The colour of the filament the printer is using: `{"effect": "solid", "speed": 1-100}`. Between prints, the last one used, breathing slowly. Needs firmware 0.7.3 or later and a Bambu printer |

Effects: solid, breathe, fill, flash, chase, rainbow, gradient, waves, scroll,
theatre, scanner, meteor, fire, twinkle, sparkle, rain, aurora.

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

## Adding one to the store

1. Add your `.json` file to this folder.
2. Add an entry for it to `index.json`: `file`, `name`, `description`,
   `author`, `kind` (`look`, `animation` or `live meter`), `preview` (25
   colours as 150 hex digits for the little strip on the store card) and, for
   an add-on that needs a location, `"needs": "place"`.
3. Open a pull request.

Add-ons run on the bar itself, so keep them small and kind to the services they
read: no faster than every 30 seconds, and only public data.
