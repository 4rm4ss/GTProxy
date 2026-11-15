-- Walking (module API 2, see docs/modules.md).
--
-- Left click in the map window, /goto <x> <y> and click-walk (a click on a
-- tile in the game walks there instead of punching or building). The
-- proxy's walking engine finds the way and sends the positions; this
-- module decides when to walk and what to tell you.

local P = gtproxy.prefix
local click_walk = false

local function say(text)
    gtproxy.game.message(text)
end

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

-- Click-walk: a punch / build in the game becomes a walk.
gtproxy.on("tile_click", function(e)
    if not click_walk then
        return
    end
    local walk = gtproxy.walk.to(e.x, e.y)
    say("`o[map] " .. walk.message)
    return "cancel"
end)

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

gtproxy.command("clickwalk", "Toggle: clicking a tile in the game walks there instead of punching", function(_, ctx)
    click_walk = not click_walk
    if click_walk then
        ctx.reply(string.format("`oClick-walk `2on``: clicking a tile walks there (no punching or building). %sclickwalk again to turn it off.", P))
    else
        ctx.reply("`oClick-walk `4off``: clicks punch and build again.")
    end
end)
