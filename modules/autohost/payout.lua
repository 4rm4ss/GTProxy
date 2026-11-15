-- Payouts and points, free of game state so they can be tested
-- (tests/modules/lua/test_autohost_payout.lua).

local M = {}

M.ROUNDING = {
    down = "комиссия округляется вниз (в пользу игрока)",
    up = "комиссия округляется вверх (в пользу хоста)",
    nearest = "комиссия округляется до ближайшего целого (0.5 — вверх)",
}

-- amount: whole number > 0, percent: 0..100, rounding: down|up|nearest
-- -> { amount, percent, exact, deduction, result } or nil, why
function M.compute(amount, percent, rounding)
    if type(amount) ~= "number" or amount < 1 or amount ~= math.floor(amount) then
        return nil, "сумма — целое число от 1"
    end
    if type(percent) ~= "number" or percent < 0 or percent > 100 then
        return nil, "комиссия — от 0 до 100 %"
    end
    if not M.ROUNDING[rounding] then
        return nil, "округление: down, up или nearest"
    end
    local exact = amount * percent / 100
    local deduction
    if rounding == "down" then
        deduction = math.floor(exact + 1e-9)
    elseif rounding == "up" then
        deduction = math.ceil(exact - 1e-9)
    else
        deduction = math.floor(exact + 0.5 + 1e-9)
    end
    return { amount = amount, percent = percent, exact = exact, deduction = deduction, result = amount - deduction }
end

function M.number(n)
    if n == math.floor(n) then
        return string.format("%d", math.floor(n))
    end
    return (string.format("%.2f", n):gsub("0+$", ""):gsub("%.$", ""))
end

-- "100 × 10% = 10 → 90"
function M.text(calc, unit)
    return string.format("%d %s × %s%% = %s → удержано %d, к выплате %d %s",
        calc.amount, unit, M.number(calc.percent), M.number(calc.exact), calc.deduction, calc.result, unit)
end

-- A point as stored: "x,y,x1,y1,x2,y2,name" (the name may hold commas).
function M.encode_point(p)
    return string.format("%d,%d,%d,%d,%d,%d,%s", p.x, p.y, p.x1, p.y1, p.x2, p.y2, p.name or "")
end

function M.decode_point(text)
    if type(text) ~= "string" then
        return nil
    end
    local x, y, x1, y1, x2, y2, name = text:match("^(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),(%-?%d+),(.*)$")
    if not x then
        return nil
    end
    return { x = tonumber(x), y = tonumber(y), x1 = tonumber(x1), y1 = tonumber(y1), x2 = tonumber(x2), y2 = tonumber(y2), name = name }
end

-- A zone around a tile.
function M.zone_around(x, y, radius)
    return { x1 = x - radius, y1 = y - radius, x2 = x + radius, y2 = y + radius }
end

function M.inside(x, y, z)
    return x >= math.min(z.x1, z.x2) and x <= math.max(z.x1, z.x2) and y >= math.min(z.y1, z.y2) and y <= math.max(z.y1, z.y2)
end

-- Are you near enough to a point (steps in x or y).
function M.near(me, point, tiles)
    return math.abs(me.x - point.x) <= tiles and math.abs(me.y - point.y) <= tiles
end

return M
