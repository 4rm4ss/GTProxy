# Modules and profiles

The proxy itself is an **empty core**: it forwards packets between the game
and the server, keeps track of where you are (world, position, inventory,
players) and offers that to modules. Every feature — drop tools, warping,
nick/netIDs, info commands, walking, the reachability check — is a module on
top of it. Without `modules/` the proxy only forwards.

Each module is a folder of Lua code; its settings are JSON files you edit, and
profiles switch groups of settings at once. All of it sits **next to
`Proxy.exe`**, whatever folder the proxy is started from:

```
Proxy.exe
modules/              the features, one folder each
  camera/  drop/  info/  looks/  reachability/  walk/  worlds/
  vend_finder/  item_finder/  world_scan/  world_moderation/  autohost/
config/               settings you edit
  modules.json
  modules/<id>.json
  profiles/default.json, test.json
data/                 what modules remember (written by the proxy)
  drop.json ...
```

The Lua scripts in `scripts/` keep working as before; modules are separate
from them (each module runs in its own Lua state and only sees the module API
below).

## Installing a module

1. Copy the module's folder into `modules/` (for example
   `modules/whereami/` with its `module.json` and `main.lua`).
2. Restart the proxy. The log says `Module 'whereami' 1.0.0 loaded ...` with
   its events and commands, or why it was not loaded.
3. Optional: put its settings in `config/modules/<id>.json` and switch it off
   or on in `config/modules.json`.

A module that is not listed in `config/modules.json` is on. A broken module
never stops the others: the log names the folder and the reason, and the rest
load as usual. To remove a feature, switch its module off (`"enabled": false`
and `cfg reload`) or delete its folder.

## module.json

```json
{
  "id": "drop",
  "version": "1.0.0",
  "api_version": 2,
  "main": "main.lua",
  "name": "Items and drop",
  "description": "...",
  "author": "...",
  "settings": { "dialog_timeout_ms": 2000, "ack_timeout_ms": 1500 }
}
```

| Key | |
|-----|-|
| `id` | Unique name: `a-z`, `0-9`, `_`, `-`. Names its settings file `config/modules/<id>.json` and its data file `data/<id>.json`. |
| `version` | The module's own version, shown in the log. |
| `api_version` | Module API it is written for. This proxy offers **1 and 2** (2 only adds to 1); a module asking for another version is not loaded (the log says so). |
| `main` | Lua file that is run, inside the module's folder. |
| `settings` | Default settings. Each value is `true`/`false`, a number or text, and fixes that setting's type. |
| `name`, `description`, `author` | Optional. |

Unknown keys are reported as errors (they are usually typos). Folders are
loaded in name order; a second folder with an `id` that is already taken is
not loaded.

## Settings and profiles

| File | |
|------|-|
| `config/modules.json` | Which modules are on, and the profile used at start: `{ "profile": "default", "modules": { "drop": { "enabled": true } } }` |
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

Settings are what you configure; what a module changes while you play (fast
drop on/off, the keep list ...) it keeps in `data/<id>.json` itself
(`gtproxy.store`).

## The module API

Everything a module can use is in the global table `gtproxy`. Modules have the
Lua standard libraries `string`, `table`, `math`, `utf8`, `coroutine` and the
base functions (without `dofile`/`loadfile`); `print` writes to the proxy log.
`gtproxy.api_version` is the proxy's API version (3); `gtproxy.module` holds
`id`, `version` and `name` of the module itself; `gtproxy.prefix` is the chat
command prefix (`"/"`).

Version 1 is marked **(1)** below, version 3 **(3)**; everything else is
version 2. A module needing version 3 says `"api_version": 3` in its
module.json.

### Events

```lua
gtproxy.on("right_click", function(e)
    -- e.x, e.y: the tile right-clicked in the map window (also outside the world)
    -- e.in_world: whether you are in a world
end)
```

