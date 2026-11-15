-- Prices of vending machines, free of game state so they can be tested
-- (tests/modules/lua/test_vend_price.lua).
--
-- A price is a pack: `wl` World Locks for `count` items. The world data has
-- one number per machine: > 0 = that many World Locks for one item, < 0 =
-- that many items for one World Lock, 0 = not for sale (or unknown). The
-- machine's own dialog may say more ("2 World Locks for 4 Dirt").

local M = {}

M.LOCKS = { ["world lock"] = 1, ["diamond lock"] = 100, ["blue gem lock"] = 10000, ["black gem lock"] = 1000000 }

-- From the world data. nil when there is no price.
function M.from_world(price)
    if not price or price == 0 then
        return nil
    end
    if price > 0 then
        return { wl = price, count = 1 }
    end
    return { wl = 1, count = -price }
end

function M.per_item(pack)
    return pack.wl / pack.count
end

-- "3 for 1 WL", "2 WL for 4", "5 WL each"
function M.pack_text(pack)
    if not pack then
        return "price unknown"
    end
    if pack.count == 1 then
        return string.format("%s WL each", M.number(pack.wl))
    end
    if pack.wl == 1 then
        return string.format("%d for 1 WL", pack.count)
    end
    return string.format("%s WL for %d", M.number(pack.wl), pack.count)
end

function M.number(n)
    if n == math.floor(n) then
        return string.format("%d", math.floor(n))
    end
    return (string.format("%.4f", n):gsub("0+$", ""):gsub("%.$", ""))
end

local function lock_value(words)
    return M.LOCKS[string.lower(words)] or M.LOCKS[string.lower(words):gsub("s$", "")]
end

-- The machine's dialog as the server sent it (gtproxy.ui.parse_dialog):
-- { item = id or nil, stock = n or nil, pack = {wl, count} or nil }.
function M.from_dialog(dialog)
    local info = {}
    local text = string.lower(dialog.text or "")
    info.x = tonumber(dialog.embed.tilex or "")
    info.y = tonumber(dialog.embed.tiley or "")
    info.item = tonumber(dialog.embed.expectitem or dialog.embed.itemid or dialog.embed.itemID or "")
    local expect = tonumber(dialog.embed.expectprice or "")

    -- Stock: "contains a total of 115", "has 115 left", "stock: 115"
    local stock = text:match("contains a total of (%d+)") or text:match("contains (%d+)")
        or text:match("has (%d+) [%a%s]-left") or text:match("stock:?%s*(%d+)") or text:match("(%d+) in stock")
    if text:find("is empty", 1, true) or text:find("out of stock", 1, true) then
        stock = "0"
    end
    info.stock = tonumber(stock or "")

    -- Price: "2 world locks for 4", "3 per world lock", "5 world locks each"
    local wl, unit, count = text:match("(%d+)%s+([%a ]-locks?)%s+for%s+(%d+)")
    if wl and lock_value(unit) then
        info.pack = { wl = tonumber(wl) * lock_value(unit), count = tonumber(count) }
    else
        -- "3 per world lock", "3 dirt per world lock"
        local per, unit2 = text:match("(%d+)[%a%s]-%s+per%s+([%a ]-lock)")
        if per and lock_value(unit2) then
            info.pack = { wl = lock_value(unit2), count = tonumber(per) }
        else
            local each, unit3 = text:match("(%d+)%s+([%a ]-locks?)%s+each")
            if each and lock_value(unit3) then
                info.pack = { wl = tonumber(each) * lock_value(unit3), count = 1 }
            end
        end
    end
    if not info.pack and expect and expect ~= 0 then
        info.pack = M.from_world(expect)
    end
    return info
end

-- How many you get and pay for `wanted` items: whole packs only, never
-- rounded silently - `exact` tells whether it is what you asked for.
-- stock (if known) caps it.
function M.purchase(pack, wanted, stock)
    local packs = math.ceil(wanted / pack.count)
    local capped = false
    if stock and packs * pack.count > stock then
        packs = math.floor(stock / pack.count)
        capped = true
    end
    local amount = packs * pack.count
    return {
        packs = packs,
        amount = amount,
        total = packs * pack.wl,
        exact = amount == wanted,
        capped = capped,
    }
end

-- Offers: known prices first, cheapest per item; equal price -> reachable
-- and shorter way first; unknown prices last.
function M.sort(offers)
    table.sort(offers, function(a, b)
        if (a.pack ~= nil) ~= (b.pack ~= nil) then
            return a.pack ~= nil
        end
        if a.pack and b.pack then
            local pa, pb = M.per_item(a.pack), M.per_item(b.pack)
            if math.abs(pa - pb) > 1e-9 then
                return pa < pb
            end
        end
        local da = a.steps and a.steps >= 0 and a.steps or math.huge
        local db = b.steps and b.steps >= 0 and b.steps or math.huge
        if da ~= db then
            return da < db
        end
        if a.y ~= b.y then
            return a.y < b.y
        end
        return a.x < b.x
    end)
    return offers
end

-- Does an item match the search: an ID, or part of its name.
function M.matches(query, id, name)
    if query == nil or query == "" or query == "any" then
        return true
    end
    local number = tonumber(query)
    if number then
        return number == id
    end
    return name ~= nil and string.find(string.lower(name), string.lower(query), 1, true) ~= nil
end

return M
