-- Choosing among dropped items, free of game state so it can be tested
-- (tests/modules/lua/test_item_find.lua).

local M = {}

-- The tile an object lies in (its position is in pixels).
function M.tile_of(object)
    return math.floor((object.x + 8) / 32), math.floor((object.y + 8) / 32)
end

-- "any" / "" / an ID / part of a name. `name_of(id)` gives a name or nil.
function M.parse_query(text)
    text = (text or ""):match("^%s*(.-)%s*$")
    if text == "" or text:lower() == "any" or text == "*" then
        return { any = true, text = "any" }
    end
    local id = tonumber(text)
    if id and id == math.floor(id) and id >= 0 then
        return { id = math.floor(id), text = text }
    end
    return { part = text:lower(), text = text }
end

function M.matches(query, item, name_of)
    if query.any then
        return true
    end
    if query.id then
        return item == query.id
    end
    local name = name_of(item)
    return name ~= nil and name:lower():find(query.part, 1, true) ~= nil
end

-- objects: { object_id, item, amount, x, y, tx, ty, steps } (steps -1: no way).
-- Sorted: reachable first, shorter way first, then by object id.
function M.sort(objects)
    table.sort(objects, function(a, b)
        local ra, rb = a.steps >= 0, b.steps >= 0
        if ra ~= rb then
            return ra
        end
        if a.steps ~= b.steps then
            return a.steps < b.steps
        end
        return a.object_id < b.object_id
    end)
    return objects
end

-- The nearest reachable one, or nil.
function M.nearest(objects)
    local best = nil
    for _, o in ipairs(objects) do
        if o.steps >= 0 and (best == nil or o.steps < best.steps or (o.steps == best.steps and o.object_id < best.object_id)) then
            best = o
        end
    end
    return best
end

return M
