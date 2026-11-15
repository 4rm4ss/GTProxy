-- Tests of modules/autohost/payout.lua (run like test_drop_items.lua).

local payout = gtproxy.load("payout.lua")

local function same(a, b, what)
    assert(a == b, string.format("%s: got %s, expected %s", what, tostring(a), tostring(b)))
end

do
    local c = assert(payout.compute(100, 10, "down"))
    same(c.deduction, 10, "100 at 10%: 10 held back"); same(c.result, 90, "100 at 10%: 90 paid")
    same(payout.text(c, "WL"), "100 WL × 10% = 10 → удержано 10, к выплате 90 WL", "text")

    same(assert(payout.compute(15, 10, "down")).result, 14, "15 at 10%, down: 1 held, 14 paid")
    same(assert(payout.compute(15, 10, "up")).result, 13, "15 at 10%, up: 2 held, 13 paid")
    same(assert(payout.compute(15, 10, "nearest")).result, 13, "15 at 10%, nearest: 1.5 -> 2")
    same(assert(payout.compute(14, 10, "nearest")).result, 13, "14 at 10%, nearest: 1.4 -> 1")
    same(assert(payout.compute(7, 0, "down")).result, 7, "no commission")
    same(assert(payout.compute(7, 100, "down")).result, 0, "all held back")
    same(assert(payout.compute(30, 10, "up")).deduction, 3, "exact 3 stays 3 when rounding up")

    same((payout.compute(0, 10, "down")), nil, "0 refused")
    same((payout.compute(2.5, 10, "down")), nil, "not whole refused")
    same((payout.compute(10, 150, "down")), nil, "over 100% refused")
    same((payout.compute(10, 10, "sideways")), nil, "unknown rounding refused")
end

do
    local p = { x = 4, y = 8, x1 = 3, y1 = 7, x2 = 5, y2 = 9, name = "Стол, левый" }
    local back = payout.decode_point(payout.encode_point(p))
    same(back.x, 4, "point x"); same(back.y2, 9, "zone"); same(back.name, "Стол, левый", "name with a comma")
    same(payout.decode_point("garbage"), nil, "broken point")
    same(payout.inside(5, 9, p), true, "in the zone"); same(payout.inside(6, 9, p), false, "outside")
    same(payout.near({ x = 6, y = 8 }, p, 2), true, "2 tiles away is near")
    same(payout.near({ x = 7, y = 8 }, p, 2), false, "3 tiles away is not")
end
