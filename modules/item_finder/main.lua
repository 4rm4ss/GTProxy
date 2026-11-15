-- Item finder (module API 3, see docs/modules.md).
--
-- /pfind <name|ID|any>: of the items lying in the world (dropped objects)
-- that match, the one you can reach by the shortest way - the way your body
-- can move (no squeezing diagonally between two blocks), not a straight
-- line. It is marked on the map with the tile you end on and the way there.
-- "Клетка на карте" (or /pfind here) lets you click any tile for its way.
--
-- "Идти к цели" walks there only when a walk on foot (standing, jumping,
-- falling) to it can be worked out; otherwise the way is only shown and the
-- tab says walking is not available and why. "Стоп" stops.
--
-- The target and its way are worked out again when the item is picked up
-- or goes away, when a block opens or closes the way, and dropped when you
-- leave the world.

local find = gtproxy.load("find.lua")
local P = gtproxy.prefix
local PANEL = "items"

local query = find.parse_query("any")
local target = nil      -- { kind = "object"|"cell", object_id, item, amount, tx, ty }
local way = nil         -- { ok, tiles, steps, reason }
local status = ""
local walking = false
local list_cache = {}
local last_tile = nil
local command_name = nil

local function item_name(id)
    local info = gtproxy.items.get(id)
    return info and info.name or nil
end

local function shown_name(id)
    return item_name(id) or string.format("предмет %d", id)
end

local function game_name(id)
    return item_name(id) or string.format("item %d", id)
end

local function in_world()
    return gtproxy.world.info().in_world
end