| Event | Fields | When |
|-------|--------|------|
| `right_click` **(1)** | `x`, `y`, `in_world` | Right click in the map window. Answer with `gtproxy.show`. |
| `left_click` | `x`, `y`, `in_world` | Left click on a tile of the map window. Answer with `gtproxy.show`. |
| `tile_click` | `x`, `y`, `item` | The game punches / builds on a tile. Return `"cancel"` and the server never gets it. |
| `dialog` | `name`, `button`, `values`, `frame` | A button of a proxy dialog (`gtproxy_*`). `values`: the dialog's inputs and checkboxes (`"1"`/`"0"`) by name. `frame`: the button belongs to the menu frame (`page_main`, `commands`, `map`), which answers it after the modules — apply your page's settings, open nothing. |
| `world_enter` | `name`, `previous` | The world's data arrived. |
| `world_menu` | | The server shows the world menu (you left the world). |
| `disconnect` | | The game or the server disconnected. |
| `inventory` | `full`, `item`, `removed`, `added`, `count` | The whole inventory arrived (`full`), or one item changed (`count`: how many you have now). |
| `object` | `change`, `kind`, `item`, `amount`, `old_amount`, `object_id`, `known`, `x`, `y`, `by_me`, `by_net_id`, `by_name` | A dropped object (after the world has it): `kind` `"dropped"` (change -1), `"changed"` (-3), `"collected"` (change = the netID of the player who took it; `by_name` only when that player is known), `"removed"`. `x`, `y`: its tile. `known`: what it was is known (else item/amount are guesses). |
| `walk_end` | `message`, `notify` | A walk ended. `notify`: arrived, stopped by the server or a blocked way (not when you left the world or pressed Stop). |
| `settings` | | The settings in force changed (`cfg reload`, `cfg exec`, or a page saved one): read them again with `gtproxy.setting`. |
| `wheel` | `notches` | The mouse wheel turned over the game's window while the module holds it (`gtproxy.input.capture_wheel`): `+` away from you, `-` towards you. |
| `tile` **(3)** | `x`, `y`, `fg`, `bg`, `old_fg`, `old_bg`, `cause` | The server changed a tile (after the world has it): `cause` `"placed"`, `"broken"`, `"changed"` or `"updated"` (same blocks, new data: a lock's access, a machine's price). |
| `player_join` **(3)** | `net_id`, `user_id`, `name`, `clean`, `country`, `is_local`, `invisible` | A player appeared in the world (`clean`: the name without colours). |
| `player_leave` **(3)** | `net_id`, `user_id`, `name`, `clean` | A player left the world. |
| `console` **(3)** | `kind`, `text`, `clean`, `net_id` | The server showed a text: `kind` `OnConsoleMessage`, `OnTalkBubble` or `OnTextOverlay`. |
| `pick` **(3)** | `x`, `y`, `in_world` | The tile clicked after `gtproxy.map.pick` (only to the module that asked). Return `{ ok = , message = }` or a text for the window. |
| `panel` **(3)** | `panel`, `action`, `row`, `value`, `values` | A button of the module's own tab in the map window (only to that module). Return `{ ok = , message = }` or a text. |

If no module handles a map window click, the window says so. A handler that
fails is logged and does not stop the others.

### Changing what the server tells the game

```lua
gtproxy.hook_call("OnNameChanged", function(call)
    -- call.name, call.net_id, call.args (the arguments after the name)
    call.args[1] = call.args[1] .. " `4[" .. call.net_id .. "]``"
    return "replace"        -- the game gets the changed call
end)
```

Hookable calls: `OnSpawn`, `OnRemove`, `OnNameChanged`, `OnDialogRequest`,
`OnSetPos`, `OnRequestWorldSelectMenu`. Return nothing to let the call through
unchanged, `"replace"` to send your changed `call.args` / `call.net_id`, or
`"cancel"` to keep it from the game (later hooks do not see it). Numbers keep
the type the server sent; vectors are `{ x =, y = }`. The proxy's own state
(world, players, position) is already updated when hooks run.

### Commands

```lua
gtproxy.command("reach", "Can you get to a tile: reach <x> <y>", function(args, ctx)
    -- args: the words after the command (strings)
    -- ctx.source: "game" (/reach in the game chat) or "console" (proxy console)
    ctx.reply("text")   -- game chat or proxy log, where the command was typed
end)
```

**(1)** One name gives both `/name` in the game and `name` in the proxy
console; the game sends `/Name` as well. Names are `a-z`, `0-9`, `_`; the
proxy's own commands (`/proxy`, `/proxyhelp`, `/map`) and those of scripts
and other modules cannot be taken (the log says so, the command is not added
and `gtproxy.command` returns false). The game's font has no Cyrillic: answer
the game in English.

### Settings **(1)**

```lua
gtproxy.setting("show_steps")   --> true / false / number / text
```

Always the value in force (defaults → `config/modules/<id>.json` → profile);
after `cfg exec` the next call returns the new value. Asking for a setting the
module does not declare is an error.

```lua
local ok, why = gtproxy.save_setting("speed", 900)
```

A module's page changes a setting: it is written to `config/modules/<id>.json`
(other keys kept) exactly like an edit by hand, and is in force at once - so
the page and the file never disagree. The value must have the setting's type.
`ok` is false, with the reason, when the file cannot be written, another config
file is broken, or the active profile sets that key (then the profile's value
stays in force). The `settings` event follows.

### World, players, items

```lua
local w = gtproxy.world.info()
-- (1) w.in_world, w.name, w.width, w.height (tiles)
-- (1) w.me: { x, y (world pixels), tile_x, tile_y } or nil while unknown
-- w.connected, w.previous, w.recent (worlds entered, newest first),
-- w.net_id (yours, -1 outside a world), w.real_name

