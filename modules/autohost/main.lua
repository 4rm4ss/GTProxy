-- Autohost (module API 3, see docs/modules.md).
--
-- Points 1-4 of the world you host in: /pos1 ... /pos4 take the tile you
-- stand on, or pick one on the map (tab "Автохост"); each point has a name
-- and a zone (a rectangle; by default zone_radius tiles around it, or two
-- corners picked on the map). Points 1 and 2 are needed, 3 and 4 not. They
-- are kept per server and world (data/autohost.json).
--
-- /host start | stop | reset: start shows the world, the points and the
-- commission - it gives nothing and moves nobody. reset clears the points
-- of this world after you confirm it.
--
-- /win1 ... /win4 [amount]: the payout window (tab "Выплата"): the point,
-- who gets it (a player inside the point's zone), the item (currency),
-- the amount, the commission %, what is held back and what is paid, with the
-- rounding rule of config/modules/autohost.json (100 WL at 10% -> 90 WL).
-- "Подтвердить" pays with one ordinary drop (the game's Drop and its "how
-- many?" answer) where you stand - only if you stand near the point (else
-- it offers the way there) and the receiver is still in the zone. States:
-- «Ожидает подтверждения», «Отправлено», «Подтверждено», «Отклонено»,
-- «Результат неизвестен»; it is never sent twice by itself.
--
-- Pull / kick / ban of the players in a point's zone: listed first, then
-- confirmed; the world_moderation module checks again (still there, same
-- account, still in the zone, your rights) and does it.
--
-- The drop window of a payout is answered here before modules/drop sees it
-- (modules run in the order of their folder names: autohost, drop, ...).

local payout = gtproxy.load("payout.lua")
local P = gtproxy.prefix
local PANEL, PANEL_PAY = "autohost", "payout"
local POINTS = 4

local running = {}          -- "server/WORLD" -> true
local point_sel = 1
local picking = nil         -- { kind = "point"|"zone1"|"zone2", n, corner }
local reset_until = 0       -- /host reset waiting for the second one
local pay = nil             -- the payout window
local mod_plan = nil        -- { action, n, targets } waiting for confirmation
local mod_batch = nil       -- { id, action, n }
local history = {}
local commands = {}

local function clean(name)
    return gtproxy.text.strip_colors(name or "")
end

local function world_key()
    local info = gtproxy.world.info()
    if not info.in_world then
        return nil
    end
    return (info.server ~= "" and info.server or "server") .. "/" .. info.name, info.name
end

local function ms(name, low, high)
    return math.max(low, math.min(high, math.floor(tonumber(gtproxy.setting(name)) or low)))
end

local function currency()
    return math.floor(tonumber(gtproxy.setting("currency")) or 242)
end

local function item_name(id)
    local info = gtproxy.items.get(id)
    return info and info.name or string.format("предмет %d", id)
end

local function unit()
    return currency() == 242 and "WL" or item_name(currency())
end

---------------------------------------------------------------------------
-- Points
---------------------------------------------------------------------------

local function point(n)
    local key = world_key()
    if not key then
        return nil
    end
    return payout.decode_point(gtproxy.store.get(string.format("p%d/%s", n, key), nil))
end

local function save_point(n, p)
    local key = world_key()
    if key then
        gtproxy.store.set(string.format("p%d/%s", n, key), p and payout.encode_point(p) or nil)
    end
end

local function me()
    local info = gtproxy.world.info()
    return info.in_world and info.me or nil
end

local function players_in(z)
    local list = {}
    for _, p in ipairs(gtproxy.world.players()) do
        if not p.is_local and payout.inside(p.tile_x, p.tile_y, z) then
            list[#list + 1] = { net_id = p.net_id, user_id = p.user_id, name = clean(p.name), tile_x = p.tile_x, tile_y = p.tile_y }
        end
    end
    return list
end

local function set_point_at(n, x, y)
    local old = point(n)
    local r = math.max(0, math.floor(tonumber(gtproxy.setting("zone_radius")) or 1))
    local z = payout.zone_around(x, y, r)
    local p = { x = x, y = y, x1 = z.x1, y1 = z.y1, x2 = z.x2, y2 = z.y2, name = old and old.name ~= "" and old.name or ("Точка " .. n) }
    save_point(n, p)
    return p
end

local function missing_required()
    local missing = {}
    for n = 1, 2 do
        if not point(n) then
            missing[#missing + 1] = tostring(n)
        end
    end
    return missing
end

---------------------------------------------------------------------------
-- The tabs
---------------------------------------------------------------------------

local refresh

local function rule_text()
    return payout.ROUNDING[gtproxy.setting("rounding")] or ("неизвестное правило «" .. tostring(gtproxy.setting("rounding")) .. "»")
end

local function show_marks()
    local markers, areas, route = {}, {}, nil
    for n = 1, POINTS do
        local p = point(n)
        if p then
            markers[#markers + 1] = { x = p.x, y = p.y, label = string.format("%d: %s", n, p.name), color = n <= 2 and "#ffcf4a" or "#7ee787" }
            areas[#areas + 1] = { x1 = p.x1, y1 = p.y1, x2 = p.x2, y2 = p.y2, label = "", color = n == point_sel and "#ffcf4a" or "#4fc3ff" }
        end
    end
    if pay and pay.route then
        route = pay.route
    end
    if #markers == 0 then
        gtproxy.map.overlay(PANEL, nil)
    else
        gtproxy.map.overlay(PANEL, { markers = markers, areas = areas, route = route, route_color = "#c77dff" })
    end
end

local function pay_lines()
    if not pay then
        return { "Окно выплаты закрыто: /win1 … /win4 или «Выплата» у точки." }
    end
    local lines = {}
    lines[#lines + 1] = string.format("Точка %d «%s» (%d, %d). Состояние: %s", pay.n, pay.point.name, pay.point.x, pay.point.y, pay.status)
    lines[#lines + 1] = "Получатель: " .. (pay.recipient and string.format("%s (#%d)", pay.recipient.name, pay.recipient.user_id) or "не выбран (щёлкните игрока в зоне)")
    lines[#lines + 1] = string.format("Предмет: %s (%d). Комиссия: %s%%. Правило: %s.", item_name(pay.item), pay.item, payout.number(pay.percent), rule_text())
    if pay.calc then
        lines[#lines + 1] = payout.text(pay.calc, pay.unit)
    else
        lines[#lines + 1] = "Сумма: " .. (pay.problem or "введите сумму")
    end
    if pay.detail then
        lines[#lines + 1] = pay.detail
    end
    return lines
end

local function refresh_pay()
    if not pay then
        gtproxy.panel.remove(PANEL_PAY)
        return
    end
    local rows = {}
    for _, t in ipairs(players_in(pay.point)) do
        rows[#rows + 1] = {
            id = tostring(t.net_id),
            cells = { t.name, t.user_id > 0 and t.user_id or "?", string.format("%d, %d", t.tile_x, t.tile_y) },
            tile = { x = t.tile_x, y = t.tile_y },
            selected = pay.recipient ~= nil and pay.recipient.net_id == t.net_id,
        }
    end
    local buttons = {}
    if pay.stage == "prepare" then
        buttons[#buttons + 1] = { id = "calc", label = "Пересчитать" }
        buttons[#buttons + 1] = { id = "confirm", label = "Подтвердить выплату" }
        buttons[#buttons + 1] = { id = "go", label = "Идти к точке" }
    end
    buttons[#buttons + 1] = { id = "close", label = pay.stage == "prepare" and "Отмена" or "Закрыть" }
    gtproxy.panel.set(PANEL_PAY, {
        title = "Выплата",
        note = table.concat(pay_lines(), "\n"),
        inputs = pay.stage == "prepare" and { { id = "amount", label = "Сумма (" .. pay.unit .. ")", value = pay.calc and tostring(pay.calc.amount) or (pay.amount_text or ""), size = 8 } } or nil,
        buttons = buttons,
        columns = { "Игрок в зоне", "ID аккаунта", "Коорд." },
        rows = rows,
        empty = "В зоне точки никого нет.",
    })
end

refresh = function()
    local key, name = world_key()
    if not key then
        gtproxy.panel.set(PANEL, { title = "Автохост", note = "Вы не в мире.", columns = {}, rows = {} })
        gtproxy.map.overlay(PANEL, nil)
        refresh_pay()
        return
    end
    local rows = {}
    for n = 1, POINTS do
        local p = point(n)
        local inside = p and players_in(p) or {}
        local names = {}
        for _, t in ipairs(inside) do
            names[#names + 1] = t.name
        end
        rows[#rows + 1] = {
            id = tostring(n),
            cells = {
                n, p and p.name or (n <= 2 and "не задана (нужна)" or "не задана"),
                p and string.format("%d, %d", p.x, p.y) or "—",
                p and string.format("%d,%d — %d,%d", p.x1, p.y1, p.x2, p.y2) or "—",
                p and (#names > 0 and table.concat(names, ", ") or "никого") or "—",
            },
            tile = p and { x = p.x, y = p.y } or nil,
            selected = n == point_sel,
            style = p and "ok" or (n <= 2 and "bad" or "muted"),
            actions = {
                { id = "here", label = "Моя позиция" }, { id = "pick", label = "На карте" }, { id = "zone", label = "Зона" },
                { id = "win", label = "Выплата" }, { id = "clear", label = "Стереть" },
            },
        }
    end
    local missing = missing_required()
    local note = {
        string.format("Мир %s · хост %s · комиссия %s%%, %s · валюта %s.", name, running[key] and "запущен" or "остановлен",
            payout.number(tonumber(gtproxy.setting("commission_percent")) or 0), rule_text(), item_name(currency())),
        #missing > 0 and ("Нужны точки: " .. table.concat(missing, ", ") .. ".") or "Точки 1 и 2 заданы.",
        string.format("Выбрана точка %d (строка). Имя точки — поле ниже и «Имя».", point_sel),
    }
    if mod_plan then
        local names = {}
        for _, t in ipairs(mod_plan.targets) do
            names[#names + 1] = string.format("%s (#%d)", t.name, t.user_id)
        end
        note[#note + 1] = string.format("Ожидает подтверждения: %s в зоне точки %d → %s", mod_plan.action, mod_plan.n, table.concat(names, ", "))
    end
    if mod_batch then
        local ok, b = pcall(gtproxy.call, "world_moderation", "batch", mod_batch.id)
        if ok and b then
            local parts = {}
            for _, it in ipairs(b.items) do
                parts[#parts + 1] = string.format("%s: %s%s", it.name, it.status, it.detail ~= "" and (" (" .. it.detail .. ")") or "")
            end
            note[#note + 1] = string.format("%s в зоне точки %d: %s", mod_batch.action, mod_batch.n, table.concat(parts, "; "))
        end
    end
    local buttons = {
        { id = running[key] and "stop" or "start", label = running[key] and "Остановить хост" or "Запустить хост" },
        { id = "reset", label = gtproxy.now() < reset_until and "Точно сбросить точки?" or "Сбросить точки" },
        { id = "name", label = "Имя" },
    }
    if mod_plan then
        buttons[#buttons + 1] = { id = "mod_confirm", label = "Подтвердить " .. mod_plan.action }
        buttons[#buttons + 1] = { id = "mod_cancel", label = "Отмена" }
    else
        buttons[#buttons + 1] = { id = "mod_pull", label = "Pull в зоне" }
        buttons[#buttons + 1] = { id = "mod_kick", label = "Kick в зоне" }
        buttons[#buttons + 1] = { id = "mod_ban", label = "Ban в зоне" }
    end
    gtproxy.panel.set(PANEL, {
        title = "Автохост",
        note = table.concat(note, "\n"),
        inputs = { { id = "pname", label = "Имя точки", value = (point(point_sel) or {}).name or "", size = 16 } },
        buttons = buttons,
        columns = { "№", "Имя", "Клетка", "Зона", "В зоне" },
        rows = rows,
    })
    show_marks()
    refresh_pay()
end

---------------------------------------------------------------------------
-- The payout
---------------------------------------------------------------------------

local function recalc(amount_text)
    if not pay then
        return
    end
    pay.amount_text = amount_text or pay.amount_text
    local amount = tonumber(pay.amount_text or "")
    pay.calc, pay.problem = nil, nil
    if amount then
        pay.calc, pay.problem = payout.compute(amount, pay.percent, gtproxy.setting("rounding"))
    end
end

-- -> true, or false, the reason (Russian, for the map window) and the same
-- in English (the game's font has no Cyrillic).
local function open_pay(n, amount_text)
    local p = point(n)
    if not p then
        return false, string.format("точка %d не задана", n), string.format("point %d is not set (%spos%d)", n, P, n)
    end
    local key = world_key()
    if not running[key] then
        return false, "хост не запущен (/host start)", "hosting is not started (" .. P .. "host start)"
    end
    if pay and (pay.stage == "dialog" or pay.stage == "sent") then
        return false, "предыдущая выплата ещё ждёт ответа сервера", "the last payout still waits for the server"
    end
    local percent = tonumber(gtproxy.setting("commission_percent")) or 0
    pay = { n = n, point = p, stage = "prepare", status = "Ожидает подтверждения", item = currency(), unit = unit(), percent = percent, world = key }
    local inside = players_in(p)
    if #inside == 1 then
        pay.recipient = inside[1]
    end
    recalc(amount_text)
    point_sel = n
    refresh()
    return true
end

local function finish_pay(status, detail)
    if not pay then
        return
    end
    gtproxy.cancel(pay.timer)
    pay.timer = nil
    pay.stage = "done"
    pay.status = status
    pay.detail = detail
    pay.route = nil
    history[#history + 1] = { time = gtproxy.date(), text = string.format("точка %d, %s, %s: %s", pay.n,
        pay.recipient and pay.recipient.name or "?", pay.calc and (pay.calc.result .. " " .. pay.unit) or "?", status) }
    gtproxy.log.info(string.format("autohost payout point %d to %s: %s %s - %s%s", pay.n, pay.recipient and pay.recipient.name or "?",
        pay.calc and pay.calc.result or "?", pay.unit, status, detail and (" (" .. detail .. ")") or ""))
    refresh()
end

-- Everything a payout needs; the reason when something is missing.
local function payout_problem()
    local key = world_key()
    if key ~= pay.world then
        return "вы в другом мире"
    end
    local p = point(pay.n)
    if not p then
        return "точки больше нет"
    end
    pay.point = p
    if not pay.calc then
        return pay.problem or "введите сумму"
    end
    if pay.calc.result < 1 then
        return "к выплате 0 — выбрасывать нечего"
    end
    if not pay.recipient then
        return "получатель не выбран"
    end
    local still = false
    for _, t in ipairs(players_in(p)) do
        if t.net_id == pay.recipient.net_id and t.user_id == pay.recipient.user_id then
            still = true
        end
    end
    if not still then
        return string.format("%s больше не в зоне точки", pay.recipient.name)
    end
    local here = me()
    if not here then
        return "ваше место неизвестно"
    end
    if not payout.near({ x = here.tile_x, y = here.tile_y }, p, ms("near_tiles", 0, 50)) then
        local way = gtproxy.map.route(p.x, p.y)
        pay.route = way.ok and way.tiles or nil
        return string.format("вы далеко от точки (вы %d, %d; точка %d, %d)%s — «Идти к точке»", here.tile_x, here.tile_y, p.x, p.y,
            way.ok and string.format(", путь %d шаг. на карте", way.steps) or ", пути нет")
    end
    local max = ms("max_drop", 1, 100000)
    if pay.calc.result > max then
        return string.format("одним выбросом можно отдать не больше %d (max_drop), а нужно %d", max, pay.calc.result)
    end
    local inv = gtproxy.inventory.info()
    if inv.known then
        local have = gtproxy.inventory.count(pay.item)
        if have < pay.calc.result then
            return string.format("у вас %d %s, нужно %d", have, pay.unit, pay.calc.result)
        end
    end
    return nil
end

local function confirm_pay()
    if not pay or pay.stage ~= "prepare" then
        return false, pay and "выплата уже отправлена — ждите результата" or "окно выплаты закрыто"
    end
    local problem = payout_problem()
    if problem then
        pay.detail = "Недоступно: " .. problem
        refresh()
        return false, problem
    end
    local p = pay
    if not gtproxy.server.text(string.format("action|drop\n|itemID|%d\n", p.item)) then
        finish_pay("Отклонено", "нет связи с сервером; ничего не отправлено")
        return false, "нет связи с сервером"
    end
    p.stage, p.status, p.detail = "dialog", "Отправлено", "ждём окно «сколько выбросить?» от сервера"
    p.before = gtproxy.inventory.count(p.item)
    p.timer = gtproxy.after(ms("dialog_timeout_ms", 500, 30000), function()
        if pay == p and p.stage == "dialog" then
            p.timer = nil
            finish_pay("Отклонено", "сервер не открыл окно выброса; ничего не выброшено")
        end
    end)
    refresh()
    return true
end

-- The server's "how many?" window of our drop: answered with the payout.
gtproxy.hook_call("OnDialogRequest", function(call)
    local p = pay
    if not p or p.stage ~= "dialog" or type(call.args[1]) ~= "string" then
        return
    end
    local d = gtproxy.ui.parse_dialog(call.args[1])
    local item = tonumber(d.embed.itemID or d.embed.itemid or "")
    if not d.name:lower():find("drop", 1, true) or item ~= p.item then
        return
    end
    local owned = tonumber(d.inputs.count or "")
    if owned and owned < p.calc.result then
        finish_pay("Отклонено", string.format("у вас %d, нужно %d; ничего не выброшено", owned, p.calc.result))
        return "cancel"
    end
    gtproxy.server.text(gtproxy.ui.answer(d, { count = p.calc.result }))
    p.stage, p.detail = "sent", string.format("выброс %d %s отправлен", p.calc.result, p.unit)
    gtproxy.cancel(p.timer)
    p.timer = gtproxy.after(ms("result_timeout_ms", 500, 60000), function()
        if pay == p and p.stage == "sent" then
            p.timer = nil
            finish_pay("Результат неизвестен", "сервер не подтвердил выброс вовремя; повторно не отправлялось — проверьте инвентарь")
        end
    end)
    refresh()
    return "cancel"
end)

gtproxy.on("object", function(e)
    local p = pay
    if p and p.stage == "sent" and e.kind == "dropped" and e.item == p.item then
        if e.amount == p.calc.result then
            finish_pay("Подтверждено", string.format("на земле %d %s в %d, %d", e.amount, p.unit, e.x, e.y))
        else
            finish_pay("Подтверждено", string.format("сервер выбросил %d вместо %d", e.amount, p.calc.result))
        end
    end
end)

gtproxy.on("inventory", function(e)
    local p = pay
    if not p or p.stage ~= "sent" or (not e.full and e.item ~= p.item) then
        return
    end
    local gone = p.before - gtproxy.inventory.count(p.item)
    if gone >= p.calc.result then
        finish_pay("Подтверждено", string.format("из инвентаря ушло %d %s", gone, p.unit))
    end
end)

gtproxy.on("console", function(e)
    local p = pay
    if not p or (p.stage ~= "sent" and p.stage ~= "dialog") then
        return
    end
    local text = (e.clean or ""):lower()
    for _, word in ipairs({ "can't", "cannot", "not allowed", "too many", "no room", "unable", "failed" }) do
        if text:find(word, 1, true) then
            finish_pay("Отклонено", "сервер: " .. e.clean)
            return
        end
    end
end)

---------------------------------------------------------------------------
-- Pull / kick / ban in a zone (world_moderation does it)
---------------------------------------------------------------------------

local function prepare_mod(action, n)
    local p = point(n)
    if not p then
        return false, string.format("точка %d не задана", n)
    end
    local ok, list = pcall(gtproxy.call, "world_moderation", "players_in", { x1 = p.x1, y1 = p.y1, x2 = p.x2, y2 = p.y2 })
    if not ok then
        return false, "модуль world_moderation не загружен — действие недоступно"
    end
    if #list == 0 then
        return false, string.format("в зоне точки %d никого", n)
    end
    mod_plan = { action = action, n = n, targets = list, zone = { x1 = p.x1, y1 = p.y1, x2 = p.x2, y2 = p.y2 } }
    refresh()
    return true, string.format("%s: %d в зоне — подтвердите", action, #list)
end

local function confirm_mod()
    local plan = mod_plan
    if not plan then
        return false, "нечего подтверждать"
    end
    mod_plan = nil
    local ok, started = pcall(gtproxy.call, "world_moderation", "run", plan.action, plan.targets, { source = "autohost", zone = plan.zone })
    if not ok then
        refresh()
        return false, "модуль world_moderation не загружен"
    end
    if not started or not started.id then
        refresh()
        return false, started and started.why or "не получилось"
    end
    mod_batch = { id = started.id, action = plan.action, n = plan.n }
    refresh()
    return true, string.format("%s: %d в очереди world_moderation", plan.action, #plan.targets)
end

---------------------------------------------------------------------------
-- Commands and the tabs' buttons
---------------------------------------------------------------------------

local function cmd_pos(n)
    return function(_, ctx)
        local here = me()
        if not here then
            ctx.reply("`4Oops: ``you are not in a world (or your position is not known yet).")
            return
        end
        local p = set_point_at(n, here.tile_x, here.tile_y)
        point_sel = n
        refresh()
        ctx.reply(string.format("`oPoint %d set to `w%d, %d`` (zone %d,%d - %d,%d).", n, p.x, p.y, p.x1, p.y1, p.x2, p.y2))
    end
end

local function host(word)
    local key, name = world_key()
    if not key then
        return false, "you are not in a world"
    end
    if word == "start" then
        local missing = missing_required()
        if #missing > 0 then
            return false, "points " .. table.concat(missing, ", ") .. " are needed first (/pos1, /pos2)"
        end
        running[key] = true
        local parts = {}
        for n = 1, POINTS do
            local p = point(n)
            if p then
                parts[#parts + 1] = string.format("%d at %d,%d", n, p.x, p.y)
            end
        end
        refresh()
        return true, string.format("Hosting in %s: points %s; commission %s%%, %s. Nothing is given or moved by starting.",
            name, table.concat(parts, ", "), payout.number(tonumber(gtproxy.setting("commission_percent")) or 0), gtproxy.setting("rounding"))
    elseif word == "stop" then
        running[key] = nil
        if pay and pay.stage == "prepare" then
            pay = nil
        end
        refresh()
        return true, "Hosting stopped."
    elseif word == "reset" then
        if gtproxy.now() > reset_until then
            reset_until = gtproxy.now() + 15000
            return true, "This clears the points of " .. name .. ". Again within 15 s to confirm."
        end
        reset_until = 0
        for n = 1, POINTS do
            save_point(n, nil)
        end
        running[key] = nil
        pay, mod_plan = nil, nil
        refresh()
        return true, "Points of " .. name .. " cleared."
    end
    return false, "usage: host start|stop|reset"
end

local function cmd_host(args, ctx)
    local ok, text = host((args[1] or ""):lower())
    ctx.reply((ok and "`o" or "`4Oops: ``") .. gtproxy.text.sanitize(text))
end

local function cmd_win(n)
    return function(args, ctx)
        local ok, _, why = open_pay(n, args[1])
        if not ok then
            ctx.reply("`4Oops: ``" .. gtproxy.text.sanitize(why))
            return
        end
        if pay.calc then
            ctx.reply(string.format("`oPayout point %d: %d %s - %s%% = `w%d %s``. Confirm in the map window (%smap), payout tab.",
                n, pay.calc.amount, pay.unit, payout.number(pay.percent), pay.calc.result, pay.unit, P))
        else
            ctx.reply(string.format("`oPayout window of point %d open: type the amount in the map window (%smap), payout tab.", n, P))
        end
    end
end

gtproxy.on("pick", function(e)
    local pk = picking
    picking = nil
    if not pk or not e.in_world then
        return { ok = false, message = "Ничего не выбиралось" }
    end
    if pk.kind == "point" then
        local p = set_point_at(pk.n, e.x, e.y)
        refresh()
        return { ok = true, message = string.format("Точка %d: %d, %d", pk.n, p.x, p.y) }
    elseif pk.kind == "zone1" then
        picking = { kind = "zone2", n = pk.n, corner = { x = e.x, y = e.y } }
        gtproxy.map.pick(string.format("второй угол зоны точки %d", pk.n))
        return { ok = true, message = "Теперь второй угол" }
    elseif pk.kind == "zone2" then
        local p = point(pk.n)
        if not p then
            return { ok = false, message = "Сначала задайте точку" }
        end
        p.x1, p.y1 = math.min(pk.corner.x, e.x), math.min(pk.corner.y, e.y)
        p.x2, p.y2 = math.max(pk.corner.x, e.x), math.max(pk.corner.y, e.y)
        save_point(pk.n, p)
        refresh()
        return { ok = true, message = string.format("Зона точки %d: %d,%d — %d,%d", pk.n, p.x1, p.y1, p.x2, p.y2) }
    end
end)

gtproxy.on("panel", function(e)
    if e.panel == PANEL then
        local a = e.action
        local n = tonumber(e.row) or point_sel
        if a == "select" then
            point_sel = n
            refresh()
            return { ok = true, message = "Точка " .. n }
        elseif a == "here" then
            local here = me()
            if not here then
                return { ok = false, message = "Ваше место неизвестно" }
            end
            local p = set_point_at(n, here.tile_x, here.tile_y)
            point_sel = n
            refresh()
            return { ok = true, message = string.format("Точка %d: %d, %d", n, p.x, p.y) }
        elseif a == "pick" then
            picking = { kind = "point", n = n }
            gtproxy.map.pick(string.format("точка %d (автохост)", n))
            return { ok = true, message = "Щёлкните клетку на карте" }
        elseif a == "zone" then
            if not point(n) then
                return { ok = false, message = "Сначала задайте точку" }
            end
            picking = { kind = "zone1", n = n }
            gtproxy.map.pick(string.format("первый угол зоны точки %d", n))
            return { ok = true, message = "Щёлкните первый угол зоны" }
        elseif a == "clear" then
            save_point(n, nil)
            refresh()
            return { ok = true, message = "Точка " .. n .. " стёрта" }
        elseif a == "win" then
            local ok, why = open_pay(n)
            return { ok = ok, message = ok and "Окно выплаты: вкладка «Выплата»" or why }
        elseif a == "name" then
            local p = point(point_sel)
            local text = (e.values and e.values.pname or ""):match("^%s*(.-)%s*$")
            if not p then
                return { ok = false, message = "Точка не задана" }
            end
            if text == "" or #text > 24 then
                return { ok = false, message = "Имя — от 1 до 24 знаков" }
            end
            p.name = text
            save_point(point_sel, p)
            refresh()
            return { ok = true, message = "Имя сохранено" }
        elseif a == "start" or a == "stop" or a == "reset" then
            local ok, text = host(a)
            return { ok = ok, message = text }
        elseif a == "mod_pull" or a == "mod_kick" or a == "mod_ban" then
            local ok, text = prepare_mod(a:sub(5), point_sel)
            return { ok = ok, message = text }
        elseif a == "mod_confirm" then
            local ok, text = confirm_mod()
            return { ok = ok, message = text }
        elseif a == "mod_cancel" then
            mod_plan = nil
            refresh()
            return { ok = true, message = "Отменено" }
        end
    elseif e.panel == PANEL_PAY then
        if not pay then
            return { ok = false, message = "Окно выплаты закрыто" }
        end
        local a = e.action
        if pay.stage == "prepare" and e.values and e.values.amount then
            recalc(e.values.amount)
        end
        if a == "select" then
            if pay.stage ~= "prepare" then
                return { ok = false, message = "Выплата уже отправлена" }
            end
            for _, t in ipairs(players_in(pay.point)) do
                if tostring(t.net_id) == e.row then
                    pay.recipient = t
                    refresh()
                    return { ok = true, message = "Получатель: " .. t.name }
                end
            end
            return { ok = false, message = "Игрок вышел из зоны" }
        elseif a == "calc" then
            refresh()
            return { ok = pay.calc ~= nil, message = pay.calc and payout.text(pay.calc, pay.unit) or (pay.problem or "введите сумму") }
        elseif a == "confirm" then
            local ok, why = confirm_pay()
            return { ok = ok, message = ok and "Выплата отправлена" or ("Недоступно: " .. why) }
        elseif a == "go" then
            local p = pay.point
            local way = gtproxy.map.route(p.x, p.y, { walk = true })
            if not way.ok then
                return { ok = false, message = "Пешком к точке не дойти: " .. way.reason }
            end
            local walk = gtproxy.walk.to(p.x, p.y)
            pay.route = way.tiles
            refresh()
            return { ok = walk.ok, message = walk.ok and string.format("Идём к точке (%d шаг.)", way.steps) or walk.message }
        elseif a == "close" then
            if pay.stage == "dialog" or pay.stage == "sent" then
                return { ok = false, message = "Ждём ответа сервера — закрыть нельзя" }
            end
            pay = nil
            refresh()
            return { ok = true, message = "Закрыто" }
        end
    end
end)

---------------------------------------------------------------------------
-- Leaving the world
---------------------------------------------------------------------------

local function reset_session(why)
    if pay and (pay.stage == "dialog" or pay.stage == "sent") then
        finish_pay(pay.stage == "sent" and "Результат неизвестен" or "Отклонено", why)
    end
    pay, mod_plan, mod_batch, picking = nil, nil, nil, nil
    gtproxy.map.pick(nil)
    gtproxy.map.overlay(PANEL, nil)
end

gtproxy.on("world_menu", function()
    reset_session("вы покинули мир")
    refresh()
end)
gtproxy.on("world_enter", function()
    reset_session("вы перешли в другой мир")
    refresh()
end)
gtproxy.on("disconnect", function()
    reset_session("связь прервалась")
    running = {}
    gtproxy.panel.remove(PANEL)
    gtproxy.panel.remove(PANEL_PAY)
end)
gtproxy.on("settings", function()
    if pay and pay.stage == "prepare" then
        pay.percent = tonumber(gtproxy.setting("commission_percent")) or 0
        pay.item, pay.unit = currency(), unit()
        recalc()
    end
    refresh()
end)

gtproxy.every(1000, function()
    if world_key() then
        refresh()
    end
end)

local function add_command(name, description, run)
    for _, candidate in ipairs({ name, "ah_" .. name }) do
        if gtproxy.command(candidate, description, run) then
            commands[name] = candidate
            if candidate ~= name then
                gtproxy.log.warn(string.format("/%s is taken by another command; autohost uses /%s", name, candidate))
            end
            return
        end
    end
    gtproxy.log.warn("autohost: no command for " .. name)
end

for n = 1, POINTS do
    add_command("pos" .. n, string.format("Autohost: point %d = the tile you stand on", n), cmd_pos(n))
end
add_command("host", "Autohost: host start|stop|reset", cmd_host)
for n = 1, POINTS do
    add_command("win" .. n, string.format("Autohost: payout window of point %d: win%d [amount]", n, n), cmd_win(n))
end

refresh()
