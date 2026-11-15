-- Looks (module API 2, see docs/modules.md).
--
-- /nick, /netid, /skin, /rainbow and the Looks page of /proxy. Everything
-- here only changes what your own game shows: nothing is sent to the
-- server.
--
-- Settings (config/modules/looks.json): rainbow_ms, the default pause
-- between rainbow colours.

local P = gtproxy.prefix
local ICON_LOOKS = 1796
local sanitize = gtproxy.text.sanitize
local strip_colors = gtproxy.text.strip_colors

local RAINBOW_COLORS = {
    0xFF0000FF, -- red
    0xFF3300FF,
    0xFF6600FF, -- orange
    0xFF9900FF,
    0xFFCC00FF,
    0xFFFF00FF, -- yellow
    0x99FF00FF,
    0x00FF00FF, -- green
    0x00FF99FF,
    0x00FFFFFF, -- cyan
    0x0099FFFF,
    0x0000FFFF, -- blue
    0x000099FF,
    0x6600FFFF, -- indigo
    0x9900FFFF, -- purple
}
local MIN_RAINBOW_MS, MAX_RAINBOW_MS = 50, 5000

local nick = ""
local show_net_ids = false
local players = {}          -- netID -> { name = as the server sent it, title = title JSON }
local local_net_id = -1