-- (3) w.server (the game server of config.json), w.user_id (your account ID)

gtproxy.world.players()   -- { { net_id, user_id, name, country, invisible, is_local, x, y, tile_x, tile_y } ... }
gtproxy.world.objects()   -- dropped items: { { object_id, item, amount, x, y } ... } (x, y in pixels; tile = floor((x + 8) / 32))
gtproxy.world.tile(x, y)  -- { fg, bg, flags, cell ("free", "solid", "platform", "dangerous", "door"), text } or nil
                          -- (3) also extra ("vending", "lock", "door", "sign" or ""), label (door / sign text),
                          --     vend_item, vend_price (vending), lock_owner, lock_access (lock: user IDs)
gtproxy.world.vendings()  -- (3) { { x, y, machine, item, price } ... }  price > 0: WL each, < 0: items per WL, 0: none
gtproxy.world.locks()     -- (3) { { x, y, item, owner, access = { user IDs }, world (true: a world lock) } ... }
gtproxy.world.summary()   -- (3) { foreground = { [id] = count }, background = { [id] = count } }
gtproxy.world.find_tiles(id)   -- (3) { { x, y, background } ... } every tile with the item

gtproxy.inventory.info()  -- { known, slots, items = { { id, count, flags } ... } }  (flags 0x01 = worn)
gtproxy.inventory.count(id)

gtproxy.items.loaded()            -- items.dat received?
gtproxy.items.get(id)             -- { id, name, type, rarity, max_amount, untradeable } or nil
gtproxy.items.find("lock", 12)    -- items whose name contains the text
```

### Talking to the game and the server

```lua
gtproxy.game.message("`oHello")               -- a line in the game's console
gtproxy.game.dialog(markup)                   -- open a dialog (OnDialogRequest markup)
gtproxy.game.call("OnChangeSkin", net_id, gtproxy.u32(0xFF0000FF))   -- any call to the game
gtproxy.server.text("action|drop\n|itemID|2\n")                       -- as the game sends it (generic text)
gtproxy.server.game_text("action|join_request\nname|START\ninvitedWorld|0")   -- game message type
gtproxy.server.tile_action(x, y, item)        -- (3) using an item on a tile, as the game does (TILE_CHANGE_REQUEST)
gtproxy.server.wrench(x, y)                   -- (3) the wrench on a tile: the server answers with the machine's / lock's window
```

A server's window (OnDialogRequest) can be read and answered like the game
does **(3)**:

```lua
gtproxy.hook_call("OnDialogRequest", function(call)
    local d = gtproxy.ui.parse_dialog(call.args[1])
    -- d.name, d.embed (embed_data by name), d.inputs, d.checkboxes, d.buttons, d.pickers,
    -- d.text (all text without colours), d.icons, d.input_list / checkbox_list / button_list (in order)
    if d.name == "vending" then
        gtproxy.server.text(gtproxy.ui.answer(d, { buycount = 4 }))   -- embed data + inputs + checkboxes, button nil = OK
        return "cancel"                                               -- the game does not show it
    end
end)
```

`game.call` arguments: text, numbers (whole numbers are sent signed, others
as floats), `gtproxy.u32(n)` / `gtproxy.i32(n)` / `gtproxy.f32(n)` for a
given type, `gtproxy.vec2(x, y)`. `server.*` returns false when not connected
to the game server. Only `server.*` reaches the server; everything else only
changes what your own game shows.

Dialogs are easiest with the builder (the markup the game understands):

```lua
local markup = gtproxy.dialog()
    :header("Items", 448)                 -- big title with an item icon
    :small("`oBackpack: 12 items")        -- also :text, :item(text, icon), :section(title, icon)
    :input("amount", "Amount:", "", 5)    -- name, label, value, max length
    :checkbox("fast", "Fast drop", true)
    :button("go", "Go")
    :spacer()
    :button("page_main", "<< Menu")
    :finish("gtproxy_items", "Apply")     -- dialog name (gtproxy_*), OK button (optional)
