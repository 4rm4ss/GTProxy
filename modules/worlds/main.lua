-- Worlds (module API 2, see docs/modules.md).
--
-- /warp <world> [door], /back, /rejoin, /exit and the Worlds page of
-- /proxy. They send what the game itself sends when you press Exit and
-- type a world name: from inside a world it exits first and joins once the
-- server shows the world menu (or after exit_timeout_ms if it never does).

local P = gtproxy.prefix
local ICON_WORLDS = 12
local sanitize = gtproxy.text.sanitize

-- Growtopia world names: 1-23 letters or digits, upper case.
local function world_name(text)
    if not text or #text < 1 or #text > 23 or not text:match("^%w+$") then
        return nil
    end
    return string.upper(text)
end

-- Door ids: 1-24 letters, digits or _, upper case.
local function door_id(text)
    if not text or #text < 1 or #text > 24 or not text:match("^[%w_]+$") then
        return nil
    end
    return string.upper(text)
end

local pending = nil         -- "WORLD" or "WORLD|DOOR" to join after exiting
local pending_timer = nil

local function send_join(target)
    gtproxy.log.info("Warping to " .. target)
    gtproxy.server.game_text("action|join_request\nname|" .. target .. "\ninvitedWorld|0")
end

local function clear_pending()
    pending = nil
    gtproxy.cancel(pending_timer)
    pending_timer = nil
end

-- Joins "WORLD" or "WORLD|DOOR". False if not connected to the game server.
local function warp(target)
    local here = gtproxy.world.info()
    if not here.connected then
        return false
    end

    if not here.in_world then
        clear_pending()
        send_join(target)
        return true
    end

    -- Leave first, like pressing Exit; the join follows once the server
    -- shows the world menu (or after a short timeout if it never does).
    clear_pending()
    pending = target
    pending_timer = gtproxy.after(gtproxy.setting("exit_timeout_ms"), function()
        pending_timer = nil
        local later = pending
        pending = nil
        if later and gtproxy.world.info().connected then
            gtproxy.log.info("Warp: no world menu from the server, joining " .. later .. " anyway")
            send_join(later)
        end
    end)
    gtproxy.server.game_text("action|quit_to_exit")
    return true
end

gtproxy.on("world_menu", function()
    if pending then
        local target = pending
        clear_pending()
        send_join(target)
    end
end)

gtproxy.on("disconnect", clear_pending)

local function go(target, reply)
    if not warp(target) then
        reply("`4Oops: ``Not connected to the game server.")
        return
    end
    reply(string.format("`oWarping to `w%s``...", target))
end

local function cmd_warp(args, reply)
    if #args == 0 or #args > 2 then
        reply(string.format("`4Usage: ``%swarp <world> [door id]", P))
        return
    end

    -- Also accept the game's own "WORLD|DOOR" spelling.
    local world, door = args[1], args[2] or ""
    local bar = world:find("|", 1, true)
    if bar and door == "" then
        world, door = world:sub(1, bar - 1), world:sub(bar + 1)
    end

    local name = world_name(world)
    if not name or name == "EXIT" then
        reply("`4Oops: ``World names are 1-23 letters or digits.")
        return
    end

    local target = name
    if door ~= "" then
        local id = door_id(door)
        if not id then
            reply("`4Oops: ``Door ids are letters, digits or _ (up to 24).")
            return
        end
        target = target .. "|" .. id
    end
    go(target, reply)
end

local function cmd_back(_, reply)
    local previous = gtproxy.world.info().previous
    if previous == "" then
        reply("`4Oops: ``No previous world yet.")
        return
    end
    go(previous, reply)
end

local function cmd_rejoin(_, reply)
    local here = gtproxy.world.info()
    if not here.in_world or here.name == "" then
        reply("`4Oops: ``You are not in a world.")
        return
    end
    go(here.name, reply)
end

local function cmd_exit()
    gtproxy.server.game_text("action|quit_to_exit")
end

gtproxy.command("warp", "Go to a world: warp <world> [door id]", function(args, ctx) cmd_warp(args, ctx.reply) end)
gtproxy.command("back", "Go back to the previous world", function(args, ctx) cmd_back(args, ctx.reply) end)
gtproxy.command("rejoin", "Exit and enter the current world again", function(args, ctx) cmd_rejoin(args, ctx.reply) end)
gtproxy.command("exit", "Exit the current world.", function() cmd_exit() end)

---------------------------------------------------------------------------
-- The Worlds page of /proxy
---------------------------------------------------------------------------

local shown_recent = {}     -- recent_<i> buttons of the page last shown

local function worlds_page()
    local here = gtproxy.world.info()
    local d = gtproxy.dialog():header("Worlds", ICON_WORLDS)
    d:small(string.format(
        "`oNow: `w%s``   Before: `w%s``",
        here.in_world and sanitize(here.name) or "-",
        here.previous == "" and "-" or sanitize(here.previous)
    ))
    d:spacer()
        :input("world", "World (WORLD or WORLD DOOR):", "", 30)
        :button("warp", "`2Warp``")
    if here.previous ~= "" then
        d:button("back", "Back to " .. sanitize(here.previous))
    end
    if here.in_world then
        d:button("rejoin", "Rejoin this world")
    end

    shown_recent = here.recent
    local current = here.in_world and here.name or ""
    if #shown_recent > 1 or (#shown_recent == 1 and shown_recent[1] ~= current) then
        d:spacer():text("`wRecent worlds``")
        for i, name in ipairs(shown_recent) do
            if name ~= current then
                d:button(string.format("recent_%d", i - 1), sanitize(name))
            end
        end
    end

    return d:spacer():button("page_main", "<< Menu"):finish("gtproxy_worlds")
end

gtproxy.menu.add({
    title = "Worlds",
    icon = ICON_WORLDS,
    button = "page_worlds",
    label = "Warp, back, recent worlds",
    order = 10,
})

local function message(text)
    gtproxy.game.message(text)
end

gtproxy.on("dialog", function(e)
    if e.frame then
        return
    end
    local button = e.button

    if button == "page_worlds" then
        gtproxy.game.dialog(worlds_page())
    elseif button == "warp" then
        local words = {}
        for word in string.gmatch(sanitize(e.values.world or ""), "%S+") do
            words[#words + 1] = word
        end
        if #words == 0 then
            message("`4Oops: ``Type a world name first.")
            return
        end
        cmd_warp(words, message)
    elseif button == "back" then
        cmd_back({}, message)
    elseif button == "rejoin" then
        cmd_rejoin({}, message)
    elseif button:match("^recent_%d+$") then
        local index = tonumber(button:sub(8))
        local recent = gtproxy.world.info().recent
        if recent[index + 1] then
            cmd_warp({ recent[index + 1] }, message)
        end
    end
end)
