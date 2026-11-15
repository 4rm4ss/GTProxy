-- Tests of modules/item_finder/find.lua (run like test_drop_items.lua).

local find = gtproxy.load("find.lua")

local function same(a, b, what)
    assert(a == b, string.format("%s: got %s, expected %s", what, tostring(a), tostring(b)))
end

local names = { [2] = "Dirt", [112] = "Gems", [242] = "World Lock" }
local function name_of(id) return names[id] end

do
    local x, y = find.tile_of({ x = 0 * 32 + 6, y = 4 * 32 + 6 })
    same(x, 0, "object tile x"); same(y, 4, "object tile y")
    local x2, y2 = find.tile_of({ x = 3 * 32 + 30, y = 2 * 32 + 25 })
    same(x2, 4, "object near the right edge counts to the next tile"); same(y2, 3, "and below")

    same(find.parse_query("any").any, true, "any")
    same(find.parse_query("").any, true, "empty = any")
    same(find.parse_query(" 242 ").id, 242, "an ID")
    same(find.parse_query("World").part, "world", "part of a name")
    same(find.matches(find.parse_query("lock"), 242, name_of), true, "name part matches")
    same(find.matches(find.parse_query("lock"), 2, name_of), false, "other name")
    same(find.matches(find.parse_query("2"), 242, name_of), false, "ID is exact")
end

-- The nearest by the way, not by the straight line; no way never wins.
do
    local list = {
        { object_id = 1, item = 2, tx = 8, ty = 4, steps = 17 },   -- behind the wall: close in a line, long way
        { object_id = 2, item = 2, tx = 0, ty = 4, steps = 5 },
        { object_id = 3, item = 2, tx = 10, ty = 7, steps = -1 },  -- only diagonally open
        { object_id = 4, item = 2, tx = 4, ty = 4, steps = 5 },    -- same steps: lower id first
    }
    same(find.nearest(list).object_id, 2, "nearest by steps")
    find.sort(list)
    same(list[1].object_id, 2, "sorted: shortest"); same(list[2].object_id, 4, "sorted: same steps by id")
    same(list[3].object_id, 1, "sorted: longer way"); same(list[4].object_id, 3, "sorted: no way last")
    same(find.nearest({ { object_id = 9, steps = -1 } }), nil, "none reachable")
end