gtproxy.game.dialog(markup)
```

Name proxy dialogs `gtproxy_<something>`: the proxy keeps them from the
server and passes their buttons to the modules (`dialog` event). Button names
are shared by all modules, so make them distinct.

### The /proxy menu

```lua
gtproxy.menu.add({
    title = "Items", icon = 448, order = 20,
    button = "page_items", label = "Drop all, locks, fast drop",
    status = function() return "`oDropping..." end,   -- optional: line(s) on the first page
})
```

The menu frame (where you are, the map window, all commands) belongs to the
core; each module adds a section with a button that opens its own page (the
module answers the button in its `dialog` handler).

### Walking

```lua
local walk = gtproxy.walk.to(x, y)   -- { ok, message, x, y }; a block: walks next to it
gtproxy.walk.to(x, y, { gravity = true })   -- (3) this walk on foot (or false: free), whatever the setting
gtproxy.walk.stop()
gtproxy.walk.active()
gtproxy.walk.options()               -- { speed, step_ms, gravity, jump_height, air_jumps, doors }
gtproxy.walk.options({ speed = 900, air_jumps = 10 })   -- changes those given, returns all
```

The proxy's walking engine (a path over free tiles, sent as the game's own
movement). `options`: `speed` px/s (16-10000; the game runs ~250), `step_ms`
between positions (10-1000), `gravity` (only paths you could run and jump
along), `jump_height` tiles per jump (1-200), `air_jumps` before landing
(1-1000; each adds `jump_height`), `doors`: `"access"` - doors and entrances
(House Entrance, Jail Door, VIP Entrance ...) open to you: public, in a world
without a lock, or under a lock you own / are on the access list of; `"all"`
- every one; `"none"` - doors only as a destination. A door with a
destination takes you there when you step in; the main door (EXIT) is only
walked to. When there is no way because of a door, the walk's message names
it and why it counts as closed. Out of range values are clamped. `config.json`
`"map"` sets them at start; the walk module sets them from its settings.

A Growtopia server decides what it accepts: an official one may stop or
reject fast walks. The `walk_end` event tells how a walk ended.

### Mouse wheel

```lua
local available = gtproxy.input.capture_wheel("always")   -- or "ctrl", "shift", "alt", "off"
gtproxy.on("wheel", function(e) ... e.notches ... end)
```

While a module holds it, the wheel turned over Growtopia's window (the window
of `Growtopia.exe`; with `"ctrl"` etc. only while that key is held) never
reaches the game: the module gets `wheel` events instead. Everything else
keeps its wheel. Only on Windows (`available` is false elsewhere); the proxy
sets the hook only while a module holds the wheel.

### Reachability **(1)**

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

### Map window **(1)**

```lua
gtproxy.show({
    title = "Цель доступна",
    text = "От 5, 4 до 0, 4: 5 шагов.",
    kind = "ok",        -- "ok" (green), "bad" (red), "none" (grey)
    marker = true,      -- mark the tile
    x = 0, y = 4,       -- optional, the clicked tile by default
    status = "reachable",   -- optional tag for tools
    toast = false,      -- (2) a short note at the bottom instead of a card
})
```

The answer to the click being handled; returns false (and shows nothing)
outside a `right_click` / `left_click` handler.

#### Marks, ways, tabs, PNG **(3)**

```lua
gtproxy.map.overlay("target", {                 -- what the module draws on the map (nil removes it)
    markers = { { x = 2, y = 8, label = "3 for 1 WL", color = "#ffcf4a" } },
    route = { { x = 4, y = 8 }, { x = 3, y = 8 } }, route_color = "#c77dff",
    areas = { { x1 = 3, y1 = 7, x2 = 5, y2 = 9, label = "point 1", color = "#4fc3ff" } },
})
local way = gtproxy.map.route(x, y)              -- { ok, tiles = { {x, y} ... }, steps, reason }: the shortest way you can move
local foot = gtproxy.map.route(x, y, { walk = true, gravity = true })   -- the walk on foot (standing, jumping, falling)
local steps = gtproxy.map.distances({ { x = 1, y = 2 }, ... })          -- steps to each (-1: no way), one search for all
gtproxy.map.pick("label")                        -- the next left click in the map window is this module's (event pick)
local path, why = gtproxy.map.render()           -- PNG of the world with all marks: data/renders/<WORLD>_<date>.png

