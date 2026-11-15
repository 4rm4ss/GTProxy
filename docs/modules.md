# Modules and profiles

Modules add features to the proxy without rebuilding it. Each is a folder of
Lua code; its settings are JSON files you edit, and profiles switch groups of
settings at once. Both folders sit **next to `Proxy.exe`**, whatever folder
the proxy is started from:

```
Proxy.exe
modules/
  reachability/
    module.json
    main.lua
config/
  modules.json
  modules/
    reachability.json
  profiles/
    default.json
    test.json
```

The Lua scripts in `scripts/` keep working as before; modules are separate
from them (each module runs in its own Lua state and only sees the module API
below).

## Installing a module

1. Copy the module's folder into `modules/` (for example
   `modules/whereami/` with its `module.json` and `main.lua`).
2. Restart the proxy. The log says `Module 'whereami' 1.0.0 loaded ...`, or
   why it was not loaded.
3. Optional: put its settings in `config/modules/<id>.json` and switch it off
   or on in `config/modules.json`.

A module that is not listed in `config/modules.json` is on. A broken module
never stops the others: the log names the folder and the reason, and the rest
load as usual.

## module.json

```json
{
  "id": "reachability",
  "version": "1.0.0",
  "api_version": 1,
  "main": "main.lua",
  "name": "Reachability check",
  "description": "...",
  "author": "...",
  "settings": { "show_marker": true, "show_steps": true, "log_result": true }
}
```

| Key | |
|-----|-|
| `id` | Unique name: `a-z`, `0-9`, `_`, `-`. Names its settings file `config/modules/<id>.json`. |
| `version` | The module's own version, shown in the log. |
| `api_version` | Module API it is written for. This proxy offers **1**; a module asking for another version is not loaded (the log says so). |
| `main` | Lua file that is run, inside the module's folder. |
| `settings` | Default settings. Each value is `true`/`false`, a number or text, and fixes that setting's type. |
| `name`, `description`, `author` | Optional. |

Unknown keys are reported as errors (they are usually typos). Folders are
loaded in name order; a second folder with an `id` that is already taken is
not loaded.

## Settings and profiles

| File | |
|------|-|
| `config/modules.json` | Which modules are on, and the profile used at start: `{ "profile": "default", "modules": { "reachability": { "enabled": true } } }` |
| `config/modules/<id>.json` | Settings of one module: `{ "show_steps": false }` |
| `config/profiles/<name>.json` | Overrides: `{ "description": "...", "modules": { "reachability": { "show_marker": false } } }` |

A module's settings are always worked out from scratch, in this order (later
wins): its defaults in `module.json` → `config/modules/<id>.json` → the active
profile. Switching from one profile to another therefore never leaves values
of the first one behind. Every setting in a file must exist in the module's
`module.json` and have the same type.

Commands in the proxy's console window (type them and press Enter):

| Command | |
|---------|-|
| `cfg list` | The profiles in `config/profiles`, the active one marked with `*` |
| `cfg exec <name>` | Apply `config/profiles/<name>.json` now |
| `cfg reload` | Read every file in `config/` again for the active profile (also switches modules on/off as `config/modules.json` says) |
| `help` | All console commands, including those of modules |

Nothing needs a restart. If a file is broken JSON, or a value has the wrong
type, the log shows the file, the place and the reason, and **the settings in
force stay as they were**. `cfg exec` does not change `config/modules.json`:
the next start uses the profile named there.

## Module API version 1

Everything a module can use is in the global table `gtproxy`. Modules have the
Lua standard libraries `string`, `table`, `math`, `utf8`, `coroutine` and the
base functions (without `dofile`/`loadfile`); `print` writes to the proxy log.

### Events

```lua
gtproxy.on("right_click", function(e)
    -- e.x, e.y: the tile right-clicked in the map window (also outside the world)
    -- e.in_world: whether you are in a world
end)
```

`right_click` is the only event of API 1. If no module handles it, the map
window says so.

### Commands

```lua
gtproxy.command("reach", "Can you get to a tile: reach <x> <y>", function(args, ctx)
    -- args: the words after the command (strings)
    -- ctx.source: "game" (/reach in the game chat) or "console" (proxy console)
    ctx.reply("text")   -- game chat or proxy log, where the command was typed
end)
```

One name gives both `/name` in the game and `name` in the proxy console.
Names are `a-z`, `0-9`, `_`; the proxy's own commands and those of other
modules cannot be taken (the log says so and the command is not added). The
game's font has no Cyrillic: answer the game in English.

### Settings

```lua
gtproxy.setting("show_steps")   --> true / false / number / text
```

Always the value in force (defaults → `config/modules/<id>.json` → profile);
after `cfg exec` the next call returns the new value. Asking for a setting the
module does not declare is an error.

### World

```lua
local w = gtproxy.world.info()
-- w.in_world, w.name, w.width, w.height (tiles)
-- w.me: { x, y (world pixels), tile_x, tile_y } or nil while unknown
```

### Reachability

```lua
local r = gtproxy.reach(x, y)
```

The proxy's check: a search over the four neighbours of each tile from the
tile you stand in (never diagonally between two blocks), your collision box
must fit in every tile on the way. It only answers; nothing is sent.

| Field | |
|-------|-|
| `status` | `"reachable"`, `"unreachable"`, `"blocked"` (block in the tile), `"out_of_bounds"`, `"no_data"` (no world, items.dat or position yet) |
| `headline` | `"Цель доступна"`, `"Цель недоступна"` or `"Нет данных"` |
| `reason`, `message` | `"в клетке блок"` / `"за пределами карты"`; message = headline plus reason |
| `steps`, `steps_word` | Length of the shortest way, `"шаг"`/`"шага"`/`"шагов"` |
| `start_x`, `start_y` | Your tile (nil when unknown) |
| `x`, `y` | The tile asked about |
| `missing` | For `no_data`: what is not known yet |
| `tile_text` | What is on the tile, e.g. `"Rock (10) on Cave Background (14) - solid."` |
| `summary` | One line for the log with your position, the tile and the result |

### Map window

```lua
gtproxy.show({
    title = "Цель доступна",
    text = "От 5, 4 до 0, 4: 5 шагов.",
    kind = "ok",        -- "ok" (green), "bad" (red), "none" (grey)
    marker = true,      -- mark the tile
    x = 0, y = 4,       -- optional, the clicked tile by default
    status = "reachable",   -- optional tag for tools
})
```

The answer to the right click being handled; returns false (and shows
nothing) outside a `right_click` handler.

### Log

```lua
gtproxy.log.info("text")    -- also .warn and .error; lines start with [<id>]
```

### Limits

- A handler or command that runs longer than 2 seconds is stopped; the log
  says which module.
- An error in a module's code is logged with the file and line; other
  modules and handlers keep working.
- `gtproxy.module` holds `id`, `version` and `name` of the module itself;
  `gtproxy.api_version` is 1.

## The modules shipped with the proxy

- `modules/reachability`: right click in the map window, `/reach X Y` in the
  game, `reach X Y` in the console. Settings: `show_marker`, `show_steps`,
  `log_result`.
- `examples/modules/whereami` (not installed): a short example to copy into
  `modules/`.
