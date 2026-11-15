-- Info (module API 2, see docs/modules.md).
--
-- /pos, /players, /world, /find, /item, /debug and the Info page of
-- /proxy: what the proxy knows about you, the world and items.dat.

local P = gtproxy.prefix
local ICON_INFO = 6016
local sanitize = gtproxy.text.sanitize
local FIND_LIMIT = 12

local function not_in_world(reply)
    reply("`4Oops: ``You are not in a world.")
end

local function cmd_pos(_, reply)
    local here = gtproxy.world.info()
    if not here.in_world or not here.me then
        reply("`4Oops: ``Position unknown - enter a world and move a bit.")
        return
    end
    reply(string.format(
        "`oYou are at tile `w%d, %d`` in `w%s``.",
        math.floor(here.me.x / 32),
        math.floor(here.me.y / 32),
        here.name
    ))
end

local function visible_players()
    local list = {}
    for _, player in ipairs(gtproxy.world.players()) do
        if not player.invisible then
            list[#list + 1] = player
        end
    end
    return list
end

local function cmd_players(_, reply)
    local here = gtproxy.world.info()
    if not here.in_world then
        not_in_world(reply)
        return
    end

    -- Only players the game shows: invisible ones stay hidden.
    local list = visible_players()
    local lines = {}
    for i, player in ipairs(list) do
        lines[i] = string.format(
            "\n`o[%d] %s`` `o%s%s",
            player.net_id,
            player.name,
            player.country == "" and "" or "(" .. player.country .. ")",
            player.is_local and " `2(you)``" or ""
        )
    end
    reply(string.format("`o%d player(s) in `w%s``:%s", #list, here.name, table.concat(lines)))
end

local function cmd_world(_, reply)
    local here = gtproxy.world.info()
    if not here.in_world then
        not_in_world(reply)
        return
    end
    reply(string.format(
        "`oWorld `w%s``: %dx%d tiles, %d player(s), %d dropped item stack(s). Previous world: `w%s``.",
        here.name,
        here.width,
        here.height,
        #visible_players(),
        #gtproxy.world.objects(),
        here.previous == "" and "-" or here.previous
    ))
end

local function cmd_find(args, reply)
    if #args == 0 then
        reply(string.format("`4Usage: ``%sfind <item name>", P))
        return
    end
    if not gtproxy.items.loaded() then
        reply("`4Oops: ``items.dat is not loaded yet - log in first.")
        return
    end

    local query = table.concat(args, " ")
    local found = gtproxy.items.find(query, FIND_LIMIT)
    if #found == 0 then
        reply(string.format("`oNo items match '%s'.", sanitize(query)))
        return
    end

    local lines = {}
    for i, item in ipairs(found) do
        lines[i] = string.format("\n`o%d: `w%s``", item.id, item.name)
    end
    reply(string.format(
        "`oItems matching '%s'%s:%s",
        sanitize(query),
        #found == FIND_LIMIT and " (first 12)" or "",
        table.concat(lines)
    ))
end

local function cmd_item(args, reply)
    local id = #args == 1 and args[1]:match("^%d+$") and math.tointeger(tonumber(args[1])) or nil
    if not id or id > 0xFFFFFFFF then
        reply(string.format("`4Usage: ``%sitem <id>", P))
        return
    end

    local item = gtproxy.items.get(id)
    if not item then
        reply(string.format("`4Oops: ``No item %d (is items.dat loaded?).", id))
        return
    end
    reply(string.format(
        "`o%d: `w%s`` - type %s, rarity %d, stacks up to %d",
        item.id,
        item.name,
        item.type,
        item.rarity,
        item.max_amount
    ))
end

-- /debug player [netID] | world | tile <x> <y> | objects
local function cmd_debug(args, reply)
    local what = args[1]
    if what == "player" then
        local players = gtproxy.world.players()
        if not args[2] then
            reply(string.format("Total Players: %d", #players))
            for _, player in ipairs(players) do
                reply(string.format("[%d] %s (%g, %g)", player.net_id, player.name, player.x, player.y))
            end
            return
        end
        local net_id = math.tointeger(tonumber(args[2]))
        if not net_id then
            reply("Invalid NetID format.")
            return
        end
        for _, player in ipairs(players) do
            if player.net_id == net_id then
                reply(string.format("Player Info (NetID: %d):", net_id))
                reply(string.format("Name: %s (User: %d)", player.name, player.user_id))
                reply(string.format("Pos: %g, %g (tile %d, %d)", player.x, player.y, player.tile_x, player.tile_y))
                reply(string.format("Country: %s", player.country))
                return
            end
        end
        reply(string.format("Player with NetID %d not found.", net_id))
    elseif what == "world" then
        local here = gtproxy.world.info()
        reply("World Info:")
        reply(string.format("Name: %s", here.in_world and here.name or "-"))
        reply(string.format("Size: %dx%d", here.width, here.height))
        reply(string.format("Tiles: %d", here.width * here.height))
        reply(string.format("Objects: %d", #gtproxy.world.objects()))
        reply(string.format("Player Count: %d", #gtproxy.world.players()))
    elseif what == "tile" then
        local x, y = math.tointeger(tonumber(args[2] or "")), math.tointeger(tonumber(args[3] or ""))
        local tile = x and y and gtproxy.world.tile(x, y)
        if not tile then
            reply(string.format("No tile %s, %s in this world.", args[2] or "?", args[3] or "?"))
            return
        end
        reply(string.format("Tile %d, %d: fg %d, bg %d, flags 0x%04X, %s", x, y, tile.fg, tile.bg, tile.flags, tile.cell))
        reply(tile.text)
    elseif what == "objects" or what == "object" then
        local objects = gtproxy.world.objects()
        reply(string.format("Objects: %d", #objects))
        for i, object in ipairs(objects) do
            if i > 30 then
                reply(string.format("... and %d more", #objects - 30))
                break
            end
            local item = gtproxy.items.get(object.item)
            reply(string.format("#%d %s (%d) x%d at %g, %g", object.object_id, item and item.name or "?", object.item, object.amount, object.x, object.y))
        end
    elseif what then
        reply(string.format("Unknown debug target: %s", sanitize(what)))
    else
        reply(string.format("Usage: %sdebug <player|world|tile|objects>", P))
    end
end

gtproxy.command("pos", "Show your position (tile x, y)", function(args, ctx) cmd_pos(args, ctx.reply) end)
gtproxy.command("players", "List the players you can see in this world", function(args, ctx) cmd_players(args, ctx.reply) end)
gtproxy.command("world", "Show information about this world", function(args, ctx) cmd_world(args, ctx.reply) end)
gtproxy.command("find", "Find items by name: find <text>", function(args, ctx) cmd_find(args, ctx.reply) end)
gtproxy.command("item", "Show an item: item <id>", function(args, ctx) cmd_item(args, ctx.reply) end)
gtproxy.command("debug", "Show debug information: debug <player [netID]|world|tile x y|objects>", function(args, ctx) cmd_debug(args, ctx.reply) end)

---------------------------------------------------------------------------
-- The Info page of /proxy
---------------------------------------------------------------------------

local function info_page()
    local d = gtproxy.dialog():header("Info", ICON_INFO)
    if gtproxy.world.info().in_world then
        d:button("players", "Players in this world")
            :button("world_info", "World info")
            :button("pos", "My position")
    end
    return d:spacer()
        :input("find", "Find item by name:", "", 30)
        :button("find_item", "Search")
        :input("item_id", "Item ID:", "", 6)
        :button("item_info", "Show item")
        :spacer()
        :button("commands", "All proxy commands")
        :button("page_main", "<< Menu")
        :finish("gtproxy_info")
end

gtproxy.menu.add({
    title = "Info",
    icon = ICON_INFO,
    button = "page_info",
    label = "Players, world, item search",
    order = 40,
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

    if button == "page_info" then gtproxy.game.dialog(info_page())
    elseif button == "players" then cmd_players({}, message)
    elseif button == "world_info" then cmd_world({}, message)
    elseif button == "pos" then cmd_pos({}, message)
    elseif button == "find_item" then with_text(cmd_find, "find", "`4Oops: ``Type part of an item name first.")
    elseif button == "item_info" then with_text(cmd_item, "item_id", "`4Oops: ``Type an item ID first.")
    end
end)