gtproxy.panel.set("vend", {                      -- the module's tab in the map window ("Modules" button)
    title = "Вендинги", note = "text\nlines",
    search = { value = "", placeholder = "name or ID" },                 -- action "search", value = the text
    inputs = { { id = "qty", label = "Amount", value = "1", size = 6 } },  -- sent with every button as values.qty
    buttons = { { id = "buy", label = "Buy" }, { id = "preset", label = "10", value = "10" } },
    columns = { "Item", "Price" },
    rows = { { id = "2,8", cells = { "Dirt", "3 for 1 WL" }, sort = { "Dirt", 0.33 },
               tile = { x = 2, y = 8 }, style = "ok" | "bad" | "muted", selected = true,
               actions = { { id = "check", label = "Check" } } } },   -- a click on a row: action "select", row = its id
    empty = "Nothing here",
})
gtproxy.panel.remove("vend")
```

The window sorts a table by any column (by `sort` values when given). A row
with a `tile` centres the map on it. Marks and tabs of a module go when it is
unloaded; a pick waiting goes when you leave the world.

#### Calls between modules **(3)**

```lua
gtproxy.export("players_in", function(zone) ... return list end)      -- in world_moderation
local ok, list = pcall(gtproxy.call, "world_moderation", "players_in", zone)   -- in another module
```

Values are copied between the two Lua states (true/false, numbers, text and
tables of them; the first returned value). An error (module not loaded, no
such function) is a Lua error: use `pcall`.

### Timers, data, more files

```lua
local id = gtproxy.after(2000, function() ... end)   -- once, after 2 s
local id = gtproxy.every(200, function() ... end)    -- every 200 ms
gtproxy.cancel(id)                                   -- nil is fine
gtproxy.now()                                        -- milliseconds, for differences
gtproxy.time()                                       -- (3) seconds since 1970 (UTC)
gtproxy.date()                                       -- (3) "2026-11-15 18:04:05", local time (also gtproxy.date(seconds))

gtproxy.store.get("fast_drop", false)   -- from data/<id>.json, or the default
gtproxy.store.set("fast_drop", true)    -- true/false, a number, text; nil removes it

