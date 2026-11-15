-- Walking (module API 4, see docs/modules.md).
--
-- Alt + click on a tile in the game (the key is "bind": alt, ctrl, shift or
-- off) walks there instead of punching; the game only sends clicks within
-- your punch reach (longer with Admin mode's reach). Also /goto <x> <y>,
-- click-walk (every click walks), saved spots (/spot) and a left click in
-- the map window when it is on. The
-- proxy's walking engine finds the way and sends the positions; this
-- module decides when to walk, what to tell you, and how walks are made:
--
--   speed        world pixels a second (32 per tile; the game runs ~250)
--   step_ms      how often a position is sent
--   gravity      false: through any free tile; true: only paths you could
--                run and jump along
--   jump_height  tiles one jump rises (gravity)
--   air_jumps    jumps before landing again, 1..1000 (gravity)
--   doors        "access": doors and entrances open to you (public, no
--                lock, or your lock / access list); "all": every one;
--                "none": doors only as a destination
--
-- Set them in config/modules/walk.json or on the Walking page of /proxy
-- (which writes the same file). The Growtopia server decides what it
-- accepts: an official server may stop or reject fast walks.

local P = gtproxy.prefix
local ICON_WALK = 42        -- Boots
local click_walk = false

local function say(text)
    gtproxy.game.message(text)
end

-- The settings in force -> the walking engine.
local function apply_settings()
    return gtproxy.walk.options({
        speed = gtproxy.setting("speed"),
        step_ms = gtproxy.setting("step_ms"),
        gravity = gtproxy.setting("gravity"),
        jump_height = gtproxy.setting("jump_height"),
        air_jumps = gtproxy.setting("air_jumps"),
        doors = gtproxy.setting("doors"),
    })
end

local function describe(o)
    local how = o.gravity
        and string.format("on foot, jump %d tile(s), %d jump(s) in the air", o.jump_height, o.air_jumps)
        or "through any free tile"
    return string.format("%s px/s, a step every %d ms, %s, doors: %s",
        string.format("%g", o.speed), o.step_ms, how, o.doors)
end

apply_settings()
gtproxy.on("settings", apply_settings)

-- Left click in the map window: walk to the tile.
gtproxy.on("left_click", function(e)
    local walk = gtproxy.walk.to(e.x, e.y)
    gtproxy.show({
        title = walk.message,
        kind = walk.ok and "ok" or "bad",
        toast = true,
        x = walk.x,
        y = walk.y,
    })
end)

-- Alt + click (the bind key held) or click-walk: a punch / build in the
-- game becomes a walk.
gtproxy.on("tile_click", function(e)
    local bind = gtproxy.setting("bind")
    local held = bind ~= "off" and gtproxy.input.key_down(bind)
    if not click_walk and not held then
        return
    end
    local walk = gtproxy.walk.to(e.x, e.y)
    say("`o[walk] " .. walk.message)
    return "cancel"
end)

---------------------------------------------------------------------------
-- Saved spots, per server and world (data/walk.json)
---------------------------------------------------------------------------

local function world_key()
    local info = gtproxy.world.info()
    if not info.in_world then
        return nil
    end
    return (info.server ~= "" and info.server or "server") .. "/" .. info.name
end

local function spot_names(key)
    local list = {}
    for name in tostring(gtproxy.store.get("spots/" .. key, "")):gmatch("[^,]+") do
        list[#list + 1] = name
    end
    return list
end

local function spot(key, name)
    local x, y = tostring(gtproxy.store.get("spot/" .. key .. "/" .. name, "")):match("^(%-?%d+),(%-?%d+)$")
    return tonumber(x), tonumber(y)
end

local function save_spot(name)
    local key = world_key()
    local me = gtproxy.world.info().me
    if not key or not me then
        return false, "you are not in a world"
    end
    name = name:lower():gsub("[^%w_]", "")
    if name == "" or #name > 16 then
        return false, "a spot's name is 1-16 letters or digits"
    end
    local names = spot_names(key)
    local known = false
    for _, n in ipairs(names) do
        known = known or n == name
    end
    if not known then
        if #names >= 12 then
            return false, "12 spots per world at most (spot del <name>)"
        end
        names[#names + 1] = name
        gtproxy.store.set("spots/" .. key, table.concat(names, ","))
    end
    gtproxy.store.set("spot/" .. key .. "/" .. name, string.format("%d,%d", me.tile_x, me.tile_y))
    return true, string.format("spot `w%s`` = %d, %d", name, me.tile_x, me.tile_y)
end

local function delete_spot(name)
    local key = world_key()
    if not key then
        return false, "you are not in a world"
    end
    local names, left = spot_names(key), {}
    for _, n in ipairs(names) do
        if n ~= name then
            left[#left + 1] = n
        end
    end
    gtproxy.store.set("spots/" .. key, #left > 0 and table.concat(left, ",") or nil)
    gtproxy.store.set("spot/" .. key .. "/" .. name, nil)
    return #left < #names, #left < #names and ("spot `w" .. name .. "`` deleted") or "no such spot"
end

local function go_spot(name)
    local key = world_key()
    if not key then
        return false, "you are not in a world"
    end
    local x, y = spot(key, name)
    if not x then
        return false, "no spot `w" .. name .. "`` in this world"
    end
    local walk = gtproxy.walk.to(x, y)
    return walk.ok, walk.message
end

-- Arrived, stopped by the server or a blocked way (not when you left the
-- world or pressed Stop in the window).
gtproxy.on("walk_end", function(e)
    if e.notify and gtproxy.setting("tell_game") then
        say("`o[map] " .. e.message)
    end
end)

local function whole(text)
    if type(text) ~= "string" or not text:match("^%-?%d+$") then
        return nil
    end
    return math.tointeger(tonumber(text))
end

gtproxy.command("goto", "Walk to a tile: goto <x> <y>; goto stop", function(args, ctx)
    if #args == 1 and args[1] == "stop" then
        ctx.reply("`o[map] " .. gtproxy.walk.stop().message)
        return
    end

    local x, y = whole(args[1]), whole(args[2])
    if #args ~= 2 or not x or not y then
        ctx.reply(string.format("`4Usage: ``%sgoto <x> <y> (tile, as %spos shows it); %sgoto stop", P, P, P))
        return
    end

    local walk = gtproxy.walk.to(x, y)
    ctx.reply((walk.ok and "`o" or "`4") .. "[map] " .. walk.message)
end)

gtproxy.command("spot", "Saved spots of this world: spot save <name> | spot <name> | spot del <name> | spot", function(args, ctx)
    local word = (args[1] or ""):lower()
    if word == "" or word == "list" then
        local key = world_key()
        if not key then
            ctx.reply("`4Oops: ``you are not in a world.")
            return
        end
        local parts = {}
        for _, n in ipairs(spot_names(key)) do
            local x, y = spot(key, n)
            parts[#parts + 1] = string.format("`w%s`` (%d, %d)", n, x or 0, y or 0)
        end
        ctx.reply(#parts > 0 and ("`oSpots: " .. table.concat(parts, ", ")) or string.format("`oNo spots here: %sspot save <name>", P))
        return
    end
    local ok, text
    if word == "save" then
        ok, text = save_spot(args[2] or "")
    elseif word == "del" then
        ok, text = delete_spot((args[2] or ""):lower())
    else
        ok, text = go_spot(word)
    end
    ctx.reply((ok and "`o" or "`4Oops: ``") .. text)
end)

gtproxy.command("clickwalk", "Toggle: clicking a tile in the game walks there instead of punching", function(_, ctx)
    click_walk = not click_walk
    if click_walk then
        ctx.reply(string.format("`oClick-walk `2on``: clicking a tile walks there (no punching or building). %sclickwalk again to turn it off.", P))
    else
        ctx.reply("`oClick-walk `4off``: clicks punch and build again.")
    end
end)

---------------------------------------------------------------------------
-- The Walking page of /proxy: the same settings as config/modules/walk.json
---------------------------------------------------------------------------

local DOORS = {
    { "access", "Doors open to me (my lock / access, public)" },
    { "all", "All doors and entrances (I may pass)" },
    { "none", "No doors (only walk up to them)" },
}

local BINDS = { { "alt", "Alt" }, { "ctrl", "Ctrl" }, { "shift", "Shift" }, { "off", "Off" } }

local function walking_page()
    local o = gtproxy.walk.options()
    local d = gtproxy.dialog():header("Walking", ICON_WALK)
    local bind = gtproxy.setting("bind")
    d:small(bind == "off" and "`oKey + click: `woff``"
        or string.format("`oHold `w%s`` and click a tile in the game: you walk there (within your punch reach).", bind))
    for _, b in ipairs(BINDS) do
        d:button("walk_bind_" .. b[1], (b[1] == bind and "`2> " or "") .. "Key: " .. b[2] .. (b[1] == bind and "``" or ""))
    end
    local key = world_key()
    if key then
        local names = spot_names(key)
        d:spacer():small(#names > 0 and "`oSaved spots of this world:" or string.format("`oNo saved spots here (%sspot save <name>, or below).", P))
        for _, n in ipairs(names) do
            local x, y = spot(key, n)
            d:button("walk_spot_" .. n, string.format("Go to %s (%d, %d)", n, x or 0, y or 0))
        end
        d:input("spot_name", "Save this tile as:", "", 16):button("walk_spot_save", "Save spot")
    end
    d:spacer():small("`oThe proxy's walking (key + click, /goto, spots) - not your own movement (that is Admin mode).")
    d:small("`oNow: `w" .. describe(o) .. "``")
    d:small("`oSaved to config/modules/walk.json - the same file you can edit by hand.")
    d:spacer()
        :input("speed", "Speed (px/s, 16-10000; the game runs ~250):", string.format("%g", o.speed), 6)
        :input("step_ms", "A position every (ms, 10-1000):", tostring(o.step_ms), 4)
        :checkbox("gravity", "On foot: only paths you could run and jump along", o.gravity)
        :input("jump_height", "Jump height (tiles, 1-200):", tostring(o.jump_height), 3)
        :input("air_jumps", "Jumps in the air before landing (1-1000):", tostring(o.air_jumps), 4)
        :button("walk_save", "`2Save``")
        :spacer()
        :small("`oDoors and entrances (House Entrance, VIP ...): `w" .. o.doors .. "``")
    for _, door in ipairs(DOORS) do
        d:button("walk_doors_" .. door[1], (door[1] == o.doors and "`2> " or "") .. door[2] .. (door[1] == o.doors and "``" or ""))
    end
    return d:spacer()
        :button("page_main", "<< Menu")
        :finish("gtproxy_walking", "Save")
end

local function show_page()
    gtproxy.game.dialog(walking_page())
end

local LIMITS = {
    speed = { 16, 10000 },
    step_ms = { 10, 1000 },
    jump_height = { 1, 200 },
    air_jumps = { 1, 1000 },
}

-- Saves what changed; returns the problems (empty when all is in force).
local function save(changes)
    local problems = {}
    for key, value in pairs(changes) do
        if value ~= gtproxy.setting(key) then
            local ok, why = gtproxy.save_setting(key, value)
            if not ok then
                problems[#problems + 1] = why
            end
        end
    end
    apply_settings()
    return problems
end

local function report(problems)
    if #problems == 0 then
        say("`oWalking: `w" .. describe(gtproxy.walk.options()) .. "`` (saved).")
    else
        say("`4Walking: ``" .. table.concat(problems, "; "))
    end
end

gtproxy.on("dialog", function(e)
    if e.button == "page_walking" then
        show_page()
        return
    end
    if e.name ~= "gtproxy_walking" then
        return
    end

    local bind = e.button:match("^walk_bind_(%a+)$")
    if bind then
        local problems = save({ bind = bind })
        say(#problems == 0 and ("`oWalking key: `w" .. bind .. "``") or ("`4Walking: ``" .. table.concat(problems, "; ")))
        show_page()
        return
    end
    if e.button == "walk_spot_save" then
        local ok, text = save_spot(e.values.spot_name or "")
        say((ok and "`o" or "`4Oops: ``") .. text)
        show_page()
        return
    end
    local spot_name = e.button:match("^walk_spot_([%w_]+)$")
    if spot_name then
        local ok, text = go_spot(spot_name)
        say((ok and "`o[walk] " or "`4Oops: ``") .. text)
        return
    end

    local door = e.button:match("^walk_doors_(%a+)$")
    if door then
        report(save({ doors = door }))
        show_page()
        return
    end

    -- Save (or Apply, or any button of the page): the fields as they are.
    if e.values.speed == nil then
        return
    end
    local changes = { gravity = e.values.gravity == "1" }
    local bad = {}
    for key, limit in pairs(LIMITS) do
        local value = tonumber(gtproxy.text.sanitize(e.values[key] or ""))
        if value and value == value then
            if key ~= "speed" then
                value = math.floor(value)
            end
            changes[key] = math.max(limit[1], math.min(limit[2], value))
        elseif e.values[key] then
            bad[#bad + 1] = key
        end
    end
    local problems = save(changes)
    if #bad > 0 then
        problems[#problems + 1] = "not a number: " .. table.concat(bad, ", ")
    end
    if not e.frame then
        report(problems)
        if e.button == "walk_save" then
            show_page()
        end
    end
end)

gtproxy.menu.add({
    title = "Walking",
    icon = ICON_WALK,
    button = "page_walking",
    label = "Alt + click, saved spots, speed, doors",
    order = 15,
})

gtproxy.command("walking", "Walking settings: speed, jumps, doors (the Walking page)", function(_, ctx)
    if ctx.source == "game" then
        show_page()
    else
        ctx.reply("Walking: " .. describe(gtproxy.walk.options()) .. " (config/modules/walk.json)")
    end
end)
