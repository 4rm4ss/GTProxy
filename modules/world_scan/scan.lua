-- Counting a world, free of game state so it can be tested
-- (tests/modules/lua/test_world_scan.lua).

local M = {}

M.GEMS = 112

-- summary: { foreground = {[id] = n}, background = {[id] = n} } (world data)
-- objects: { {item, amount}, ... } (dropped)
-- -> rows { id, fg, bg, stacks, pieces } and totals. Gems lying around are
-- counted apart; nothing missing becomes a zero (nil stays nil).
function M.count(summary, objects)
    local rows, by_id = {}, {}
    local function row(id)
        local r = by_id[id]
        if not r then
            r = { id = id, fg = 0, bg = 0, stacks = 0, pieces = 0 }
            by_id[id] = r
            rows[#rows + 1] = r
        end
        return r
    end
    local totals = { fg = 0, bg = 0, stacks = 0, pieces = 0, gems = nil, gem_stacks = nil, kinds = 0 }
    if summary then
        for id, n in pairs(summary.foreground or {}) do
            row(id).fg = row(id).fg + n
            totals.fg = totals.fg + n
        end
        for id, n in pairs(summary.background or {}) do
            row(id).bg = row(id).bg + n
            totals.bg = totals.bg + n
        end
    end
    if objects then
        totals.gems, totals.gem_stacks = 0, 0
        for _, o in ipairs(objects) do
            if o.item == M.GEMS then
                totals.gems = totals.gems + o.amount
                totals.gem_stacks = totals.gem_stacks + 1
            else
                local r = row(o.item)
                r.stacks = r.stacks + 1
                r.pieces = r.pieces + o.amount
                totals.stacks = totals.stacks + 1
                totals.pieces = totals.pieces + o.amount
            end
        end
    end
    totals.kinds = #rows
    table.sort(rows, function(a, b)
        local na, nb = a.fg + a.bg + a.pieces, b.fg + b.bg + b.pieces
        if na ~= nb then
            return na > nb
        end
        return a.id < b.id
    end)
    return rows, totals
end

-- Does a row match a search: "" (all), an ID, or part of the name.
function M.matches(query, id, name)
    query = (query or ""):match("^%s*(.-)%s*$")
    if query == "" then
        return true
    end
    local n = tonumber(query)
    if n then
        return n == id
    end
    return name ~= nil and name:lower():find(query:lower(), 1, true) ~= nil
end

-- A journal line for an object event. by_name only when the server said who
-- (the netID of the player in the packet); otherwise "Объект исчез".
function M.journal_text(e, name)
    local what = string.format("%s x%d", name, e.amount)
    if e.kind == "dropped" then
        return "Появился на земле: " .. what
    elseif e.kind == "changed" then
        if e.old_amount and e.old_amount >= 0 then
            return string.format("Кучка изменилась: %s, было %d", what, e.old_amount)
        end
        return "Кучка изменилась: " .. what
    elseif e.kind == "collected" then
        if not e.known then
            return "Объект исчез (неизвестно какой)"
        end
        if e.by_me then
            return "Подняли вы: " .. what
        end
        if e.by_name and e.by_name ~= "" then
            return string.format("Поднял игрок %s: %s", e.by_name, what)
        end
        return "Объект исчез: " .. what
    end
    if not e.known then
        return "Объект исчез (неизвестно какой)"
    end
    return "Объект исчез: " .. what
end

return M
