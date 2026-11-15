-- World moderation (module API 3, see docs/modules.md).
--
-- The players of the world in the map window's tab "Модерация": choose one,
-- several, or everyone inside a zone picked on the map, and pull, kick or
-- ban them. Every action is what you would do by hand: the player's window
-- (the wrench on the player) is asked from the server and its own button is
-- pressed, so the server checks your rights and decides. A player is acted
-- on by the netID of this session, checked against the account ID (user ID)
-- and name before each step; the name is only shown.
--
-- Each action ends as one of:
--   «Подтверждено»   the server did it (kick/ban: the player left; pull: the
--                    player now stands next to you; or the server said so)
--   «Нет прав»       the player's window has no such button for you
--   «Недоступно»     the server did not open the player's window
--   «Отклонено»      the server refused (its message is shown)
--   «Результат неизвестен»  sent, but no answer in result_timeout_ms
-- Several players: one after another, each with its own result; "Стоп"
-- stops the rest.
--
-- Lock access: "Замок на карте" + a click on a lock shows where it is, its
-- kind, owner and who has access (and who of them is here); access is given
-- or taken through the lock's own window (wrench), one player at a time.
--
-- Entry rules / blacklist (config/modules/world_moderation.json, off by
-- default): rules_enabled, dry_run (only writes what it would do), rules,
-- exceptions (see rules.lua). Everything done is in the tab "Журнал модерации"
-- and the proxy log.
--
-- Other modules (autohost) use the same operations: gtproxy.call(
-- "world_moderation", "players_in", zone), "run", "batch", "stop".

local rules = gtproxy.load("rules.lua")
local P = gtproxy.prefix
local PANEL, PANEL_LOG = "moderation", "moderation_log"
local NAMES = { pull = "Pull", kick = "Kick", ban = "Ban", give = "Дать доступ", take = "Забрать доступ" }

local selected = {}         -- net_id -> true
local zone = nil            -- { x1, y1, x2, y2 }
local corner = nil          -- first corner while picking a zone
local picking = nil         -- "zone1" | "zone2" | "lock"
local plan = nil            -- prepared, waiting for confirmation
local batches = {}          -- id -> batch
local queue = {}            -- batch ids waiting / running (first runs)
local next_batch = 1
local op = nil              -- the step in flight
local lock_sel = nil        -- { x, y }
local journal = {}
local last_result = {}      -- net_id -> text, for the table
local active_rules, active_exceptions, rules_problem = {}, { names = {}, ids = {} }, nil
local checked_entry = {}    -- net_id -> true: entry rules looked at
local gap_timer = nil

local function clean(name)
    return gtproxy.text.strip_colors(name or "")
end

local function world()
    local info = gtproxy.world.info()
    return info.in_world and info.name or nil
end

local function ms(name, low, high)
    return math.max(low, math.min(high, math.floor(tonumber(gtproxy.setting(name)) or low)))
end

local function player_by_net(net_id)
    for _, p in ipairs(gtproxy.world.players()) do
        if p.net_id == net_id then
            return p
        end
    end
    return nil
end

local function player_text(t)
    return string.format("%s (#%s)", t.name, (t.user_id and t.user_id > 0) and tostring(t.user_id) or "?")
end

---------------------------------------------------------------------------
-- Settings: the rules
---------------------------------------------------------------------------

local function load_rules()
    local parsed, why = rules.parse_rules(gtproxy.setting("rules"))
    local exceptions, why2 = rules.parse_exceptions(gtproxy.setting("exceptions"))
    if not parsed or not exceptions then
        rules_problem = string.format("config/modules/world_moderation.json: %s: %s — действуют прежние правила",
            parsed and "exceptions" or "rules", why or why2)
        gtproxy.log.error(rules_problem)
        return false
    end
    active_rules, active_exceptions, rules_problem = parsed, exceptions, nil
    return true
end

---------------------------------------------------------------------------
-- Journal
---------------------------------------------------------------------------

local refresh