-- "key|value" lines of the game's text (OnSpawn).
local function field(text, key)
    for line in (text .. "\n"):gmatch("(.-)\n") do
        if line:sub(1, #key + 1) == key .. "|" then
            return line:sub(#key + 2)
        end
    end
    return nil
end

-- The text with the value of the first 'key' line replaced; nil without one.
local function replace_field(text, key, value)
    local start = 1
    while start <= #text + 1 do
        local stop = text:find("\n", start, true) or (#text + 1)
        local line = text:sub(start, stop - 1)
        if line:sub(1, #key + 1) == key .. "|" then
            return text:sub(1, start + #key) .. value .. text:sub(stop)
        end
        start = stop + 1
    end
    return nil
end

-- A nick with colour codes keeps them; a plain one is shown white.
local function colored_name(name)
    local clean = sanitize(name)
    if clean:find("`", 1, true) then
        return clean
    end
    return "`w" .. clean .. "``"
end

-- Used only when the server never told us a player's title JSON.
local function default_title(net_id)
    return string.format(
        '{"PlayerWorldID":%d,"TitleTexture":"","TitleTextureCoordinates":"","WrenchCustomization":{"WrenchForegroundCanRotate":false,"WrenchForegroundID":-1,"WrenchIconID":-1}}',
        net_id
    )
end

local function shown_name(net_id, server_name)
    local name = (net_id == local_net_id and nick ~= "") and colored_name(nick) or server_name
    if show_net_ids then
        name = name .. string.format(" `4[%d]``", net_id)
    end
    return name
end

-- Shows a player's name again (after the nick or the netID option changed).
local function refresh(net_id)
    local player = players[net_id]
    if net_id < 0 or not player then
        return
    end
    gtproxy.game.call("OnNameChanged", net_id, shown_name(net_id, player.name), player.title ~= "" and player.title or default_title(net_id))
end

local function forget_players()
    players = {}
    local_net_id = -1
end

gtproxy.hook_call("OnSpawn", function(call)
    local text = call.args[1]
    if type(text) ~= "string" then
        return
    end
    local net_id = math.tointeger(tonumber(field(text, "netID") or ""))
    local server_name = field(text, "name")
    if not net_id or not server_name then
        return
    end

    players[net_id] = { name = server_name, title = field(text, "titleIcon") or "" }
    if field(text, "type") == "local" then
        local_net_id = net_id
    end

    local name = shown_name(net_id, server_name)
    if name == server_name then
        return
    end
    -- Only the name line changes; every other field goes to the game as
    -- the server sent it.
    local changed = replace_field(text, "name", name)
    if changed then
        call.args[1] = changed
        return "replace"
    end
end)

gtproxy.hook_call("OnNameChanged", function(call)
    local server_name = call.args[1]
    if type(server_name) ~= "string" then
        return
    end
    local player = players[call.net_id] or { name = "", title = "" }
    player.name = server_name
    if type(call.args[2]) == "string" and call.args[2] ~= "" then
        player.title = call.args[2]
    end
    players[call.net_id] = player

    local name = shown_name(call.net_id, server_name)
    if name ~= server_name then
        call.args[1] = name
        return "replace"
    end
end)

gtproxy.hook_call("OnRemove", function(call)
    local text = call.args[1]
    local net_id = type(text) == "string" and math.tointeger(tonumber(field(text, "netID") or ""))
    if net_id then
        players[net_id] = nil
    end
end)

gtproxy.on("world_menu", forget_players)
gtproxy.on("world_enter", forget_players)
gtproxy.on("disconnect", function()
    forget_players()
end)

---------------------------------------------------------------------------
-- Rainbow skin
---------------------------------------------------------------------------

local rainbow_timer = nil
local rainbow_index = 0

local function stop_rainbow()
    gtproxy.cancel(rainbow_timer)
    rainbow_timer = nil
end

local function next_colour()
    local net_id = gtproxy.world.info().net_id
    if net_id < 0 then
        stop_rainbow()
        gtproxy.log.info("Rainbow effect stopped (left the world)")
        return
    end
    gtproxy.game.call("OnChangeSkin", net_id, gtproxy.u32(RAINBOW_COLORS[rainbow_index + 1]))
    rainbow_index = (rainbow_index + 1) % #RAINBOW_COLORS
end

local function cmd_rainbow(args, reply)
    if gtproxy.world.info().net_id < 0 then
        reply("`4Error: ``You are not in a world")
        return
    end
    if rainbow_timer then
        stop_rainbow()
        reply("`2Rainbow effect stopped")
        return
    end

    local interval = gtproxy.setting("rainbow_ms")
    if #args > 0 then
        interval = math.tointeger(tonumber(args[1]))
        if not interval then
            reply("`4Error: ``Invalid speed value '" .. sanitize(args[1]) .. "'")
            return
        end
    end
    if interval < MIN_RAINBOW_MS then
        reply(string.format("`4Error: ``Too fast (at least %d ms)", MIN_RAINBOW_MS))
        return
    end
    if interval > MAX_RAINBOW_MS then
        reply(string.format("`4Error: ``Too slow (at most %d ms)", MAX_RAINBOW_MS))
        return
    end

    rainbow_index = 0
    next_colour()
    rainbow_timer = gtproxy.every(interval, next_colour)
    reply(string.format("`2Rainbow effect started ``(speed: %dms)", interval))
end

gtproxy.on("world_menu", stop_rainbow)
gtproxy.on("disconnect", stop_rainbow)

---------------------------------------------------------------------------
-- Commands
---------------------------------------------------------------------------

local function cmd_nick(args, reply)
    if #args == 0 then
        nick = ""
        refresh(local_net_id)
        reply("`oNick reset to your real name.")
        return
    end

    local wanted = sanitize(table.concat(args, " "))
    if strip_colors(wanted) == "" or #wanted > 64 then
        reply("`4Oops: ``Nick must have 1-64 characters.")
        return
    end

    nick = wanted
    refresh(local_net_id)
    reply(string.format("`oNick set to %s`` (only you see it; %snick resets).", colored_name(nick), P))
end

local function cmd_netid(_, reply)
    show_net_ids = not show_net_ids
    for net_id in pairs(players) do
        refresh(net_id)
    end
    reply(show_net_ids and "`oNetIDs shown next to names." or "`oNetIDs hidden.")
end

local function cmd_skin(args, reply)
    if #args == 0 then
        reply(string.format("`4Usage: ``%sskin [hex] <code>", P))
        return
    end

    local hex = string.lower(args[1]) == "hex" and args[2] ~= nil and args[2]:match("^%x+$") ~= nil
    local text = hex and args[2] or args[1]
    local code = nil
    if hex and #text <= 8 then
        code = tonumber(text, 16)
    elseif not hex and text:match("^%d+$") then
        code = tonumber(text)
    end
    if not code or code > 0xFFFFFFFF then
        reply(string.format("`4Oops: ``Invalid skin code: %s", sanitize(text)))
        return
    end

    gtproxy.game.call("OnChangeSkin", gtproxy.world.info().net_id, gtproxy.u32(code))
    reply(string.format("Skin changed to %s", sanitize(text)))
end

gtproxy.command("nick", "Show another name above you (only you see it); no name resets", function(args, ctx) cmd_nick(args, ctx.reply) end)
gtproxy.command("netid", "Show/hide netIDs next to player names", function(args, ctx) cmd_netid(args, ctx.reply) end)
gtproxy.command("skin", "Change your skin code", function(args, ctx) cmd_skin(args, ctx.reply) end)
gtproxy.command("rainbow", "Toggle rainbow skin effect: rainbow [ms]", function(args, ctx) cmd_rainbow(args, ctx.reply) end)

---------------------------------------------------------------------------
-- The Looks page of /proxy
---------------------------------------------------------------------------

local function looks_page()
    local d = gtproxy.dialog():header("Looks", ICON_LOOKS)
    d:small("`oOnly you see these changes.")
    d:spacer()
        :input("nick", "Nick:", nick, 64)
        :button("set_nick", "Set nick")
    if nick ~= "" then
        d:button("reset_nick", "Back to my real name")
    end
    return d:checkbox("show_netid", "Show netIDs next to names", show_net_ids)
        :button("rainbow", "Rainbow skin on/off")
        :input("skin", "Skin colour code:", "", 10)
        :button("set_skin", "Set skin colour")
        :spacer()
        :button("page_main", "<< Menu")
        :finish("gtproxy_looks", "Apply")
end

gtproxy.menu.add({
    title = "Looks",
    icon = ICON_LOOKS,
    button = "page_looks",
    label = "Nick, netIDs, skin",
    order = 30,
})

local function message(text)
    gtproxy.game.message(text)
end

local function words(text)
    local out = {}
    for word in string.gmatch(text, "%S+") do
        out[#out + 1] = word
    end
    return out
end

gtproxy.on("dialog", function(e)
    -- The netID option applies with any button of the page (or Apply).
    if e.name == "gtproxy_looks" then
        local show = e.values.show_netid == "1"
        if show ~= show_net_ids then
            show_net_ids = show
            for net_id in pairs(players) do
                refresh(net_id)
            end
        end
    end
    if e.frame then
        return
    end

    local button = e.button
    local function with_text(run, name, missing)
        local text = sanitize(e.values[name] or "")
        if not text:find("%S") then
            message(missing)
            return
        end
        run(words(text), message)
    end

    if button == "page_looks" then gtproxy.game.dialog(looks_page())
    elseif button == "set_nick" then with_text(cmd_nick, "nick", "`4Oops: ``Type a nick first.")
    elseif button == "reset_nick" then cmd_nick({}, message)
    elseif button == "rainbow" then cmd_rainbow({}, message)
    elseif button == "set_skin" then with_text(cmd_skin, "skin", "`4Oops: ``Type a colour code first.")
    elseif button == "" and e.name == "gtproxy_looks" then message("`oSettings saved.")
    end
end)
