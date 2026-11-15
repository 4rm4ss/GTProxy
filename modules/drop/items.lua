-- Item helpers of the drop module. They only work on text and numbers (no
-- game state), so tests/modules/lua/test_drop_items.lua can check them.

local M = {}

M.FIST = 18
M.WRENCH = 32
M.WORLD_LOCK = 242
M.DIAMOND_LOCK = 1796
M.BLUE_GEM_LOCK = 7188
M.FLAG_WORN = 0x01

-- Names the proxy knows without items.dat.
local KNOWN_NAMES = {
    [M.WORLD_LOCK] = "World Lock",
    [M.DIAMOND_LOCK] = "Diamond Lock",
    [M.BLUE_GEM_LOCK] = "Blue Gem Lock",
}

-- Locks in World Locks. By ID, or by name for locks whose ID differs
-- between game versions.
local LOCK_BY_ID = { [M.WORLD_LOCK] = 1, [M.DIAMOND_LOCK] = 100, [M.BLUE_GEM_LOCK] = 10000 }
local LOCK_BY_NAME = {
    ["World Lock"] = 1,
    ["Diamond Lock"] = 100,
    ["Blue Gem Lock"] = 10000,
    ["Black Gem Lock"] = 1000000,
}

function M.known_name(id)
    return KNOWN_NAMES[id]
end

function M.lock_value(id, name)
    return LOCK_BY_ID[id] or LOCK_BY_NAME[name or ""] or 0
end

-- What the game sends when you press Drop / Trash on an item.
function M.drop_request(id)
    return string.format("action|drop\n|itemID|%d\n", id)
end

function M.trash_request(id)
    return string.format("action|trash\n|itemID|%d\n", id)
end

-- The answer to the "how many?" dialog.
function M.dialog_reply(dialog_name, id, count)
    return string.format("action|dialog_return\ndialog_name|%s\nitemID|%d|\ncount|%d\n", dialog_name, id, count)
end