local function log_entry(action, target, result, detail, source)
    local entry = {
        at = gtproxy.time(), time = gtproxy.date():sub(12), world = world() or "—",
        action = NAMES[action] or action, target = target, result = result, detail = detail or "", source = source or "вы",
    }
    journal[#journal + 1] = entry
    while #journal > 300 do
        table.remove(journal, 1)
    end
    gtproxy.log.info(string.format("moderation %s: %s %s -> %s%s (%s)", entry.world, entry.action, target, result,
        detail and detail ~= "" and (": " .. detail) or "", entry.source))
end

---------------------------------------------------------------------------
-- Running actions: one step at a time
---------------------------------------------------------------------------

local start_next

local function batch_done(b)
    for _, item in ipairs(b.items) do
        if item.status == "Ожидает" or item.status == "Окно…" or item.status == "Отправлено" then
            return false
        end
    end
    return true
end

local function end_step(status, detail)
    local o = op
    if not o then
        return
    end
    gtproxy.cancel(o.timer)
    gtproxy.cancel(o.poll)
    op = nil
    local item = o.item
    item.status, item.detail = status, detail or ""
    local t = item.target
    if t.net_id then
        last_result[t.net_id] = status
    end
    log_entry(o.batch.action, item.target_text, status, detail, o.batch.source)
    gtproxy.cancel(gap_timer)
    gap_timer = gtproxy.after(ms("gap_ms", 50, 5000), function()
        gap_timer = nil
        start_next()
    end)
    refresh()
end

local function skip(item, b, status, detail)
    item.status, item.detail = status, detail
    if item.target.net_id then
        last_result[item.target.net_id] = status
    end
    log_entry(b.action, item.target_text, status, detail, b.source)
end

-- Is the target still the same player, here (and in the zone)?
local function recheck(b, item)
    local t = item.target
    if world() ~= b.world then
        return false, "другой мир"
    end
    if b.action == "give" or b.action == "take" then
        local tile = gtproxy.world.tile(b.lock.x, b.lock.y)
        if not tile or tile.extra ~= "lock" or tile.fg ~= b.lock.item then
            return false, "замка больше нет"
        end
        if b.action == "take" then
            return true
        end
    end
    local p = player_by_net(t.net_id)
    if not p then
        return false, "игрок ушёл из мира"
    end
    if (t.user_id and t.user_id > 0 and p.user_id ~= t.user_id) or clean(p.name):lower() ~= t.name:lower() then
        return false, "под этим netID теперь другой игрок"
    end
    if p.is_local then
        return false, "это вы"
    end
    if b.zone and not rules.inside(p.tile_x, p.tile_y, b.zone) then
        return false, string.format("игрок вышел из зоны (сейчас %d, %d)", p.tile_x, p.tile_y)
    end
    return true
end

local function open_window(b, item)
    local o = { batch = b, item = item, stage = "window" }
    op = o
    item.status = "Окно…"
    local sent
    if b.action == "give" or b.action == "take" then
        sent = gtproxy.server.wrench(b.lock.x, b.lock.y)
    else
        sent = gtproxy.server.text(string.format("action|wrench\n|netid|%d\n", item.target.net_id))
    end
    if not sent then
        end_step("Недоступно", "нет связи с сервером")
        return
    end
    o.timer = gtproxy.after(ms("window_timeout_ms", 500, 30000), function()
        if op == o and o.stage == "window" then
            o.timer = nil
            end_step("Недоступно", b.lock and "сервер не открыл окно замка (не ваш замок или далеко)" or "сервер не открыл окно игрока")
        end
    end)
    refresh()
end

start_next = function()
    if op then
        return
    end
    while #queue > 0 do
        local b = batches[queue[1]]
        local item = nil
        if b and not b.stopped then
            for _, it in ipairs(b.items) do
                if it.status == "Ожидает" then
                    item = it
                    break
                end
            end
        end
        if item then
            local ok, why = recheck(b, item)
            if ok then
                open_window(b, item)
                return
            end
            skip(item, b, why == "другой мир" and "Отменено" or "Пропущено", why)
        else
            if b and b.stopped then
                for _, it in ipairs(b.items) do
                    if it.status == "Ожидает" then
                        skip(it, b, "Отменено", "очередь остановлена")
                    end
                end
            end
            table.remove(queue, 1)
        end
    end
    refresh()
end

-- A new batch of one action for several targets.
local function new_batch(action, targets, options)
    options = options or {}
    local w = world()
    if not w then
        return nil, "вы не в мире"
    end
    if #targets == 0 then
        return nil, "никого не выбрано"
    end
    local b = { id = next_batch, action = action, world = w, source = options.source or "вы", zone = options.zone, lock = options.lock, items = {} }
    next_batch = next_batch + 1
    for _, t in ipairs(targets) do
        b.items[#b.items + 1] = { target = t, target_text = player_text(t), status = "Ожидает", detail = "" }
    end
    batches[b.id] = b
    queue[#queue + 1] = b.id
    start_next()
    return b.id
end

local function stop_all(why)
    for _, id in ipairs(queue) do
        local b = batches[id]
        if b then
            b.stopped = true
            for _, it in ipairs(b.items) do
                if it.status == "Ожидает" then
                    skip(it, b, "Отменено", why)
                end
            end
        end
    end
end

-- The player's or the lock's window the server sent for our step.
gtproxy.hook_call("OnDialogRequest", function(call)
    local o = op
    if not o or o.stage ~= "window" or type(call.args[1]) ~= "string" then
        return
    end
    local d = gtproxy.ui.parse_dialog(call.args[1])
    local b, item = o.batch, o.item

    if b.lock then
        if not d.name:lower():find("lock", 1, true) then
            return
        end
        local tx, ty = tonumber(d.embed.tilex or ""), tonumber(d.embed.tiley or "")
        if tx and ty and (tx ~= b.lock.x or ty ~= b.lock.y) then
            return
        end
        local values, field = {}, nil
        if b.action == "give" then
            field = d.pickers[1]
            if not field then
                end_step("Нет прав", "в окне замка нет выбора игрока (замок не ваш или сервер не даёт)")
                return "cancel"
            end
            values[field] = item.target.net_id
        else
            local id = tostring(item.target.user_id or "")
            for _, name in ipairs(d.checkbox_list) do
                if id ~= "" and (name == "checkbox_" .. id or name:match("_" .. id .. "$")) then
                    field = name
                end
            end
            if not field then
                end_step("Недоступно", "в окне замка нет этого игрока (у него нет доступа, или замок не ваш)")
                return "cancel"
            end
            values[field] = false
        end
        o.before_access = nil
        gtproxy.server.text(gtproxy.ui.answer(d, values))
    else
        local net = tonumber(d.embed.netID or d.embed.netid or "")
        if net and net ~= item.target.net_id then
            return
        end
        if not net and not d.name:lower():find("popup", 1, true) then
            return
        end
        local button = rules.button_for(d, b.action)
        if not button then
            end_step("Нет прав", string.format("в окне игрока нет кнопки %s", NAMES[b.action]))
            return "cancel"
        end
        gtproxy.server.text(gtproxy.ui.answer(d, {}, button))
    end

    o.stage = "sent"
    item.status = "Отправлено"
    gtproxy.cancel(o.timer)
    o.timer = gtproxy.after(ms("result_timeout_ms", 500, 60000), function()
        if op == o then
            o.timer = nil
            end_step("Результат неизвестен", "сервер не подтвердил вовремя; повторно не отправлялось")
        end
    end)
    if b.action == "pull" then
        o.poll = gtproxy.every(200, function()
            if op ~= o then
                return
            end
            local p, me = player_by_net(item.target.net_id), gtproxy.world.info().me
            if p and me and math.abs(p.tile_x - me.tile_x) <= 1 and math.abs(p.tile_y - me.tile_y) <= 1 then
                end_step("Подтверждено", string.format("игрок теперь в %d, %d", p.tile_x, p.tile_y))
            end
        end)
    end
    refresh()
    return "cancel"
end)

gtproxy.on("player_leave", function(e)
    selected[e.net_id] = nil
    checked_entry[e.net_id] = nil
    local o = op
    if o and o.stage == "sent" and o.item.target.net_id == e.net_id and (o.batch.action == "kick" or o.batch.action == "ban") then
        end_step("Подтверждено", "игрок покинул мир")
        return
    end
    refresh()
end)

gtproxy.on("console", function(e)
    local o = op
    if not o or o.stage ~= "sent" then
        return
    end
    local text = e.clean or ""
    if rules.is_refusal(text) then
        end_step("Отклонено", "сервер: " .. text)
        return
    end
    local lower = text:lower()
    if lower:find(o.item.target.name:lower(), 1, true) and (lower:find("pulled", 1, true) or lower:find("kicked", 1, true) or lower:find("banned", 1, true)) then
        end_step("Подтверждено", "сервер: " .. text)
    end
end)

-- Lock access: the lock's tile changes when the server applies it.
gtproxy.on("tile", function(e)
    local o = op
    if o and o.stage == "sent" and o.batch.lock and e.x == o.batch.lock.x and e.y == o.batch.lock.y then
        local tile = gtproxy.world.tile(e.x, e.y)
        if not tile or tile.extra ~= "lock" then
            end_step("Результат неизвестен", "замок исчез")
            return
        end
        local has = false
        for _, id in ipairs(tile.lock_access or {}) do
            if id == o.item.target.user_id then
                has = true
            end
        end
        if (o.batch.action == "give") == has then
            end_step("Подтверждено", has and "доступ есть" or "доступа больше нет")
        end
        return
    end
    if lock_sel and e.x == lock_sel.x and e.y == lock_sel.y then
        local tile = gtproxy.world.tile(e.x, e.y)
        if not tile or tile.extra ~= "lock" then
            lock_sel = nil
            if plan and plan.lock then
                plan = nil
            end
            stop_all("замка больше нет")
        end
        refresh()
    end
end)

---------------------------------------------------------------------------
-- Entry rules
---------------------------------------------------------------------------

local function check_entry(p)
    if p.is_local or checked_entry[p.net_id] or not gtproxy.setting("rules_enabled") then
        return
    end
    checked_entry[p.net_id] = true
    local name = clean(p.name)
    local rule = rules.match(active_rules, active_exceptions, world(), name, p.user_id)
    if not rule then
        return
    end
    local target = { net_id = p.net_id, user_id = p.user_id, name = name }
    if gtproxy.setting("dry_run") then
        log_entry(rule.action, player_text(target), "Проба (dry run)", "правило сработало бы", "правила входа")
        gtproxy.game.message(string.format("`oModeration dry run: would %s `w%s``.", rule.action, gtproxy.text.sanitize(name)))
        refresh()
        return
    end
    new_batch(rule.action, { target }, { source = "правила входа" })
end

gtproxy.on("player_join", function(e)
    if e.is_local then
        return
    end
    -- After the world has the player (position, IDs).
    gtproxy.after(300, function()
        local p = player_by_net(e.net_id)
        if p then
            check_entry(p)
        end
        refresh()
    end)
end)

---------------------------------------------------------------------------
-- The tab
---------------------------------------------------------------------------

local function lock_info()
    if not lock_sel then
        return nil
    end
    local tile = gtproxy.world.tile(lock_sel.x, lock_sel.y)
    if not tile or tile.extra ~= "lock" then
        return nil
    end
    local item = gtproxy.items.get(tile.fg)
    local here = {}
    for _, p in ipairs(gtproxy.world.players()) do
        here[p.user_id] = clean(p.name)
    end
    local access, present = {}, {}
    for _, id in ipairs(tile.lock_access or {}) do
        access[#access + 1] = here[id] and string.format("%s (#%d)", here[id], id) or ("#" .. id)
        if here[id] then
            present[#present + 1] = here[id]
        end
    end
    return {
        x = lock_sel.x, y = lock_sel.y, item = tile.fg, kind = item and item.name or ("предмет " .. tile.fg),
        owner = here[tile.lock_owner] and string.format("%s (#%d)", here[tile.lock_owner], tile.lock_owner) or ("#" .. tile.lock_owner),
        access = access, present = present,
    }
end

local function selected_targets()
    local list = {}
    for _, p in ipairs(gtproxy.world.players()) do
        if selected[p.net_id] and not p.is_local then
            list[#list + 1] = { net_id = p.net_id, user_id = p.user_id, name = clean(p.name) }
        end
    end
    return list
end

local function players_in(z)
    local list = {}
    for _, p in ipairs(gtproxy.world.players()) do
        if not p.is_local and rules.inside(p.tile_x, p.tile_y, z) then
            list[#list + 1] = { net_id = p.net_id, user_id = p.user_id, name = clean(p.name), tile_x = p.tile_x, tile_y = p.tile_y }
        end
    end
    return list
end

local function show_marks()
    local markers, areas = {}, {}
    for _, p in ipairs(gtproxy.world.players()) do
        if selected[p.net_id] then
            markers[#markers + 1] = { x = p.tile_x, y = p.tile_y, label = clean(p.name), color = "#ff7b72" }
        end
    end
    if zone then
        areas[#areas + 1] = { x1 = zone.x1, y1 = zone.y1, x2 = zone.x2, y2 = zone.y2, label = "зона модерации", color = "#ff7b72" }
    end
    if lock_sel then
        markers[#markers + 1] = { x = lock_sel.x, y = lock_sel.y, label = "замок", color = "#ffcf4a" }
    end
    if #markers == 0 and #areas == 0 then
        gtproxy.map.overlay(PANEL, nil)
    else
        gtproxy.map.overlay(PANEL, { markers = markers, areas = areas })
    end
end

local function batch_lines()
    local lines = {}
    for _, id in ipairs(queue) do
        local b = batches[id]
        if b then
            local done = 0
            for _, it in ipairs(b.items) do
                if it.status ~= "Ожидает" and it.status ~= "Окно…" and it.status ~= "Отправлено" then
                    done = done + 1
                end
            end
            lines[#lines + 1] = string.format("Идёт: %s — %d из %d (%s)", NAMES[b.action], done, #b.items, b.source)
        end
    end
    return lines
end

local function refresh_log()
    local rows = {}
    for i = #journal, 1, -1 do
        local j = journal[i]
        rows[#rows + 1] = {
            id = tostring(i),
            cells = { j.time, j.world, j.action, j.target, j.result, j.detail, j.source },
            sort = { j.at, j.world, j.action, j.target, j.result, j.detail, j.source },
            style = j.result == "Подтверждено" and "ok" or ((j.result == "Отклонено" or j.result == "Нет прав" or j.result == "Недоступно") and "bad" or ""),
        }
    end
    gtproxy.panel.set(PANEL_LOG, {
        title = "Журнал модерации",
        note = "Мир, действие, игрок, подтверждённый результат. Также в логе прокси.",
        columns = { "Время", "Мир", "Действие", "Игрок", "Результат", "Подробно", "Кто" },
        rows = rows,
        empty = "Пока ничего не делалось.",
    })
end

refresh = function()
    local w = world()
    if not w then
        gtproxy.panel.set(PANEL, { title = "Модерация", note = "Вы не в мире.", columns = {}, rows = {} })
        gtproxy.map.overlay(PANEL, nil)
        refresh_log()
        return
    end
    local rows = {}
    for _, p in ipairs(gtproxy.world.players()) do
        local inside = zone and rules.inside(p.tile_x, p.tile_y, zone)
        rows[#rows + 1] = {
            id = tostring(p.net_id),
            cells = {
                clean(p.name) .. (p.is_local and " (вы)" or ""), p.user_id > 0 and p.user_id or "?", p.net_id,
                string.format("%d, %d%s", p.tile_x, p.tile_y, inside and " · в зоне" or ""),
                selected[p.net_id] and "да" or "", last_result[p.net_id] or "",
            },
            sort = { clean(p.name), p.user_id, p.net_id, p.tile_y * 10000 + p.tile_x, selected[p.net_id] and 1 or 0, last_result[p.net_id] or "" },
            tile = { x = p.tile_x, y = p.tile_y },
            selected = selected[p.net_id] == true,
            style = p.is_local and "muted" or "",
        }
    end

    local note = { string.format("Мир %s. Клик по строке — выбрать/снять игрока.", w) }
    if zone then
        note[#note + 1] = string.format("Зона: %d,%d — %d,%d (игроков в ней: %d).", zone.x1, zone.y1, zone.x2, zone.y2, #players_in(zone))
    end
    local rules_state = gtproxy.setting("rules_enabled") and (gtproxy.setting("dry_run") and "включены, только проба (dry run)" or "включены") or "выключены"
    note[#note + 1] = string.format("Правила входа: %s, правил %d.", rules_state, #active_rules)
    if rules_problem then
        note[#note + 1] = rules_problem
    end
    local lock = lock_info()
    if lock then
        note[#note + 1] = string.format("Замок %d, %d: %s (%d), владелец %s. Доступ: %s. Сейчас здесь с доступом: %s.",
            lock.x, lock.y, lock.kind, lock.item, lock.owner, #lock.access > 0 and table.concat(lock.access, ", ") or "ни у кого",
            #lock.present > 0 and table.concat(lock.present, ", ") or "никого")
    end
    if plan then
        local names = {}
        for _, t in ipairs(plan.targets) do
            names[#names + 1] = player_text(t)
        end
        note[#note + 1] = string.format("Ожидает подтверждения: %s → %s", NAMES[plan.action], table.concat(names, ", "))
    end
    for _, line in ipairs(batch_lines()) do
        note[#note + 1] = line
    end

    local buttons = {
        { id = "all", label = "Выбрать всех" }, { id = "none", label = "Снять выбор" }, { id = "zone", label = "Зона на карте" },
    }
    if zone then
        buttons[#buttons + 1] = { id = "zone_select", label = "Выбрать зону" }
        buttons[#buttons + 1] = { id = "zone_clear", label = "Убрать зону" }
    end
    if plan then
        buttons[#buttons + 1] = { id = "confirm", label = "Подтвердить" }
        buttons[#buttons + 1] = { id = "cancel", label = "Отмена" }
    else
        buttons[#buttons + 1] = { id = "pull", label = "Pull" }
        buttons[#buttons + 1] = { id = "kick", label = "Kick" }
        buttons[#buttons + 1] = { id = "ban", label = "Ban" }
        buttons[#buttons + 1] = { id = "lock", label = "Замок на карте" }
        if lock then
            buttons[#buttons + 1] = { id = "give", label = "Дать доступ выбранным" }
            buttons[#buttons + 1] = { id = "take", label = "Забрать доступ у выбранных" }
        end
    end
    if #queue > 0 then
        buttons[#buttons + 1] = { id = "stop", label = "Стоп очереди" }
    end
    gtproxy.panel.set(PANEL, {
        title = "Модерация",
        note = table.concat(note, "\n"),
        buttons = buttons,
        columns = { "Игрок", "ID аккаунта", "netID", "Коорд.", "Выбран", "Результат" },
        rows = rows,
        empty = "В мире никого.",
    })
    show_marks()
    refresh_log()
end

local function prepare(action, targets, extra)
    if #targets == 0 then
        return { ok = false, message = "Никто не выбран" }
    end
    plan = { action = action, targets = targets, world = world(), lock = extra and extra.lock or nil, zone = extra and extra.zone or nil }
    refresh()
    return { ok = true, message = string.format("%s для %d — подтвердите", NAMES[action], #targets) }
end

gtproxy.on("pick", function(e)
    if not e.in_world then
        picking = nil
        return { ok = false, message = "Вы не в мире" }
    end
    if picking == "zone1" then
        corner = { x = e.x, y = e.y }
        picking = "zone2"
        gtproxy.map.pick("второй угол зоны (модерация)")
        return { ok = true, message = string.format("Первый угол %d, %d — щёлкните второй", e.x, e.y) }
    elseif picking == "zone2" and corner then
        zone = { x1 = math.min(corner.x, e.x), y1 = math.min(corner.y, e.y), x2 = math.max(corner.x, e.x), y2 = math.max(corner.y, e.y) }
        picking, corner = nil, nil
        refresh()
        return { ok = true, message = string.format("Зона %d,%d — %d,%d: игроков %d", zone.x1, zone.y1, zone.x2, zone.y2, #players_in(zone)) }
    elseif picking == "lock" then
        picking = nil
        local tile = gtproxy.world.tile(e.x, e.y)
        if not tile or tile.extra ~= "lock" then
            refresh()
            return { ok = false, message = string.format("%d, %d: здесь нет замка", e.x, e.y) }
        end
        lock_sel = { x = e.x, y = e.y }
        refresh()
        return { ok = true, message = string.format("Замок %d, %d выбран", e.x, e.y) }
    end
    return { ok = false, message = "Ничего не выбиралось" }
end)

gtproxy.on("panel", function(e)
    if e.panel ~= PANEL then
        return
    end
    local a = e.action
    if a == "select" then
        local net = tonumber(e.row)
        local p = net and player_by_net(net)
        if not p then
            refresh()
            return { ok = false, message = "Игрок ушёл" }
        end
        if p.is_local then
            return { ok = false, message = "Это вы" }
        end
        selected[net] = not selected[net] or nil
        refresh()
        return { ok = true, message = selected[net] and ("Выбран " .. clean(p.name)) or ("Снят " .. clean(p.name)) }
    elseif a == "all" then
        for _, p in ipairs(gtproxy.world.players()) do
            if not p.is_local then
                selected[p.net_id] = true
            end
        end
        refresh()
        return { ok = true, message = "Выбраны все, кроме вас" }
    elseif a == "none" then
        selected = {}
        refresh()
        return { ok = true, message = "Выбор снят" }
    elseif a == "zone" then
        picking = "zone1"
        gtproxy.map.pick("первый угол зоны (модерация)")
        return { ok = true, message = "Щёлкните первый угол зоны на карте" }
    elseif a == "zone_select" then
        if not zone then
            return { ok = false, message = "Зоны нет" }
        end
        selected = {}
        for _, t in ipairs(players_in(zone)) do
            selected[t.net_id] = true
        end
        refresh()
        return { ok = true, message = string.format("В зоне: %d", #players_in(zone)) }
    elseif a == "zone_clear" then
        zone = nil
        refresh()
        return { ok = true, message = "Зона убрана" }
    elseif a == "lock" then
        picking = "lock"
        gtproxy.map.pick("замок (модерация)")
        return { ok = true, message = "Щёлкните замок на карте" }
    elseif a == "pull" or a == "kick" or a == "ban" then
        if plan then
            return { ok = false, message = "Уже ждёт подтверждения другое действие" }
        end
        return prepare(a, selected_targets())
    elseif a == "give" or a == "take" then
        if plan then
            return { ok = false, message = "Уже ждёт подтверждения другое действие" }
        end
        local lock = lock_info()
        if not lock then
            return { ok = false, message = "Замок не выбран или его больше нет" }
        end
        return prepare(a, selected_targets(), { lock = { x = lock.x, y = lock.y, item = lock.item } })
    elseif a == "confirm" then
        if not plan then
            return { ok = false, message = "Нечего подтверждать" }
        end
        local p = plan
        plan = nil
        if p.world ~= world() then
            refresh()
            return { ok = false, message = "Отменено: вы в другом мире" }
        end
        local id, why = new_batch(p.action, p.targets, { lock = p.lock, zone = p.zone })
        refresh()
        return { ok = id ~= nil, message = id and string.format("%s: %d в очереди", NAMES[p.action], #p.targets) or why }
    elseif a == "cancel" then
        plan = nil
        refresh()
        return { ok = true, message = "Отменено" }
    elseif a == "stop" then
        stop_all("остановлено вами")
        refresh()
        return { ok = true, message = "Очередь остановлена (текущий шаг доведётся до результата)" }
    end
end)

---------------------------------------------------------------------------
-- Leaving the world
---------------------------------------------------------------------------

local function reset(why)
    if op then
        local o = op
        if o.stage == "sent" then
            end_step("Результат неизвестен", why)
        else
            end_step("Отменено", why)
        end
    end
    stop_all(why)
    queue = {}
    gtproxy.cancel(gap_timer)
    gap_timer = nil
    selected, zone, corner, picking, plan, lock_sel, last_result, checked_entry = {}, nil, nil, nil, nil, nil, {}, {}
    gtproxy.map.pick(nil)
    gtproxy.map.overlay(PANEL, nil)
end

gtproxy.on("world_menu", function()
    reset("вы покинули мир")
    refresh()
end)
gtproxy.on("world_enter", function()
    reset("вы перешли в другой мир")
    -- Players already here when you came in: the entry rules look at them too.
    gtproxy.after(1500, function()
        for _, p in ipairs(gtproxy.world.players()) do
            check_entry(p)
        end
        refresh()
    end)
    refresh()
end)
gtproxy.on("disconnect", function()
    reset("связь прервалась")
    gtproxy.panel.remove(PANEL)
end)
gtproxy.on("settings", function()
    load_rules()
    refresh()
end)

-- Positions change all the time: the table follows (cheap, a few players).
gtproxy.every(1000, function()
    if world() then
        refresh()
    end
end)

---------------------------------------------------------------------------
-- For other modules (autohost)
---------------------------------------------------------------------------

gtproxy.export("players_in", function(z)
    if type(z) ~= "table" or not world() then
        return {}
    end
    return players_in(z)
end)

-- run(action, targets, { source, zone }) -> { id = batch id } or { why = ... }. The
-- caller has shown and confirmed the action; each target is checked again
-- here (still present, same account, inside the zone) before its step.
gtproxy.export("run", function(action, targets, options)
    if action ~= "pull" and action ~= "kick" and action ~= "ban" then
        return { why = "unknown action" }
    end
    local list = {}
    for _, t in ipairs(type(targets) == "table" and targets or {}) do
        if type(t) == "table" and tonumber(t.net_id) then
            list[#list + 1] = { net_id = tonumber(t.net_id), user_id = tonumber(t.user_id) or 0, name = tostring(t.name or "?") }
        end
    end
    local id, why = new_batch(action, list, { source = options and options.source or "другой модуль", zone = options and options.zone or nil })
    refresh()
    return { id = id, why = why }
end)

gtproxy.export("batch", function(id)
    local b = batches[tonumber(id) or -1]
    if not b then
        return nil
    end
    local items = {}
    for _, it in ipairs(b.items) do
        items[#items + 1] = { name = it.target.name, user_id = it.target.user_id, status = it.status, detail = it.detail }
    end
    return { action = b.action, done = batch_done(b), items = items }
end)

gtproxy.export("stop", function()
    stop_all("остановлено другим модулем")
    refresh()
    return true
end)

---------------------------------------------------------------------------
-- /wm
---------------------------------------------------------------------------

local function cmd_wm(args, ctx)
    local word = (args[1] or ""):lower()
    if not world() then
        ctx.reply("`4Oops: ``you are not in a world.")
        return
    end
    if word == "" or word == "list" then
        local players = gtproxy.world.players()
        ctx.reply(string.format("`o%d player(s) here. Choose and act in the map window (%smap), world_moderation tab.", #players, P))
        for i, p in ipairs(players) do
            if i > 8 then
                break
            end
            ctx.reply(string.format("`o%s: user ID %d, netID %d at %d, %d%s", gtproxy.text.sanitize(clean(p.name)), p.user_id, p.net_id, p.tile_x, p.tile_y, p.is_local and " (you)" or ""))
        end
        return
    end
    if (word == "pull" or word == "kick" or word == "ban") and args[2] then
        local wanted = table.concat(args, " ", 2):lower()
        for _, p in ipairs(gtproxy.world.players()) do
            if not p.is_local and clean(p.name):lower() == wanted then
                local id, why = new_batch(word, { { net_id = p.net_id, user_id = p.user_id, name = clean(p.name) } }, { source = "команда" })
                ctx.reply(id and string.format("`o%s `w%s``: asking the server (the result is in the map window's moderation journal).", word, gtproxy.text.sanitize(clean(p.name)))
                    or "`4Not done: ``not in a world.")
                return
            end
        end
        ctx.reply("`4Oops: ``no player with exactly that name here.")
        return
    end
    if word == "stop" then
        stop_all("остановлено командой")
        ctx.reply("`oQueue stopped.")
        return
    end
    ctx.reply("`oUsage: wm [list] | wm pull|kick|ban <exact name> | wm stop")
end

local command_name = nil
for _, name in ipairs({ "wm", "world_mod" }) do
    if gtproxy.command(name, "World moderation: wm [list] | wm pull|kick|ban <exact name> | wm stop", cmd_wm) then
        command_name = name
        break
    end
end
if command_name ~= "wm" then
    gtproxy.log.warn("/wm is taken by another command; world_moderation uses " .. (command_name and ("/" .. command_name) or "no command"))
end

load_rules()
refresh()
