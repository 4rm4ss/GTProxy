-- Tests of modules/vend_finder/price.lua (run like test_drop_items.lua).

local price = gtproxy.load("price.lua")

local function same(a, b, what)
    assert(a == b, string.format("%s: got %s, expected %s", what, tostring(a), tostring(b)))
end

-- World data: > 0 WL each, < 0 items per WL, 0 none.
do
    local each = price.from_world(5)
    same(each.wl, 5, "5 WL each: wl"); same(each.count, 1, "5 WL each: count")
    local per = price.from_world(-3)
    same(per.wl, 1, "3 per WL: wl"); same(per.count, 3, "3 per WL: count")
    same(price.from_world(0), nil, "price 0 is unknown")
    same(price.pack_text(per), "3 for 1 WL", "pack text 3 for 1")
    same(price.pack_text({ wl = 2, count = 4 }), "2 WL for 4", "pack text 2 WL for 4")
    same(price.pack_text(each), "5 WL each", "pack text each")
    same(price.pack_text(nil), "price unknown", "pack text unknown")
end

-- "3 for 1 WL" and "2 WL for 4" compared per item: 0.333 < 0.5.
do
    local offers = {
        { x = 9, y = 1, pack = { wl = 2, count = 4 }, steps = 3 },
        { x = 1, y = 1, pack = nil, steps = 1 },
        { x = 5, y = 1, pack = { wl = 1, count = 3 }, steps = 20 },
        { x = 7, y = 1, pack = { wl = 1, count = 2 }, steps = -1 },   -- same price as 2 for 4, no way
        { x = 3, y = 1, pack = { wl = 1, count = 2 }, steps = 8 },    -- same price, longer than 3
    }
    price.sort(offers)
    same(offers[1].x, 5, "cheapest per item first (3 for 1 WL)")
    same(offers[2].x, 9, "equal price: the shorter way first")
    same(offers[3].x, 3, "equal price: then the longer way")
    same(offers[4].x, 7, "equal price: no way after those with one")
    same(offers[5].x, 1, "unknown price last")
    same(price.number(price.per_item({ wl = 1, count = 3 })), "0.3333", "per item 1/3")
end

-- Whole packs only, the stock caps them, never rounded quietly.
do
    local b = price.purchase({ wl = 1, count = 3 }, 10)
    same(b.packs, 4, "10 at 3 per WL: 4 packs"); same(b.amount, 12, "10 at 3 per WL: 12 items")
    same(b.total, 4, "10 at 3 per WL: 4 WL"); same(b.exact, false, "10 at 3 per WL: not exact")
    local c = price.purchase({ wl = 2, count = 4 }, 8)
    same(c.amount, 8, "8 at 2 WL for 4"); same(c.total, 4, "8 at 2 WL for 4 costs 4 WL"); same(c.exact, true, "exact")
    local d = price.purchase({ wl = 1, count = 3 }, 10, 7)
    same(d.amount, 6, "stock 7: 2 packs"); same(d.capped, true, "capped by stock")
    local e = price.purchase({ wl = 1, count = 3 }, 10, 2)
    same(e.amount, 0, "stock below one pack: nothing")
    local f = price.purchase({ wl = 5, count = 1 }, 1)
    same(f.total, 5, "1 at 5 WL each")
end

-- The machine's own dialog.
do
    local d = gtproxy.ui.parse_dialog(
        "set_default_color|`o\nadd_label_with_icon|big|`wVending Machine``|left|2978|\n"
        .. "add_label_with_icon|small|`2Dirt``|left|2|\n"
        .. "add_textbox|The machine contains a total of 115 Dirt.|left|\n"
        .. "add_textbox|`52 World Locks`` for 4 Dirt.|left|\n"
        .. "add_text_input|buycount|Buy how many?|0|5|\n"
        .. "embed_data|tilex|6\nembed_data|tiley|8\nembed_data|expectprice|-2\nembed_data|expectitem|2\n"
        .. "end_dialog|vending|Cancel|Buy|\n")
    local info = price.from_dialog(d)
    same(info.x, 6, "dialog tile x"); same(info.y, 8, "dialog tile y"); same(info.item, 2, "dialog item")
    same(info.stock, 115, "dialog stock")
    same(info.pack.wl, 2, "dialog pack wl"); same(info.pack.count, 4, "dialog pack count")

    local per = price.from_dialog(gtproxy.ui.parse_dialog(
        "add_textbox|The machine contains a total of 30 Dirt.|left|\nadd_textbox|3 Dirt per World Lock.|left|\n"
        .. "embed_data|tilex|2\nembed_data|tiley|8\nembed_data|expectitem|2\nend_dialog|vending|Cancel|Buy|\n"))
    same(per.pack.wl, 1, "per WL pack wl"); same(per.pack.count, 3, "per WL pack count")

    local empty = price.from_dialog(gtproxy.ui.parse_dialog(
        "add_textbox|This machine is empty.|left|\nembed_data|tilex|1\nembed_data|tiley|1\nend_dialog|vending|Close||\n"))
    same(empty.stock, 0, "empty machine: stock 0")
    same(empty.pack, nil, "empty machine: no price")

    local by_embed = price.from_dialog(gtproxy.ui.parse_dialog(
        "add_textbox|Buy something?|left|\nembed_data|expectprice|5\nembed_data|expectitem|2\nend_dialog|vending|Cancel|Buy|\n"))
    same(by_embed.pack.wl, 5, "price from expectprice"); same(by_embed.stock, nil, "no stock in the text: unknown")

    local diamond = price.from_dialog(gtproxy.ui.parse_dialog("add_textbox|1 Diamond Lock each.|left|\nend_dialog|vending|||\n"))
    same(diamond.pack.wl, 100, "1 Diamond Lock each = 100 WL")
end

-- Search: ID, part of the name, any.
do
    same(price.matches("2", 2, "Dirt"), true, "by ID")
    same(price.matches("2", 22, "Dirt Seed"), false, "ID is exact")
    same(price.matches("irt", 2, "Dirt"), true, "part of the name")
    same(price.matches("DIRT", 2, "Dirt"), true, "letter case")
    same(price.matches("any", 5, nil), true, "any")
    same(price.matches("rock", 2, "Dirt"), false, "no match")
end
