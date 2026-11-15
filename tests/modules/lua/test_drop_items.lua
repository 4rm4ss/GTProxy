-- Tests of modules/drop/items.lua, run by tests/modules/test_shipped_modules.cpp
-- as a module next to a copy of items.lua: the module loads only if every
-- check passes (a failing one is in the log with its line).

local items = gtproxy.load("items.lua")

local function same(a, b, what)
    local function show(v)
        if type(v) ~= "table" then return tostring(v) end
        local parts = {}
        for i, x in ipairs(v) do parts[i] = show(x) end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    assert(show(a) == show(b), string.format("%s: got %s, expected %s", what, show(a), show(b)))
end

-- The server's drop dialog: count from the input, name from the title.
do
    local dialog = items.parse_item_dialog(
        "set_default_color|`o\n"
        .. "add_label_with_icon|big|`wDrop Diamond Lock``|left|1796|\n"
        .. "add_textbox|How many to drop?|left|\n"
        .. "add_text_input|count||7|5|\n"
        .. "embed_data|itemID|1796\n"
        .. "end_dialog|drop_item|Cancel|OK|\n")
    assert(dialog, "drop dialog is read")
    same(dialog.dialog_name, "drop_item", "drop dialog name")
    same(dialog.item_id, 1796, "drop dialog item")
    same(dialog.count, 7, "drop dialog count")
    same(dialog.item_name, "Diamond Lock", "drop dialog item name")
end

-- Trash: what you own ("you have 143"), not the prefilled 0.
do
    local dialog = items.parse_item_dialog(
        "add_label_with_icon|big|`4Trash`` `wDirt``|left|2|\n"
        .. "add_textbox|How many to `4destroy``? (you have 143)|left|\n"
        .. "add_text_input|count||0|5|\n"
        .. "embed_data|itemID|2\n"
        .. "end_dialog|trash_item|Cancel|OK|\n")
    assert(dialog, "trash dialog is read")
    same(dialog.dialog_name, "trash_item", "trash dialog name")
    same(dialog.item_id, 2, "trash dialog item")
    same(dialog.count, 143, "trash dialog count")
    same(dialog.item_name, "Dirt", "trash dialog item name")
end

-- Other dialogs are not item dialogs; another dialog about an item is read.
assert(items.parse_item_dialog("add_button|x|X|noflags|0|0|\nend_dialog|popup|Close||\n") == nil, "popup")
assert(items.parse_item_dialog("end_dialog|drop_item|Cancel|OK|\n") == nil, "no item id")
assert(items.parse_item_dialog("") == nil, "empty")
assert(items.parse_item_dialog("embed_data|itemID|70000\nend_dialog|drop_item|Cancel|OK|\n") == nil, "item id too big")
same(items.parse_item_dialog("embed_data|itemID|242\nend_dialog|item_info|Close||\n").dialog_name, "item_info", "item info dialog")

-- Requests and the answer, exactly as the game sends them.
same(items.drop_request(242), "action|drop\n|itemID|242\n", "drop request")
same(items.trash_request(2), "action|trash\n|itemID|2\n", "trash request")
same(items.dialog_reply("drop_item", 242, 5), "action|dialog_return\ndialog_name|drop_item\nitemID|242|\ncount|5\n", "dialog reply")

-- /dd: biggest locks first, the rest in smaller ones.
do
    local have = { { id = 242, value = 1, count = 150 }, { id = 1796, value = 100, count = 5 }, { id = 7188, value = 10000, count = 1 } }
    same(items.pick_locks(have, 300), { { 1796, 3 } }, "300 WL")
    same(items.pick_locks(have, 10250), { { 7188, 1 }, { 1796, 2 }, { 242, 50 } }, "10250 WL")
    same(items.pick_locks(have, 640), { { 1796, 5 }, { 242, 140 } }, "640 WL: not enough DL")
    same(items.pick_locks(have, 3), { { 242, 3 } }, "3 WL")
    assert(items.pick_locks({ { id = 1796, value = 100, count = 5 } }, 250) == nil, "no WL for the 50")
    assert(items.pick_locks(have, 20000) == nil, "only one BGL")
    assert(items.pick_locks({}, 1) == nil, "no locks")
end

-- Lock values: known IDs without items.dat, newer locks by name.
same(items.lock_value(242), 1, "WL")
same(items.lock_value(1796), 100, "DL")
same(items.lock_value(7188), 10000, "BGL")
same(items.known_name(242), "World Lock", "WL name")
same(items.known_name(2), nil, "no name for dirt")
same(items.lock_value(20000, "Black Gem Lock"), 1000000, "Black Gem Lock")
same(items.lock_value(202, "Small Lock"), 0, "Small Lock")
same(items.lock_value(243, "World Lock Seed"), 0, "World Lock Seed")

same(items.lock_text(0), "no locks", "no locks")
same(items.lock_text(10205), "1 BGL 2 DL 5 WL (= 10205 WL)", "lock text")
same(items.lock_text(300), "3 DL (= 300 WL)", "lock text DL")

-- /dropall keeps fist/wrench, the keep list, favourites, worn, untradeable.
do
    local keep = { [10] = true }
    same(items.keep_reason({ id = 18, count = 1, flags = 0 }, keep, false), "fist/wrench", "fist")
    same(items.keep_reason({ id = 10, count = 50, flags = 0 }, keep, false), "keep list", "keep list")
    same(items.keep_reason({ id = 4, count = 3, flags = 0x02 }, keep, false), "favourite", "favourite")
    same(items.keep_reason({ id = 48, count = 1, flags = 0x01 }, keep, false), "worn", "worn")
    same(items.keep_reason({ id = 5, count = 1, flags = 0 }, keep, true), "untradeable", "untradeable")
    same(items.keep_reason({ id = 2, count = 200, flags = 0 }, keep, false), nil, "dirt is dropped")

    local plan = items.plan_drop_all({
        { id = 2, count = 200, flags = 0 }, { id = 4, count = 3, flags = 2 }, { id = 10, count = 50, flags = 0 },
        { id = 18, count = 1, flags = 0 }, { id = 242, count = 5, flags = 0 }, { id = 1796, count = 5, flags = 0 },
    }, function(item) return items.keep_reason(item, {}, false) end)
    local order = {}
    for i, item in ipairs(plan.drop) do order[i] = item.id end
    same(order, { 2, 10, 242, 1796 }, "biggest stacks first, ties by ID")
    same(#plan.keep, 2, "kept")
end

same(items.format_keep(items.parse_keep("1796, 242,x,242")), "242,1796", "keep list text skips what is not an ID")
same(items.format_keep(items.parse_keep("1796,242")), "242,1796", "keep list round trip")
same(items.strip_colors("`wDrop `4Dirt``"), "Drop Dirt", "strip colours")

gtproxy.log.info("drop items: all checks passed")
