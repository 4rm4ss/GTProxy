-- Vending finder (module API 3, see docs/modules.md).
--
-- /pvend <name|ID>: the world's vending machines that sell an item, cheapest
-- per item first ("3 for 1 WL" and "2 WL for 4" are compared per item, the
-- machine's own pack shown next to it); equal prices: the one you can reach
-- by the shorter way first. The map window's tab "Вендинги" has the table:
-- a click on a row shows the machine and the way to a free tile next to it.
--
-- What the proxy knows: the world data has every machine's item and price;
-- the stock only comes with the machine's dialog (your wrench, or "Проверить"
-- here, which asks the server the same way). Until then: "Остаток неизвестен".
--
-- A purchase is one ordinary vending purchase, never repeated by itself:
--   Купить -> «Ожидает подтверждения» (amount and total in whole packs) ->
--   Подтвердить -> the machine is asked again (world, machine, item, price,
--   stock must be the same, else the new offer waits for a new confirmation)
--   -> «Отправлено» -> «Подтверждено» (your inventory got the items) /
--   «Отклонено» (the server said no) / «Результат неизвестен» (no answer in
--   buy_timeout_ms; check your inventory before buying again).
--
-- Settings (config/modules/vend_finder.json): presets, check_timeout_ms,
-- buy_timeout_ms, buycount ("items": the amount typed is in items, "packs":
-- in packs, as your server's vending dialog expects).

local price = gtproxy.load("price.lua")
local P = gtproxy.prefix
local PANEL = "vend"

local query = ""            -- "" = every machine
local qty = 1
local selected = nil        -- "x,y"
local known = {}            -- "x,y" -> { item, stock, pack } from the machine's dialog
local probe = nil           -- { key, timer }: "Проверить" waiting for the dialog
local deal = nil            -- the purchase (see the top)
local last_tile = nil       -- your tile when the table was made
local command_name = nil

local function key_of(x, y)
    return string.format("%d,%d", x, y)
end

local function world_name()
    local info = gtproxy.world.info()
    return info.in_world and info.name or nil
end

local function item_name(id)
    if id == nil or id == 0 then
        return "пусто"
    end
    local info = gtproxy.items.get(id)
    return info and info.name or string.format("предмет %d", id)
end

local function game_name(id)
    local info = gtproxy.items.get(id)
    return info and info.name or string.format("item %d", id)
end

local function setting_number(name, low, high)
    local value = tonumber(gtproxy.setting(name)) or low
    return math.max(low, math.min(high, math.floor(value)))
end

local function presets()
    local list = {}
    for part in tostring(gtproxy.setting("presets")):gmatch("[^,%s]+") do
        local n = tonumber(part)
        if n and n >= 1 and n == math.floor(n) then
            list[#list + 1] = math.floor(n)
        end
    end
    if #list == 0 then
        list = { 1, 10, 50 }
    end
    return list
end

-- Tiles next to a machine you could stand on, and the machine itself if it
-- is not solid.
local function around(x, y)
    return { { x = x, y = y }, { x = x - 1, y = y }, { x = x + 1, y = y }, { x = x, y = y - 1 }, { x = x, y = y + 1 } }
end

---------------------------------------------------------------------------
-- The offers
---------------------------------------------------------------------------

local function offers()
    local list = {}
    local spots, owner = {}, {}
    for _, v in ipairs(gtproxy.world.vendings()) do
        local name = v.item ~= 0 and item_name(v.item) or nil
        if price.matches(query, v.item, name) or (v.item == 0 and (query == "" or query == "any")) then
            local key = key_of(v.x, v.y)
            local info = known[key]
            if info and info.item ~= v.item then
                info = nil      -- the machine sells something else now
                known[key] = nil
            end
            local offer = {
                key = key, x = v.x, y = v.y, item = v.item, name = name or "пусто",
                pack = price.from_world(v.price), raw_price = v.price,
                stock = info and info.stock or nil, steps = -1,
            }
            -- The machine's window is newer than the world data, as long as
            -- the world data has not changed since.
            if info and info.pack and (offer.pack == nil or info.world_price == v.price) then
                offer.pack = info.pack
            end
            list[#list + 1] = offer
            for _, spot in ipairs(around(v.x, v.y)) do
                spots[#spots + 1] = spot
                owner[#spots] = offer
            end
        end
    end
    -- One search for every machine: the shortest way to a tile next to it.
    if #spots > 0 then
        local steps = gtproxy.map.distances(spots)
        for i, s in ipairs(steps) do
            local offer = owner[i]
            if s >= 0 and (offer.steps < 0 or s < offer.steps) then
                offer.steps = s
                offer.stand = spots[i]
            end
        end
    end
    return price.sort(list)
end

local function find_offer(key)
    for _, offer in ipairs(offers()) do
        if offer.key == key then
            return offer
        end
    end
    return nil
end

local function stock_text(offer)
    if offer.item == 0 then
        return "0"
    end
    return offer.stock and tostring(offer.stock) or "Остаток неизвестен"
end

local function way_text(offer)
    if offer.steps < 0 then
        return "нет пути"
    end
    return string.format("%d шаг.", offer.steps)
end

---------------------------------------------------------------------------
-- The map window: marks and the tab
---------------------------------------------------------------------------

local function show_marks(list)
    local markers = {}
    local route = nil
    for i, offer in ipairs(list) do
        if i > 60 then
            break
        end
        local chosen = offer.key == selected
        markers[#markers + 1] = {
            x = offer.x, y = offer.y,
            label = offer.pack and price.number(price.per_item(offer.pack)) .. " WL" or "?",
            color = chosen and "#ffcf4a" or (offer.pack and "#7ee787" or "#8b949e"),
        }
        if chosen and offer.stand then
            local way = gtproxy.map.route(offer.stand.x, offer.stand.y)
            if way.ok then
                route = way.tiles
                markers[#markers + 1] = { x = offer.stand.x, y = offer.stand.y, label = "встать здесь", color = "#c77dff" }
            end
        end
    end
    if #markers == 0 then
        gtproxy.map.overlay(PANEL, nil)
        return
    end
    gtproxy.map.overlay(PANEL, { markers = markers, route = route, route_color = "#c77dff" })
end

local function deal_lines()
    if not deal then
        return {}
    end
    local d = deal
    local lines = {}
    lines[#lines + 1] = string.format("Покупка: %s — %s", d.name, d.status)
    lines[#lines + 1] = string.format("Автомат %d, %d: %s (%s WL за 1 шт.)", d.x, d.y, price.pack_text(d.pack), price.number(price.per_item(d.pack)))
    lines[#lines + 1] = string.format("К покупке: %d шт. (%d пач.) за %s WL; запрошено %d.", d.amount, d.packs, price.number(d.total), d.wanted)
    if not d.exact then
        lines[#lines + 1] = d.capped and "Остатка не хватает: количество уменьшено до целых пачек в наличии."
            or "Автомат продаёт целыми пачками: количество округлено вверх до целых пачек."
    end
    if d.changed then
        lines[#lines + 1] = "Предложение изменилось: " .. d.changed .. " Подтвердите заново."
    end
    if d.detail then
        lines[#lines + 1] = d.detail
    end
    return lines
end

local function refresh()
    local world = world_name()
    if not world then
        gtproxy.panel.set(PANEL, { title = "Вендинги", note = "Вы не в мире.", columns = {}, rows = {} })
        gtproxy.map.overlay(PANEL, nil)
        return
    end
    local list = offers()
    local me = gtproxy.world.info().me
    last_tile = me and key_of(me.tile_x, me.tile_y) or nil

    local rows = {}
    for _, offer in ipairs(list) do
        local unit = offer.pack and price.per_item(offer.pack) or nil
        rows[#rows + 1] = {
            id = offer.key,
            cells = {
                offer.name .. (offer.item ~= 0 and string.format(" (%d)", offer.item) or ""),
                offer.item == 0 and "—" or (offer.pack and price.pack_text(offer.pack) or "Цена неизвестна"),
                unit and price.number(unit) or (offer.item == 0 and "—" or "Цена неизвестна"),
                stock_text(offer),
                string.format("%d, %d", offer.x, offer.y),
                way_text(offer),
            },
            sort = {
                offer.name, unit or 1e18, unit or 1e18, offer.stock or -1, offer.y * 10000 + offer.x,
                offer.steps >= 0 and offer.steps or 1e9,
            },
            tile = { x = offer.x, y = offer.y },
            style = offer.item == 0 and "muted" or (offer.pack and offer.steps >= 0 and "ok" or (offer.pack and "" or "muted")),
            selected = offer.key == selected,
            actions = offer.item ~= 0 and { { id = "check", label = "Проверить" } } or nil,
        }
    end

    local buttons = {}
    for _, n in ipairs(presets()) do
        buttons[#buttons + 1] = { id = "preset", label = tostring(n), value = tostring(n) }
    end
    if deal and deal.stage == "confirm" then
        buttons[#buttons + 1] = { id = "confirm", label = "Подтвердить покупку" }
        buttons[#buttons + 1] = { id = "cancel", label = "Отмена" }
    else
        buttons[#buttons + 1] = { id = "buy", label = "Купить выбранное" }
    end
    buttons[#buttons + 1] = { id = "refresh", label = "Обновить" }

    local note = {
        string.format("Мир %s: автоматов %d%s.", world, #list, query ~= "" and (", поиск «" .. query .. "»") or ""),
        "Сортировка: цена за 1 шт., при равной — доступный и более короткий путь. Клик по строке — автомат и путь на карте.",
    }
    for _, line in ipairs(deal_lines()) do
        note[#note + 1] = line
    end
    gtproxy.panel.set(PANEL, {
        title = "Вендинги",
        note = table.concat(note, "\n"),
        search = { value = query, placeholder = "Название или ID предмета (пусто — все)" },
        inputs = { { id = "qty", label = "Количество", value = tostring(qty), size = 6 } },
        buttons = buttons,
        columns = { "Предмет", "Цена (как в автомате)", "WL за 1 шт.", "Остаток", "Коорд.", "Подход" },
        rows = rows,
        empty = query ~= "" and "Нет автоматов с этим предметом в мире." or "В мире нет торговых автоматов.",
    })
    show_marks(list)
end

---------------------------------------------------------------------------
-- The purchase
---------------------------------------------------------------------------

local function busy()
    return deal ~= nil and (deal.stage == "checking" or deal.stage == "sent")
end

local function finish(status, detail)
    if not deal then
        return
    end
    gtproxy.cancel(deal.timer)
    deal.timer = nil
    deal.stage = "done"
    deal.status = status
    deal.detail = detail
    gtproxy.log.info(string.format("vend: %s x%d at %d,%d: %s%s", game_name(deal.item), deal.amount, deal.x, deal.y, status, detail and (" - " .. detail) or ""))
    refresh()
end

-- A new offer from the selected row: whole packs, capped by the stock.
local function prepare(offer, wanted)
    if offer.item == 0 then
        return nil, "Автомат пуст."
    end
    if not offer.pack then
        return nil, "Цена неизвестна: автомат не продаёт или цена ещё не пришла. Нажмите «Проверить»."
    end
    if offer.stock == 0 then
        return nil, "Автомат пуст (остаток 0)."
    end
    local buy = price.purchase(offer.pack, wanted, offer.stock)
    if buy.amount == 0 then
        return nil, string.format("В автомате меньше одной пачки (%s).", price.pack_text(offer.pack))
    end
    return {
        stage = "confirm", status = "Ожидает подтверждения",
        key = offer.key, x = offer.x, y = offer.y, item = offer.item, name = offer.name,
        pack = offer.pack, raw_price = offer.raw_price, stock = offer.stock, wanted = wanted,
        amount = buy.amount, packs = buy.packs, total = buy.total, exact = buy.exact, capped = buy.capped,
        world = world_name(),
    }
end

local function same_pack(a, b)
    return a ~= nil and b ~= nil and a.wl == b.wl and a.count == b.count
end

-- The machine's dialog for the purchase being checked: the same offer ->
-- buy; anything else -> the new offer waits for a new confirmation.
local function check_and_buy(dialog, info)
    local d = deal
    local tile = gtproxy.world.tile(d.x, d.y)
    local changes = {}
    if world_name() ~= d.world then
        changes[#changes + 1] = "другой мир"
    end
    if not tile or tile.extra ~= "vending" then
        changes[#changes + 1] = "автомата больше нет"
    elseif tile.vend_item ~= d.item or (info.item and info.item ~= d.item) then
        changes[#changes + 1] = string.format("теперь продаёт %s", item_name(info.item or tile.vend_item))
    end
    local pack = price.from_world(tile and tile.vend_price or 0) or info.pack
    if info.pack and pack and not same_pack(info.pack, pack) then
        pack = info.pack    -- the dialog is newer than the world data
    end
    if not same_pack(pack, d.pack) then
        changes[#changes + 1] = string.format("цена %s вместо %s", pack and price.pack_text(pack) or "неизвестна", price.pack_text(d.pack))
    end
    if info.stock and info.stock < d.amount then
        changes[#changes + 1] = string.format("в наличии %d вместо нужных %d", info.stock, d.amount)
    end

    if #changes > 0 then
        local offer = find_offer(d.key)
        local fresh = offer and prepare(offer, d.wanted) or nil
        if not fresh then
            finish("Отменено", "Предложение изменилось (" .. table.concat(changes, ", ") .. ") и купить нельзя.")
            return
        end
        fresh.changed = table.concat(changes, ", ") .. "."
        deal = fresh
        refresh()
        return
    end

    -- The field the server reads the amount from.
    local field = nil
    for _, name in ipairs(dialog.input_list) do
        if name == "buycount" or (not field and name:lower():find("count", 1, true)) then
            field = name
        end
    end
    field = field or dialog.input_list[1]
    if not field then
        finish("Отменено", "Окно автомата не похоже на покупку (нет поля количества); ничего не отправлено.")
        return
    end
    local count = gtproxy.setting("buycount") == "packs" and d.packs or d.amount
    local before = gtproxy.inventory.count(d.item)
    if not gtproxy.server.text(gtproxy.ui.answer(dialog, { [field] = count })) then
        finish("Отклонено", "Нет связи с сервером; ничего не отправлено.")
        return
    end
    d.stage, d.status, d.before, d.detail = "sent", "Отправлено", before, nil
    gtproxy.cancel(d.timer)
    d.timer = gtproxy.after(setting_number("buy_timeout_ms", 500, 60000), function()
        if deal == d and d.stage == "sent" then
            d.timer = nil
            finish("Результат неизвестен", "Сервер не подтвердил покупку вовремя. Проверьте инвентарь; повторно не отправлялось.")
        end
    end)
    gtproxy.log.info(string.format("vend: buying %s x%d (%d packs, %s WL) at %d,%d", game_name(d.item), d.amount, d.packs, price.number(d.total), d.x, d.y))
    refresh()
end

-- The vending machine's dialog (your wrench, "Проверить" or a purchase check).
gtproxy.hook_call("OnDialogRequest", function(call)
    local markup = call.args[1]
    if type(markup) ~= "string" then
        return
    end
    local dialog = gtproxy.ui.parse_dialog(markup)
    if not dialog.name:lower():find("vend", 1, true) then
        return
    end
    local info = price.from_dialog(dialog)
    local key = (info.x and info.y) and key_of(info.x, info.y) or nil
    -- Without coordinates in the dialog: the machine we asked for.
    if not key then
        key = (deal and deal.stage == "checking" and deal.key) or (probe and probe.key) or nil
    end
    if not key then
        return
    end
    local x, y = key:match("^(%-?%d+),(%-?%d+)$")
    local tile = gtproxy.world.tile(tonumber(x), tonumber(y))
    known[key] = { item = info.item or (tile and tile.vend_item) or 0, stock = info.stock, pack = info.pack, world_price = tile and tile.vend_price or nil }

    if deal and deal.stage == "checking" and deal.key == key then
        gtproxy.cancel(deal.timer)
        deal.timer = nil
        check_and_buy(dialog, info)
        return "cancel"
    end
    if probe and probe.key == key then
        gtproxy.cancel(probe.timer)
        probe = nil
        refresh()
        return "cancel"
    end
    refresh()
end)

gtproxy.on("inventory", function(e)
    local d = deal
    if not d or d.stage ~= "sent" or (not e.full and e.item ~= d.item) then
        return
    end
    local got = gtproxy.inventory.count(d.item) - d.before
    if got >= d.amount then
        finish("Подтверждено", string.format("Получено %d шт.", got))
    elseif got > 0 then
        finish("Подтверждено", string.format("Получено %d из %d шт. (сервер выдал меньше).", got, d.amount))
    end
end)

local REFUSALS = { "can't", "cannot", "not enough", "don't have", "do not have", "out of stock", "has changed",
    "is empty", "no longer", "too far", "not allowed", "failed", "full", "only buy", "multiple" }

gtproxy.on("console", function(e)
    local d = deal
    if not d or d.stage ~= "sent" then
        return
    end
    local text = (e.clean or ""):lower()
    for _, word in ipairs(REFUSALS) do
        if text:find(word, 1, true) then
            finish("Отклонено", "Сервер: " .. e.clean)
            return
        end
    end
    if text:find("bought", 1, true) or text:find("purchased", 1, true) then
        finish("Подтверждено", "Сервер: " .. e.clean)
    end
end)

---------------------------------------------------------------------------
-- The tab's buttons
---------------------------------------------------------------------------

local function wanted_from(values)
    local n = tonumber(values and values.qty or "")
    if not n or n < 1 or n ~= math.floor(n) or n > 1000000 then
        return nil
    end
    return math.floor(n)
end

gtproxy.on("panel", function(e)
    if e.panel ~= PANEL then
        return
    end
    local a = e.action
    if e.values and e.values.qty and wanted_from(e.values) then
        qty = wanted_from(e.values)
    end

    if a == "search" then
        query = (e.value or ""):match("^%s*(.-)%s*$")
        selected = nil
        refresh()
        return { ok = true, message = "" }
    elseif a == "refresh" then
        refresh()
        return { ok = true, message = "Обновлено" }
    elseif a == "preset" then
        qty = tonumber(e.value) or qty
        refresh()
        return { ok = true, message = "Количество: " .. qty }
    elseif a == "select" then
        selected = e.row
        refresh()
        local offer = find_offer(e.row)
        if not offer then
            return { ok = false, message = "Этого автомата больше нет" }
        end
        if offer.steps < 0 then
            return { ok = false, message = string.format("%d, %d: к автомату нет пути", offer.x, offer.y) }
        end
        return { ok = true, message = string.format("%d, %d: путь %d шаг. до %d, %d", offer.x, offer.y, offer.steps, offer.stand.x, offer.stand.y) }
    elseif a == "check" then
        if busy() then
            return { ok = false, message = "Идёт покупка — дождитесь результата" }
        end
        if probe then
            return { ok = false, message = "Уже ждём ответ автомата" }
        end
        local x, y = (e.row or ""):match("^(%-?%d+),(%-?%d+)$")
        if not x then
            return { ok = false, message = "Выберите автомат" }
        end
        if not gtproxy.server.wrench(tonumber(x), tonumber(y)) then
            return { ok = false, message = "Нет связи с сервером или вы не в мире" }
        end
        local p = { key = e.row }
        p.timer = gtproxy.after(setting_number("check_timeout_ms", 500, 30000), function()
            if probe == p then
                probe = nil
            end
        end)
        probe = p
        return { ok = true, message = "Спрашиваем автомат " .. e.row }
    elseif a == "buy" then
        if busy() then
            return { ok = false, message = "Покупка уже выполняется — ждите результата" }
        end
        if not selected then
            return { ok = false, message = "Сначала выберите автомат в таблице" }
        end
        local wanted = wanted_from(e.values)
        if not wanted then
            return { ok = false, message = "Количество — целое число от 1" }
        end
        local offer = find_offer(selected)
        if not offer then
            return { ok = false, message = "Этого автомата больше нет" }
        end
        local fresh, why = prepare(offer, wanted)
        if not fresh then
            return { ok = false, message = why }
        end
        deal = fresh
        refresh()
        return { ok = true, message = string.format("%d шт. за %s WL — подтвердите", fresh.amount, price.number(fresh.total)) }
    elseif a == "confirm" then
        if not deal or deal.stage ~= "confirm" then
            return { ok = false, message = busy() and "Покупка уже выполняется — ждите результата" or "Нечего подтверждать" }
        end
        if world_name() ~= deal.world then
            finish("Отменено", "Вы в другом мире.")
            return { ok = false, message = "Отменено: другой мир" }
        end
        local d = deal
        d.stage, d.status, d.changed = "checking", "Проверка автомата…", nil
        if not gtproxy.server.wrench(d.x, d.y) then
            finish("Отклонено", "Нет связи с сервером; ничего не отправлено.")
            return { ok = false, message = "Нет связи с сервером" }
        end
        d.timer = gtproxy.after(setting_number("check_timeout_ms", 500, 30000), function()
            if deal == d and d.stage == "checking" then
                d.timer = nil
                finish("Отменено", "Автомат не ответил на проверку (далеко или занят); покупка не отправлена.")
            end
        end)
        refresh()
        return { ok = true, message = "Проверяем автомат…" }
    elseif a == "cancel" then
        if busy() then
            return { ok = false, message = "Запрос уже отправлен — отменить нельзя" }
        end
        if deal and deal.stage == "confirm" then
            finish("Отменено", "Вы отменили покупку.")
        end
        return { ok = true, message = "Отменено" }
    end
end)

---------------------------------------------------------------------------
-- Leaving the world, changes in it
---------------------------------------------------------------------------

local function reset(why)
    if deal and deal.stage == "sent" then
        gtproxy.log.warn(string.format("vend: %s before the server answered the purchase of %s", why, game_name(deal.item)))
        gtproxy.game.message(string.format("`4Vending: ``the result of buying %s is unknown (%s). Check your inventory.", game_name(deal.item), why))
    end
    if deal then
        gtproxy.cancel(deal.timer)
    end
    if probe then
        gtproxy.cancel(probe.timer)
    end
    deal, probe, selected, known = nil, nil, nil, {}
    gtproxy.map.overlay(PANEL, nil)
end

gtproxy.on("world_menu", function()
    reset("you left the world")
    refresh()
end)
gtproxy.on("world_enter", function()
    reset("you went to another world")
    refresh()
end)
gtproxy.on("disconnect", function()
    reset("the connection closed")
    gtproxy.panel.remove(PANEL)
end)

gtproxy.on("tile", function(e)
    local key = key_of(e.x, e.y)
    local tile = gtproxy.world.tile(e.x, e.y)
    local is_vending = tile ~= nil and tile.extra == "vending"
    if not is_vending and known[key] == nil and not (deal and deal.key == key) and selected ~= key then
        -- Not a machine; still, a block may open or close a way.
        if e.cause == "placed" or e.cause == "broken" then
            refresh()
        end
        return
    end
    if not is_vending or (known[key] and known[key].item ~= tile.vend_item) then
        known[key] = nil
    end
    if deal and deal.key == key and deal.stage == "confirm" then
        local pack = is_vending and price.from_world(tile.vend_price) or nil
        if not is_vending or tile.vend_item ~= deal.item or not same_pack(pack, deal.pack) then
            local offer = find_offer(key)
            local fresh = offer and prepare(offer, deal.wanted) or nil
            if fresh then
                fresh.changed = "автомат обновился."
                deal = fresh
            else
                finish("Отменено", "Автомат изменился, купить нельзя.")
                return
            end
        end
    end
    refresh()
end)

-- The ways depend on where you stand.
gtproxy.every(1500, function()
    local info = gtproxy.world.info()
    if not info.in_world or not info.me then
        return
    end
    if key_of(info.me.tile_x, info.me.tile_y) ~= last_tile then
        refresh()
    end
end)

---------------------------------------------------------------------------
-- The command
---------------------------------------------------------------------------

local function cmd_pvend(args, ctx)
    query = table.concat(args, " "):match("^%s*(.-)%s*$")
    selected = nil
    if not world_name() then
        ctx.reply("`4Oops: ``you are not in a world.")
        return
    end
    refresh()
    local list = offers()
    local selling = {}
    for _, offer in ipairs(list) do
        if offer.item ~= 0 then
            selling[#selling + 1] = offer
        end
    end
    if #selling == 0 then
        ctx.reply(query == "" and "`oNo vending machines sell anything in this world." or string.format("`oNo vending machine sells `w%s`` here.", gtproxy.text.sanitize(query)))
        return
    end
    ctx.reply(string.format("`o%d machine(s)%s. Cheapest first (per item):", #selling, query ~= "" and (" with `w" .. gtproxy.text.sanitize(query) .. "``") or ""))
    for i, offer in ipairs(selling) do
        if i > 5 then
            ctx.reply(string.format("`o... and %d more: open the map window (%smap), vend_finder tab.", #selling - 5, P))
            break
        end
        ctx.reply(string.format("`o%s at `w%d, %d``: %s%s, stock %s, %s",
            gtproxy.text.sanitize(game_name(offer.item)), offer.x, offer.y,
            offer.pack and price.pack_text(offer.pack) or "price unknown",
            offer.pack and string.format(" (%s WL each)", price.number(price.per_item(offer.pack))) or "",
            offer.stock and tostring(offer.stock) or "unknown",
            offer.steps >= 0 and string.format("%d steps away", offer.steps) or "no way there"))
    end
end

-- /pvend, or /vend_pvend if something else already has /pvend.
for _, name in ipairs({ "pvend", "vend_pvend" }) do
    if gtproxy.command(name, "Vending machines selling an item, cheapest first: pvend <name|ID>", cmd_pvend) then
        command_name = name
        break
    end
end
if command_name ~= "pvend" then
    gtproxy.log.warn("/pvend is taken by another command; vend_finder uses " .. (command_name and ("/" .. command_name) or "no command"))
end

refresh()