-- Matching objects with the steps to each (one search for all).
local function matching()
    local list, spots = {}, {}
    for _, o in ipairs(gtproxy.world.objects()) do
        if find.matches(query, o.item, item_name) then
            local tx, ty = find.tile_of(o)
            list[#list + 1] = { object_id = o.object_id, item = o.item, amount = o.amount, x = o.x, y = o.y, tx = tx, ty = ty, steps = -1 }
            spots[#spots + 1] = { x = tx, y = ty }
        end
    end
    if #spots > 0 then
        local steps = gtproxy.map.distances(spots)
        for i, s in ipairs(steps) do
            list[i].steps = s
        end
    end
    return find.sort(list)
end

local function object_by_id(id)
    for _, o in ipairs(gtproxy.world.objects()) do
        if o.object_id == id then
            return o
        end
    end
    return nil
end

---------------------------------------------------------------------------
-- The map and the tab
---------------------------------------------------------------------------

local function show_marks(list)
    local markers, route = {}, nil
    for i, o in ipairs(list) do
        if i > 80 then
            break
        end
        if not (target and target.kind == "object" and target.object_id == o.object_id) then
            markers[#markers + 1] = { x = o.tx, y = o.ty, label = "", color = o.steps >= 0 and "#7ee787" or "#8b949e" }
        end
    end
    if target then
        local label = target.kind == "object" and string.format("%s x%d", shown_name(target.item), target.amount) or "цель"
        markers[#markers + 1] = { x = target.tx, y = target.ty, label = label, color = "#ffcf4a" }
        if way and way.ok and #way.tiles > 0 then
            route = way.tiles
            local last = way.tiles[#way.tiles]
            markers[#markers + 1] = { x = last.x, y = last.y, label = "конец пути", color = "#c77dff" }
        end
    end
    if #markers == 0 then
        gtproxy.map.overlay(PANEL, nil)
    else
        gtproxy.map.overlay(PANEL, { markers = markers, route = route, route_color = "#c77dff" })
    end
end

local function target_text()
    if not target then
        return "Цель не выбрана."
    end
    local where = string.format("%d, %d", target.tx, target.ty)
    local what = target.kind == "object" and string.format("%s x%d (ID %d) в %s", shown_name(target.item), target.amount, target.item, where)
        or ("клетка " .. where)
    if way and way.ok then
        return string.format("Цель: %s — путь %d шаг.", what, way.steps)
    end
    return string.format("Цель: %s — пути нет (%s).", what, way and way.reason or "?")
end

local function refresh()
    if not in_world() then
        gtproxy.panel.set(PANEL, { title = "Предметы", note = "Вы не в мире.", columns = {}, rows = {} })
        gtproxy.map.overlay(PANEL, nil)
        return
    end
    local list = matching()
    list_cache = list
    local me = gtproxy.world.info().me
    last_tile = me and string.format("%d,%d", me.tile_x, me.tile_y) or nil

    local limit = math.max(10, math.floor(tonumber(gtproxy.setting("list_limit")) or 100))
    local rows = {}
    for i, o in ipairs(list) do
        if i > limit then
            break
        end
        rows[#rows + 1] = {
            id = tostring(o.object_id),
            cells = { string.format("%s (%d)", shown_name(o.item), o.item), o.amount, string.format("%d, %d", o.tx, o.ty), o.steps >= 0 and (o.steps .. " шаг.") or "нет пути" },
            sort = { shown_name(o.item), o.amount, o.ty * 10000 + o.tx, o.steps >= 0 and o.steps or 1e9 },
            tile = { x = o.tx, y = o.ty },
            style = o.steps >= 0 and "ok" or "muted",
            selected = target ~= nil and target.kind == "object" and target.object_id == o.object_id,
        }
    end

    local reachable = 0
    for _, o in ipairs(list) do
        if o.steps >= 0 then
            reachable = reachable + 1
        end
    end
    local note = {
        string.format("Поиск «%s»: лежит %d, из них доступно %d.", query.text, #list, reachable),
        target_text(),
    }
    if status ~= "" then
        note[#note + 1] = status
    end
    gtproxy.panel.set(PANEL, {
        title = "Предметы",
        note = table.concat(note, "\n"),
        search = { value = query.any and "any" or query.text, placeholder = "Название, ID или any" },
        buttons = {
            { id = "go", label = "Идти к цели" },
            { id = "stop", label = "Стоп" },
            { id = "pick", label = "Клетка на карте" },
            { id = "clear", label = "Убрать цель" },
        },
        columns = { "Предмет", "Шт.", "Коорд.", "Путь" },
        rows = rows,
        empty = "Таких предметов на земле нет.",
    })
    show_marks(list)
end

-- The way to the target again (after a change in the world).
local function update_way()
    if not target then
        way = nil
        return
    end
    way = gtproxy.map.route(target.tx, target.ty)
end

local function set_target(new_target, why)
    target = new_target
    status = why or ""
    update_way()
    refresh()
end

-- The nearest match becomes the target.
local function search(text)
    query = find.parse_query(text)
    local list = matching()
    local best = find.nearest(list)
    if best then
        set_target({ kind = "object", object_id = best.object_id, item = best.item, amount = best.amount, tx = best.tx, ty = best.ty })
    else
        set_target(nil, #list > 0 and "Есть подходящие предметы, но ни до одного нет пути." or "")
    end
    return list, best
end

---------------------------------------------------------------------------
-- Walking
---------------------------------------------------------------------------

local function walk_target()
    if not target then
        return false, "Нет цели"
    end
    if not way or not way.ok then
        return false, "До цели нет пути"
    end
    local gravity = gtproxy.setting("walk_gravity") == true
    local foot = gtproxy.map.route(target.tx, target.ty, { walk = true, gravity = gravity })
    if not foot.ok then
        status = "Путь показан, но ходьба недоступна: " .. (foot.reason ~= "" and foot.reason or "пеший путь не найден") .. "."
        refresh()
        return false, "Ходьба недоступна: " .. (foot.reason ~= "" and foot.reason or "пеший путь не найден")
    end
    local result = gtproxy.walk.to(target.tx, target.ty, { gravity = gravity })
    if not result.ok then
        status = "Ходьба не началась: " .. result.message
        refresh()
        return false, result.message
    end
    walking = true
    status = string.format("Идём к %d, %d (%d шаг. по пешему пути).", target.tx, target.ty, foot.steps)
    refresh()
    return true, status
end

local function stop_walk(why)
    if walking or gtproxy.walk.active() then
        gtproxy.walk.stop()
    end
    walking = false
    if why then
        status = why
    end
end

gtproxy.on("walk_end", function(e)
    if walking then
        walking = false
        status = "Ходьба: " .. (e.message ~= "" and e.message or "закончена")
        refresh()
    end
end)

---------------------------------------------------------------------------
-- Changes in the world
---------------------------------------------------------------------------

gtproxy.on("object", function(e)
    if not in_world() then
        return
    end
    if target and target.kind == "object" and e.object_id == target.object_id then
        if e.kind == "collected" or e.kind == "removed" then
            local who = e.by_me and "вы подняли" or (e.by_name and e.by_name ~= "" and ("поднял игрок " .. e.by_name)) or "объект исчез"
            stop_walk()
            local before = target
            local _, best = search(query.any and "any" or query.text)
            status = string.format("Цель %s x%d: %s.%s", shown_name(before.item), before.amount, who,
                best and " Выбрана следующая ближайшая." or " Других доступных нет.")
            refresh()
            return
        elseif e.kind == "changed" then
            target.amount = e.amount
        end
    end
    refresh()
end)

gtproxy.on("tile", function(e)
    if e.cause ~= "placed" and e.cause ~= "broken" and e.cause ~= "changed" then
        return
    end
    if target then
        local had = way and way.ok
        update_way()
        if had and not way.ok then
            stop_walk("Путь к цели закрылся (" .. way.reason .. ").")
        elseif not had and way.ok then
            status = "Путь к цели открылся."
        end
    end
    refresh()
end)

local function reset(why)
    stop_walk()
    target, way, status, list_cache = nil, nil, why or "", {}
    gtproxy.map.overlay(PANEL, nil)
end

gtproxy.on("world_menu", function()
    reset()
    refresh()
end)
gtproxy.on("world_enter", function()
    reset()
    refresh()
end)
gtproxy.on("disconnect", function()
    reset()
    gtproxy.panel.remove(PANEL)
end)

-- The way follows you while you move.
gtproxy.every(1000, function()
    local info = gtproxy.world.info()
    if not info.in_world or not info.me then
        return
    end
    local here = string.format("%d,%d", info.me.tile_x, info.me.tile_y)
    if here ~= last_tile then
        update_way()
        refresh()
    end
end)

---------------------------------------------------------------------------
-- The tab, the map, the command
---------------------------------------------------------------------------

gtproxy.on("pick", function(e)
    if not e.in_world then
        return { ok = false, message = "Вы не в мире" }
    end
    stop_walk()
    set_target({ kind = "cell", tx = e.x, ty = e.y })
    if way.ok then
        return { ok = true, message = string.format("%d, %d: путь %d шаг.", e.x, e.y, way.steps) }
    end
    return { ok = false, message = string.format("%d, %d: пути нет — %s", e.x, e.y, way.reason) }
end)

gtproxy.on("panel", function(e)
    if e.panel ~= PANEL then
        return
    end
    if e.action == "search" then
        stop_walk()
        local list, best = search(e.value)
        if best then
            return { ok = true, message = string.format("Ближайший: %s x%d, %d шаг.", shown_name(best.item), best.amount, best.steps) }
        end
        return { ok = false, message = #list > 0 and "Ни до одного нет пути" or "Ничего не найдено" }
    elseif e.action == "select" then
        local id = tonumber(e.row)
        for _, o in ipairs(list_cache) do
            if o.object_id == id then
                stop_walk()
                set_target({ kind = "object", object_id = o.object_id, item = o.item, amount = o.amount, tx = o.tx, ty = o.ty })
                return { ok = way.ok, message = way.ok and string.format("Путь %d шаг.", way.steps) or ("Пути нет: " .. way.reason) }
            end
        end
        refresh()
        return { ok = false, message = "Этого предмета уже нет" }
    elseif e.action == "go" then
        local ok, message = walk_target()
        return { ok = ok, message = message }
    elseif e.action == "stop" then
        stop_walk("Остановлено.")
        refresh()
        return { ok = true, message = "Стоп" }
    elseif e.action == "pick" then
        gtproxy.map.pick("путь к клетке (item_finder)")
        return { ok = true, message = "Щёлкните клетку на карте" }
    elseif e.action == "clear" then
        reset()
        refresh()
        return { ok = true, message = "Цель убрана" }
    end
end)

local function cmd_pfind(args, ctx)
    if not in_world() then
        ctx.reply("`4Oops: ``you are not in a world.")
        return
    end
    local text = table.concat(args, " ")
    if text:lower() == "stop" then
        stop_walk("Остановлено.")
        refresh()
        ctx.reply("`oStopped.")
        return
    end
    if text:lower() == "go" then
        local ok, message = walk_target()
        ctx.reply(ok and "`oWalking to the target." or "`4Not walking: ``no way on foot can be proven (or no target); the way and the reason are in the map window.")
        return
    end
    stop_walk()
    local list, best = search(text)
    if #list == 0 then
        ctx.reply(string.format("`oNothing matching `w%s`` lies in this world.", gtproxy.text.sanitize(query.text)))
    elseif not best then
        ctx.reply(string.format("`o%d matching item(s) lie here, but none can be reached.", #list))
    else
        ctx.reply(string.format("`oNearest: `w%s x%d`` at `w%d, %d``, %d steps (%d matching). %spfind go walks there; the way is in %smap.",
            gtproxy.text.sanitize(game_name(best.item)), best.amount, best.tx, best.ty, best.steps, #list, P, P))
    end
end

for _, name in ipairs({ "pfind", "item_pfind" }) do
    if gtproxy.command(name, "Nearest reachable dropped item: pfind <name|ID|any> (pfind go / pfind stop)", cmd_pfind) then
        command_name = name
        break
    end
end
if command_name ~= "pfind" then
    gtproxy.log.warn("/pfind is taken by another command; item_finder uses " .. (command_name and ("/" .. command_name) or "no command"))
end

refresh()