local items = gtproxy.load("items.lua") -- another Lua file of the module's folder, run once
gtproxy.text.strip_colors("`2Hi``")     -- "Hi"
gtproxy.text.sanitize(text)             -- without '|' and line breaks (safe in dialogs)
```

Timers run on the proxy's main loop (at least 10 ms apart). A repeating timer
that fails is stopped. A broken `data/<id>.json` is reported; the module
starts without it and the broken file is kept as `<id>.json.broken`.

### Log **(1)**

```lua
gtproxy.log.info("text")    -- also .debug (session log only), .warn and .error; lines start with [<id>]
```

### Limits

- A handler, timer or command that runs longer than 2 seconds is stopped; the
  log says which module.
- An error in a module's code is logged with the file and line; other
  modules and handlers keep working.
- A module only reaches the server through `gtproxy.server.*`.

## The modules shipped with the proxy

| Module | Commands | Menu page | Settings |
|--------|----------|-----------|----------|
| `drop` | `/inv`, `/dropall [now\|stop\|speed <ms>]`, `/daw`, `/dd [WL]`, `/dw`, `/ddl`, `/db [amount]`, `/drop <item> [amount]`, `/fastdrop`, `/fasttrash`, `/keep [add\|del] <item>`, `/balance` | Items (inventory, drop all, locks, fast drop/trash, pause) | `dialog_timeout_ms`, `ack_timeout_ms`; your choices in `data/drop.json` |
| `worlds` | `/warp <world> [door]`, `/back`, `/rejoin`, `/exit` | Worlds (warp, back, recent worlds) | `exit_timeout_ms` |
| `looks` | `/nick [name]`, `/netid`, `/skin [hex] <code>`, `/rainbow [ms]` | Looks | `rainbow_ms` |
| `camera` | the mouse wheel over the game; `/zoom <value> [ms]`, `/zoom reset` | Camera (wheel mode, step, range, zoom to a value) | `wheel` (`always`/`ctrl`/`shift`/`alt`/`off`), `wheel_step`, `invert`, `min_zoom`, `max_zoom`, `zoom`, `normal`, `time_ms`, `keep`, `keep_delay_ms`; your last zoom in `data/camera.json` |
| `info` | `/pos`, `/players`, `/world`, `/find <name>`, `/item <id>`, `/debug ...` | Info | |
| `walk` | `/goto <x> <y>`, `/goto stop`, `/clickwalk`, `/walking`; left click in the map window | Walking (speed, step, gravity, jump height, jumps in the air, doors) | `speed`, `step_ms`, `gravity`, `jump_height`, `air_jumps`, `doors`, `tell_game` |
| `reachability` | `/reach <x> <y>` (and `reach x y` in the console); right click in the map window | | `show_marker`, `show_steps`, `log_result` |
| `vend_finder` | `/pvend <name\|ID>` | map window tab Вендинги | `presets`, `check_timeout_ms`, `buy_timeout_ms`, `buycount` (`items`/`packs`) |
| `item_finder` | `/pfind <name\|ID\|any>`, `/pfind go`, `/pfind stop` | map window tab Предметы | `walk_gravity`, `list_limit` |
| `world_scan` | `/growscan [name\|ID]`, `/track [on\|off\|clear]`, `/render` | map window tabs Скан мира, Журнал | `journal_size`, `marks_limit`, `track_on_start` |
| `world_moderation` | `/wm [list]`, `/wm pull\|kick\|ban <exact name>`, `/wm stop` | map window tabs Модерация, Журнал модерации | `rules_enabled`, `dry_run`, `rules`, `exceptions`, `window_timeout_ms`, `result_timeout_ms`, `gap_ms` |
| `autohost` | `/pos1`-`/pos4`, `/host start\|stop\|reset`, `/win1`-`/win4 [amount]` | map window tabs Автохост, Выплата | `commission_percent`, `rounding` (`down`/`up`/`nearest`), `currency`, `zone_radius`, `near_tiles`, `max_drop`, `dialog_timeout_ms`, `result_timeout_ms`; points in `data/autohost.json` per server and world |

If a module's command is already taken (by the proxy, a script or another
module), the module takes its name with a prefix instead (`/vend_pvend`,
`/item_pfind`, `/scan_growscan`, `/ah_host`, `/world_mod` ...) and says so in
the log. The proxy cannot know your server's own commands: a module command
with the same name as one of your server's is answered by the proxy and never
reaches the server.

The five modules of a local test world (`vend_finder`, `item_finder`,
`world_scan`, `world_moderation`, `autohost`) only do what you could do by
hand in the game: open a machine's, a player's or a lock's window with the
wrench, press its own button, answer the drop window. The server checks your
rights and confirms; each action ends as «Подтверждено», «Отклонено», «Нет
прав», «Недоступно» or «Результат неизвестен», and nothing is sent twice by
itself. Their tabs are in Russian; what they write in the game is in English
(the game's font has no Cyrillic). Leaving the world, another world or a
lost connection drops every target, way and unconfirmed action.

The core keeps `/proxy` (the menu frame), `/proxyhelp` (all commands) and
`/map` (open the map window).

Settings changed on a module's page (Walking, Camera) are written to its
`config/modules/<id>.json`: the page and the file are the same settings.

`camera` takes the mouse wheel over the game's window (`wheel`) and moves the
camera with the server's own call `OnZoomCamera(zoom, time in ms)`, sent to
your game only, so the game's zoom limits do not apply: each notch multiplies
the zoom by `wheel_step` within `min_zoom`..`max_zoom`. If the camera moves the
wrong way, tick `invert`. With `wheel: "ctrl"` the plain wheel stays the
game's (for scrolling dialogs). The game resets the camera when you enter a
world; with `keep` the module sends your zoom again.

`examples/modules/whereami` (not installed) is a short API 1 example to copy
into `modules/`. Item settings of earlier versions (`gtproxy-settings.txt`)
are moved to `data/drop.json` on the first start.
