-- Entry rules and the blacklist of config/modules/world_moderation.json,
-- free of game state so they can be tested (tests/modules/lua/test_moderation_rules.lua).
--
-- "rules": rules separated by ';', each a few key=value words:
--     world=MYWORLD action=kick name=Spammer1
--     world=* action=ban id=12345
--   world: the world's name or * (every world); action: pull, kick or ban;
--   name: the account name, matched exactly (letter case does not matter);
--   id: the account's ID (user ID), which a name change does not change.
-- "exceptions": never acted on, e.g. "name=MyFriend; id=777".

local M = {}

M.ACTIONS = { pull = true, kick = true, ban = true }

local function trim(text)
    return (text or ""):match("^%s*(.-)%s*$")
end

local function words(part)
    local fields = {}
    for key, value in part:gmatch("([%a_]+)%s*=%s*([^%s]+)") do
        fields[key:lower()] = value
    end
    return fields
end

-- -> list of rules, or nil and "rule 2: ..." when something is wrong.
function M.parse_rules(text)
    local list = {}
    local index = 0
    for part in (text or ""):gmatch("[^;]+") do
        part = trim(part)
        if part ~= "" then
            index = index + 1
            local f = words(part)
            local rule = { world = (f.world or "*"):upper(), action = (f.action or ""):lower() }
            if not M.ACTIONS[rule.action] then
                return nil, string.format("rule %d (%s): action must be pull, kick or ban", index, part)
            end
            if f.name and f.id then
                return nil, string.format("rule %d (%s): give name or id, not both", index, part)
            end
            if f.name then
                rule.name = f.name:lower()
            elseif f.id then
                rule.id = tonumber(f.id)
                if not rule.id or rule.id ~= math.floor(rule.id) or rule.id <= 0 then
                    return nil, string.format("rule %d (%s): id must be a whole number above 0", index, part)
                end
            else
                return nil, string.format("rule %d (%s): needs name=... or id=...", index, part)
            end
            for key in pairs(f) do
                if key ~= "world" and key ~= "action" and key ~= "name" and key ~= "id" then
                    return nil, string.format("rule %d (%s): unknown word '%s' (world, action, name, id)", index, part, key)
                end
            end
            list[#list + 1] = rule
        end
    end
    return list
end

-- -> { names = {[lower name] = true}, ids = {[id] = true} } or nil, why
function M.parse_exceptions(text)
    local result = { names = {}, ids = {} }
    local index = 0
    for part in (text or ""):gmatch("[^;,]+") do
        part = trim(part)
        if part ~= "" then
            index = index + 1
            local f = words(part)
            if f.name then
                result.names[f.name:lower()] = true
            elseif f.id and tonumber(f.id) then
                result.ids[math.floor(tonumber(f.id))] = true
            else
                return nil, string.format("exception %d (%s): write name=... or id=...", index, part)
            end
        end
    end
    return result
end

-- The rule for a player entering `world`, or nil. name: the clean name.
function M.match(rules, exceptions, world, name, user_id)
    local lower = (name or ""):lower()
    if exceptions and (exceptions.names[lower] or (user_id and exceptions.ids[user_id])) then
        return nil
    end
    world = (world or ""):upper()
    for _, rule in ipairs(rules or {}) do
        if rule.world == "*" or rule.world == world then
            if (rule.name and rule.name == lower) or (rule.id and user_id and rule.id == user_id) then
                return rule
            end
        end
    end
    return nil
end

-- Buttons of the player's window (wrench) for each action; the first one
-- the window has is pressed.
M.BUTTONS = { pull = { "pull" }, kick = { "kick" }, ban = { "worldban", "world_ban", "ban" } }

-- Which button of a parsed dialog does `action`, or nil.
function M.button_for(dialog, action)
    for _, name in ipairs(M.BUTTONS[action] or {}) do
        if dialog.buttons[name] ~= nil then
            return name
        end
    end
    return nil
end

local REFUSALS = { "can't", "cannot", "not allowed", "permission", "don't have", "do not have", "unable", "immune", "failed", "only the", "not the owner", "too far" }

function M.is_refusal(text)
    text = (text or ""):lower()
    for _, word in ipairs(REFUSALS) do
        if text:find(word, 1, true) then
            return true
        end
    end
    return false
end

-- Is a tile inside a rectangle (corners in any order).
function M.inside(x, y, zone)
    local x1, x2 = math.min(zone.x1, zone.x2), math.max(zone.x1, zone.x2)
    local y1, y2 = math.min(zone.y1, zone.y2), math.max(zone.y1, zone.y2)
    return x >= x1 and x <= x2 and y >= y1 and y <= y2
end

return M
