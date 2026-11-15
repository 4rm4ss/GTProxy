-- World scan (module API 3, see docs/modules.md).
--
-- /growscan [name|ID]: every item of the world in a table (map window tab
-- "Скан мира"): blocks (foreground), backgrounds and what lies on the ground
-- (stacks and pieces) counted apart; gems on the ground apart from the rest.
-- Search by name or ID, sort by any column, a click on a row marks every
-- place of the item on the map. Made from the world the proxy holds, and
-- made again when blocks are placed or broken and things dropped or picked
-- up. A new world starts a new scan: nothing of the old one stays.
--
-- /track [on|off|clear]: the journal (tab "Журнал"): time, item, amount,
-- place. "Поднял игрок X" only when the server named the player who took it;
-- otherwise "Объект исчез".
--
-- /render: a PNG of the world as the map window shows it (with the marks of
-- the modules), saved as data/renders/<WORLD>_<date>.png.

local scan = gtproxy.load("scan.lua")
local PANEL_SCAN, PANEL_LOG = "scan", "journal"

local query = ""
local selected = nil        -- item id
local tracking = gtproxy.setting("track_on_start") == true
local journal = {}          -- newest last
local journal_world = nil
local dirty_timer = nil
local last_render = nil
local commands = {}

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

local function world()
    local info = gtproxy.world.info()
    return info.in_world and info.name or nil
end

---------------------------------------------------------------------------
-- The scan
---------------------------------------------------------------------------

