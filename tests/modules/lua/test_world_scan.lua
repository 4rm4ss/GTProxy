-- Tests of modules/world_scan/scan.lua (run like test_drop_items.lua).

local scan = gtproxy.load("scan.lua")

local function same(a, b, what)
    assert(a == b, string.format("%s: got %s, expected %s", what, tostring(a), tostring(b)))
end

do
    local rows, totals = scan.count(
        { foreground = { [2] = 10, [10] = 4 }, background = { [14] = 20, [2] = 1 } },
        { { item = 2, amount = 3 }, { item = 2, amount = 7 }, { item = 112, amount = 5 }, { item = 112, amount = 1 }, { item = 242, amount = 1 } })
    local by = {}
    for _, r in ipairs(rows) do by[r.id] = r end
    same(by[2].fg, 10, "dirt blocks"); same(by[2].bg, 1, "dirt as background")
    same(by[2].stacks, 2, "dirt stacks on the ground"); same(by[2].pieces, 10, "dirt pieces on the ground")
    same(by[112], nil, "gems are not an item row")
    same(totals.gems, 6, "gems on the ground"); same(totals.gem_stacks, 2, "gem stacks")
    same(totals.fg, 14, "blocks"); same(totals.bg, 21, "backgrounds")
    same(totals.stacks, 3, "stacks without gems"); same(totals.pieces, 11, "pieces without gems")
    same(rows[1].id, 2, "the most of an item first")

    local _, unknown = scan.count({ foreground = {}, background = {} }, nil)
    same(unknown.gems, nil, "objects not known: gems unknown, not 0")
    local _, none = scan.count({ foreground = {}, background = {} }, {})
    same(none.gems, 0, "objects known, none lying: 0 gems")
end

do
    same(scan.matches("", 2, "Dirt"), true, "empty search")
    same(scan.matches("10", 10, "Rock"), true, "ID"); same(scan.matches("10", 100, "x"), false, "ID exact")
    same(scan.matches("ock", 10, "Rock"), true, "name part")
end

-- Who took it: only when the server said so.
do
    same(scan.journal_text({ kind = "collected", known = true, amount = 3, by_name = "Bob" }, "Dirt"), "Поднял игрок Bob: Dirt x3", "by a named player")
    same(scan.journal_text({ kind = "collected", known = true, amount = 3, by_me = true }, "Dirt"), "Подняли вы: Dirt x3", "by you")
    same(scan.journal_text({ kind = "collected", known = true, amount = 3, by_name = "" }, "Dirt"), "Объект исчез: Dirt x3", "unknown player")
    same(scan.journal_text({ kind = "removed", known = true, amount = 2 }, "Dirt"), "Объект исчез: Dirt x2", "removed")
    same(scan.journal_text({ kind = "removed", known = false, amount = 0 }, "?"), "Объект исчез (неизвестно какой)", "unknown object")
    same(scan.journal_text({ kind = "dropped", known = true, amount = 5 }, "Gems"), "Появился на земле: Gems x5", "dropped")
end
