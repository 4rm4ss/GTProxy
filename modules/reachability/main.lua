-- Reachability check (module API 1, see docs/modules.md).
--
-- Right click in the map window, /reach X Y in the game and reach X Y in the
-- proxy console all ask the same question: can you get to that tile from
-- where you stand? The answer comes from the proxy's own check (a search over
-- the four neighbours of each tile, gtproxy.reach); this module decides what
-- is shown and logged, following its settings:
--
--   show_marker   mark the tile in the map window
--   show_steps    say how many steps the way takes
--   log_result    write every check to the proxy log

-- Card colour per result.
local KIND = { reachable = "ok", no_data = "none" }

-- The game's font has no Cyrillic: the game gets the result in English.
local IN_ENGLISH = {
    reachable = "`2target reachable``",
    unreachable = "`4target unreachable``",
    blocked = "`4target unreachable: a block is there``",
    out_of_bounds = "`4target unreachable: outside the map``",
}

-- "От 5, 4 до 0, 4: 5 шагов. Cave Background (14) - free."
local function details(r)
    if r.status == "no_data" then
        return "Нет данных: " .. r.missing .. "."
    end

    local text = string.format("От %d, %d до %d, %d", r.start_x, r.start_y, r.x, r.y)
    if r.status == "reachable" and gtproxy.setting("show_steps") then
        text = text .. string.format(": %d %s", r.steps, r.steps_word)
    end
    text = text .. "."
    if r.tile_text ~= "" then
        text = text .. " " .. r.tile_text
    end
    return text
end

local function check(x, y, source)
    local r = gtproxy.reach(x, y)
    if gtproxy.setting("log_result") then
        -- Your position, the tile and the result.
        gtproxy.log.info(string.format("Reach check (%s): %s", source, r.summary))
    end
    return r
end

gtproxy.on("right_click", function(event)
    local r = check(event.x, event.y, "map window right click")
    gtproxy.show({
        title = r.message,
        text = details(r),
        kind = KIND[r.status] or "bad",
        marker = gtproxy.setting("show_marker"),
        status = r.status,
    })
end)

local function whole(text)
    local number = tonumber(text)
    if number and number == math.floor(number) then
        return number
    end
    return nil
end

gtproxy.command("reach", "Can you get to a tile from where you stand: reach <x> <y>", function(args, ctx)
    local x, y = whole(args[1]), whole(args[2])
    if #args ~= 2 or not x or not y then
        ctx.reply(ctx.source == "game"
            and "`4Usage: ``/reach <x> <y> (tile, as /pos shows it)"
            or "Usage: reach <x> <y> (tile, as /pos shows it)")
        return
    end

    if ctx.source == "game" then
        local r = check(x, y, "/reach in the game")
        local text = IN_ENGLISH[r.status] or "`5no data`` (no world, items.dat or position yet - see the proxy log)"
        if r.status == "reachable" and gtproxy.setting("show_steps") then
            text = text .. string.format(" (%d steps)", r.steps)
        end
        ctx.reply(string.format("[map] %d, %d: %s", x, y, text))
    else
        local r = check(x, y, "proxy console")
        ctx.reply(r.message .. " - " .. details(r))
    end
end)