local function places(id)
    local list = {}
    for _, hit in ipairs(gtproxy.world.find_tiles(id)) do
        list[#list + 1] = { x = hit.x, y = hit.y, kind = hit.background and "фон" or "блок" }
    end
    for _, o in ipairs(gtproxy.world.objects()) do
        if o.item == id then
            list[#list + 1] = { x = math.floor((o.x + 8) / 32), y = math.floor((o.y + 8) / 32), kind = "лежит x" .. o.amount }
        end
    end
    return list
end

local function show_places()
    if not selected or not world() then
        gtproxy.map.overlay(PANEL_SCAN, nil)
        return 0
    end
    local list = places(selected)
    local limit = math.max(1, math.floor(tonumber(gtproxy.setting("marks_limit")) or 200))
    local markers = {}
    for i, p in ipairs(list) do
        if i > limit then
            break
        end
        markers[#markers + 1] = { x = p.x, y = p.y, label = i == 1 and shown_name(selected) or "", color = p.kind:find("лежит", 1, true) and "#ffcf4a" or "#4fc3ff" }
    end
    gtproxy.map.overlay(PANEL_SCAN, #markers > 0 and { markers = markers } or nil)
    return #list
end

local function refresh_scan()
    local name = world()
    if not name then
        gtproxy.panel.set(PANEL_SCAN, { title = "Скан мира", note = "Вы не в мире: сканировать нечего.", columns = {}, rows = {} })
        gtproxy.map.overlay(PANEL_SCAN, nil)
        return nil
    end
    local info = gtproxy.world.info()
    local rows_data, totals = scan.count(gtproxy.world.summary(), gtproxy.world.objects())
    local rows = {}
    for _, r in ipairs(rows_data) do
        local item = item_name(r.id)
        if scan.matches(query, r.id, item) then
            local first = nil
            if r.id == selected then
                local list = places(r.id)
                first = list[1]
            end
            rows[#rows + 1] = {
                id = tostring(r.id),
                cells = { string.format("%s (%d)", item or ("предмет " .. r.id), r.id), r.fg, r.bg, r.stacks, r.pieces },
                sort = { item or "", r.fg, r.bg, r.stacks, r.pieces },
                tile = first and { x = first.x, y = first.y } or nil,
                selected = r.id == selected,
                style = "click",
            }
        end
    end
    local gems = totals.gems == nil and "неизвестно" or string.format("%d (кучек %d)", totals.gems, totals.gem_stacks)
    local note = {
        string.format("Мир %s, %dx%d. Видов предметов: %d.", name, info.width, info.height, totals.kinds),
        string.format("Блоков: %d · фона: %d · на земле: кучек %d, всего %d шт. · гемы на земле: %s", totals.fg, totals.bg, totals.stacks, totals.pieces, gems),
        "Строка таблицы — все места предмета на карте.",
    }
    if selected then
        note[#note + 1] = string.format("Выбрано: %s — мест на карте: %d.", shown_name(selected), #places(selected))
    end
    gtproxy.panel.set(PANEL_SCAN, {
        title = "Скан мира",
        note = table.concat(note, "\n"),
        search = { value = query, placeholder = "Название или ID (пусто — все)" },
        buttons = { { id = "render", label = "PNG мира" }, { id = "clear", label = "Снять отметки" } },
        columns = { "Предмет", "Блоки", "Фон", "Кучек на земле", "Шт. на земле" },
        rows = rows,
        empty = query ~= "" and "В мире нет такого предмета." or "Мир пуст.",
    })
    show_places()
    return totals
end

-- Many changes at once (a world loading, someone breaking fast): one scan.
local function soon()
    if dirty_timer then
        return
    end
    dirty_timer = gtproxy.after(400, function()
        dirty_timer = nil
        refresh_scan()
    end)
end

---------------------------------------------------------------------------
-- The journal
---------------------------------------------------------------------------

local function refresh_journal()
    local rows = {}
    for i = #journal, 1, -1 do
        local j = journal[i]
        rows[#rows + 1] = {
            id = tostring(i),
            cells = { j.time, j.item_text, j.amount, string.format("%d, %d", j.x, j.y), j.text },
            sort = { j.at, j.item_text, j.amount, j.y * 10000 + j.x, j.text },
            tile = { x = j.x, y = j.y },
        }
    end
    gtproxy.panel.set(PANEL_LOG, {
        title = "Журнал",
        note = string.format("%s · мир %s · записей %d. «Поднял игрок» — только когда сервер назвал игрока; иначе «Объект исчез».",
            tracking and "Запись идёт" or "Запись выключена (/track on)", journal_world or "—", #journal),
        buttons = { { id = tracking and "off" or "on", label = tracking and "Остановить" or "Начать запись" }, { id = "clear", label = "Очистить" } },
        columns = { "Время", "Предмет", "Шт.", "Коорд.", "Событие" },
        rows = rows,
        empty = "Пока ничего не происходило.",
    })
end

local function add_journal(e)
    local name = world()
    if not name then
        return
    end
    if journal_world ~= name then
        journal, journal_world = {}, name
    end
    local item_text = e.known and string.format("%s (%d)", shown_name(e.item), e.item) or "?"
    local entry = {
        at = gtproxy.time(), time = gtproxy.date():sub(12), item_text = item_text,
        amount = e.known and e.amount or "?", x = e.x, y = e.y,
        text = scan.journal_text(e, shown_name(e.item)),
    }
    journal[#journal + 1] = entry
    local size = math.max(10, math.floor(tonumber(gtproxy.setting("journal_size")) or 300))
    while #journal > size do
        table.remove(journal, 1)
    end
    gtproxy.log.info(string.format("track %s: %s at %d,%d", name, entry.text, e.x, e.y))
    refresh_journal()
end

---------------------------------------------------------------------------
-- Events
---------------------------------------------------------------------------

gtproxy.on("tile", function(e)
    if e.cause ~= "updated" then
        soon()
    end
end)

gtproxy.on("object", function(e)
    soon()
    if tracking then
        add_journal(e)
    end
end)

gtproxy.on("world_enter", function(e)
    selected = nil
    journal, journal_world = {}, e.name
    gtproxy.map.overlay(PANEL_SCAN, nil)
    soon()
    refresh_journal()
end)

gtproxy.on("world_menu", function()
    selected = nil
    gtproxy.map.overlay(PANEL_SCAN, nil)
    refresh_scan()
end)

gtproxy.on("disconnect", function()
    selected = nil
    gtproxy.cancel(dirty_timer)
    dirty_timer = nil
    gtproxy.map.overlay(PANEL_SCAN, nil)
    gtproxy.panel.remove(PANEL_SCAN)
end)

local function render()
    local path, why = gtproxy.map.render()
    if not path then
        return nil, why
    end
    last_render = path
    return path
end

gtproxy.on("panel", function(e)
    if e.panel == PANEL_SCAN then
        if e.action == "search" then
            query = e.value or ""
            refresh_scan()
            return { ok = true, message = "" }
        elseif e.action == "select" then
            selected = tonumber(e.row)
            local count = 0
            if selected then
                count = #places(selected)
            end
            refresh_scan()
            return { ok = true, message = string.format("%s: мест на карте %d", shown_name(selected or 0), count) }
        elseif e.action == "clear" then
            selected = nil
            refresh_scan()
            return { ok = true, message = "Отметки сняты" }
        elseif e.action == "render" then
            local path, why = render()
            return { ok = path ~= nil, message = path and ("PNG: " .. path) or ("PNG не сделан: " .. why) }
        end
    elseif e.panel == PANEL_LOG then
        if e.action == "on" or e.action == "off" then
            tracking = e.action == "on"
            refresh_journal()
            return { ok = true, message = tracking and "Запись включена" or "Запись остановлена" }
        elseif e.action == "clear" then
            journal = {}
            refresh_journal()
            return { ok = true, message = "Журнал очищен" }
        end
    end
end)

---------------------------------------------------------------------------
-- Commands
---------------------------------------------------------------------------

local function cmd_growscan(args, ctx)
    query = table.concat(args, " ")
    local totals = refresh_scan()
    if not totals then
        ctx.reply("`4Oops: ``you are not in a world.")
        return
    end
    ctx.reply(string.format("`o%s: `w%d`` kinds, `w%d`` blocks, `w%d`` backgrounds, on the ground `w%d`` stacks / `w%d`` pieces, gems `w%s``.",
        world(), totals.kinds, totals.fg, totals.bg, totals.stacks, totals.pieces, totals.gems == nil and "unknown" or tostring(totals.gems)))
    if query ~= "" then
        local rows = scan.count(gtproxy.world.summary(), gtproxy.world.objects())
        local shown = 0
        for _, r in ipairs(rows) do
            if scan.matches(query, r.id, item_name(r.id)) then
                shown = shown + 1
                if shown <= 6 then
                    ctx.reply(string.format("`o%s (%d): blocks %d, background %d, ground %d stacks / %d pcs",
                        gtproxy.text.sanitize(game_name(r.id)), r.id, r.fg, r.bg, r.stacks, r.pieces))
                end
            end
        end
        if shown == 0 then
            ctx.reply(string.format("`oNo `w%s`` in this world.", gtproxy.text.sanitize(query)))
        end
    end
    ctx.reply("`oThe full table: map window (" .. gtproxy.prefix .. "map), world_scan tab.")
end

local function cmd_track(args, ctx)
    local word = (args[1] or ""):lower()
    if word == "on" or word == "" and not tracking then
        tracking = true
    elseif word == "off" or word == "" and tracking then
        tracking = false
    elseif word == "clear" then
        journal = {}
    else
        ctx.reply("`oUsage: track [on|off|clear]")
        return
    end
    refresh_journal()
    ctx.reply(string.format("`oItem journal %s (%d entries). Map window, journal tab.", tracking and "`2on``" or "`4off``", #journal))
end

local function cmd_render(_, ctx)
    local path, why = render()
    if not path then
        ctx.reply("`4No PNG: ``" .. gtproxy.text.sanitize(why))
        return
    end
    ctx.reply("`oSaved `w" .. gtproxy.text.sanitize(path) .. "``")
end

local function add_command(name, description, run)
    for _, candidate in ipairs({ name, "scan_" .. name }) do
        if gtproxy.command(candidate, description, run) then
            commands[name] = candidate
            if candidate ~= name then
                gtproxy.log.warn(string.format("/%s is taken by another command; world_scan uses /%s", name, candidate))
            end
            return
        end
    end
    gtproxy.log.warn(string.format("world_scan: no command for %s", name))
end

add_command("growscan", "Everything in this world counted: growscan [name|ID]", cmd_growscan)
add_command("track", "Journal of items dropped / picked up: track [on|off|clear]", cmd_track)
add_command("render", "Save a PNG of this world", cmd_render)

refresh_scan()
refresh_journal()