-- "a|b||c" -> { "a", "b", "", "c" }
local function split(text, separator)
    local parts, start = {}, 1
    while true do
        local at = string.find(text, separator, start, true)
        if not at then
            parts[#parts + 1] = string.sub(text, start)
            return parts
        end
        parts[#parts + 1] = string.sub(text, start, at - 1)
        start = at + 1
    end
end
M.split = split

-- A whole number written with digits only, up to 'max'.
local function number(text, max)
    if type(text) ~= "string" or not text:match("^%d+$") then
        return nil
    end
    local value = tonumber(text)
    if max and value > max then
        return nil
    end
    return math.tointeger(value) or value
end
M.number = number

function M.strip_colors(text)
    return (string.gsub(text, "`.?", ""))
end

-- "How many to destroy? (you have 200)" -> 200
local function owned_amount(text)
    local low = string.lower(text)
    local at = string.find(low, "you have ", 1, true)
    if not at then
        return nil
    end
    return number(low:match("^%d+", at + 9), 0xFFFFFFFF)
end

-- The server's "how many?" dialog for an item (drop, trash, recycle):
-- { dialog_name, item_id, item_name, count } or nil for any other dialog.
-- count is the amount the dialog offers, or what you own.
function M.parse_item_dialog(markup)
    local dialog = { dialog_name = "", item_id = 0, item_name = "", count = 0 }
    local prefilled, owned, has_item

    for _, line in ipairs(split(markup, "\n")) do
        local parts = split(line, "|")
        local key = parts[1]
        if key == "end_dialog" and #parts > 1 then
            dialog.dialog_name = parts[2]
        elseif key == "embed_data" and #parts > 2 and parts[2] == "itemID" then
            local id = number(parts[3], 0xFFFF)
            if id then
                dialog.item_id = id
                has_item = true
            end
        elseif key == "add_text_input" and #parts > 3 and parts[2] == "count" then
            local value = number(parts[4], 0xFFFFFFFF)
            if value and value > 0 then
                prefilled = value
            end
        elseif (key == "add_textbox" or key == "add_smalltext") and #parts > 1 then
            local value = owned_amount(parts[2])
            if value then
                owned = value
            end
        elseif key == "add_label_with_icon" and #parts > 2 and dialog.item_name == "" then
            local title = M.strip_colors(parts[3])
            for _, prefix in ipairs({ "Drop ", "Trash ", "Recycle " }) do
                if title:sub(1, #prefix) == prefix then
                    title = title:sub(#prefix + 1)
                    break
                end
            end
            dialog.item_name = title
        end
    end

    if not has_item or dialog.dialog_name == "" then
        return nil
    end
    dialog.count = prefilled or owned or 0
    if dialog.dialog_name == "trash_item" and owned then
        dialog.count = owned
    end
    return dialog
end

-- Locks worth exactly 'amount' World Locks from what you have:
-- have = { { id =, value =, count = } ... }. Each lock is worth 100 of the
-- next smaller one, so taking as many big locks as fit is always right.
-- Returns { { id, count } ... } (big locks first) or nil.
function M.pick_locks(have, amount)
    local sorted = {}
    for i, lock in ipairs(have) do
        sorted[i] = lock
    end
    table.sort(sorted, function(a, b) return a.value > b.value end)

    local picked = {}
    for _, lock in ipairs(sorted) do
        if amount == 0 then
            break
        end
        if lock.value > 0 and lock.count > 0 then
            local take = math.min(lock.count, amount // lock.value)
            if take > 0 then
                picked[#picked + 1] = { lock.id, take }
                amount = amount - take * lock.value
            end
        end
    end

    if amount ~= 0 then
        return nil
    end
    return picked
end

-- 10205 -> "1 BGL 2 DL 5 WL (= 10205 WL)"
function M.lock_text(world_locks)
    if world_locks == 0 then
        return "no locks"
    end
    local bgl = world_locks // 10000
    local dl = world_locks % 10000 // 100
    local wl = world_locks % 100
    local out = ""
    if bgl > 0 then out = out .. string.format("%d BGL ", bgl) end
    if dl > 0 then out = out .. string.format("%d DL ", dl) end
    if wl > 0 then out = out .. string.format("%d WL ", wl) end
    return string.format("%s(= %d WL)", out, world_locks)
end

-- Why /dropall keeps an item (nil: it is dropped). The worn bit is known;
-- any other bit marks the item specially in the game (favourites), so
-- those are kept rather than risk dropping them.
function M.keep_reason(item, keep_list, untradeable)
    if item.id == M.FIST or item.id == M.WRENCH then
        return "fist/wrench"
    end
    if keep_list[item.id] then
        return "keep list"
    end
    if item.flags & ~M.FLAG_WORN ~= 0 then
        return "favourite"
    end
    if item.flags & M.FLAG_WORN ~= 0 then
        return "worn"
    end
    if untradeable then
        return "untradeable"
    end
    return nil
end

-- What /dropall drops (biggest stacks first) and what it keeps.
-- items: { { id, count, flags } ... } by ID; reason(item) -> keep reason or nil.
function M.plan_drop_all(items, reason)
    local drop, keep = {}, {}
    for _, item in ipairs(items) do
        local why = reason(item)
        if why then
            keep[#keep + 1] = { item = item, reason = why }
        else
            drop[#drop + 1] = item
        end
    end
    table.sort(drop, function(a, b)
        if a.count ~= b.count then
            return a.count > b.count
        end
        return a.id < b.id
    end)
    return { drop = drop, keep = keep }
end

-- "242,1796" <-> { [242] = true, [1796] = true }
function M.parse_keep(text)
    local list = {}
    for id in string.gmatch(text or "", "[^,]+") do
        local value = number(id, 0xFFFF)
        if value then
            list[value] = true
        end
    end
    return list
end

function M.format_keep(list)
    local ids = {}
    for id in pairs(list) do
        ids[#ids + 1] = id
    end
    table.sort(ids)
    local parts = {}
    for i, id in ipairs(ids) do
        parts[i] = tostring(id)
    end
    return table.concat(parts, ",")
end

return M
