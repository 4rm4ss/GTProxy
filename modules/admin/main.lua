-- Admin mode (module API 4, see docs/modules.md).
--
-- Your character's abilities on your own server, the way the server gives
-- them with items: walking speed, gravity (lower = higher and longer jumps),
-- acceleration, speed in water, double jump, walking through blocks, a longer
-- punch / build reach, and god mode (the damage the game reports - lava, acid,
-- hits - is not passed on to the server; a death on spikes is not reported
-- and you are put back where you stood).
--
-- The proxy applies them to the server's character state as it reaches your
-- game (the game itself moves you: this is your own movement with the arrow
-- keys, not the proxy's walking engine). Refused on Growtopia's own servers:
-- there they would be cheats against other players.
--
-- The game knows one jump in the air (double jump); more and higher jumps
-- come from lower gravity.
--
-- Settings: config/modules/admin.json, or the Admin page of /proxy (which
-- writes the same file); /speed, /jump, /dj, /noclip, /range, /god.

local P = gtproxy.prefix
local ICON = 32             -- Wrench
local refused_said = false

local LIMITS = {
    speed = { 0.05, 20 },
    gravity = { 0.05, 20 },
    acceleration = { 0.05, 20 },
    water_speed = { 0.05, 20 },
    range = { 0, 100 },
}

local PRESETS = {
    { "normal", "Normal", { speed = 1, gravity = 1, acceleration = 1, water_speed = 1, double_jump = false, noclip = false, range = 0 } },
    { "fast", "Fast (speed x2)", { speed = 2, acceleration = 2 } },
    { "faster", "Very fast (speed x4)", { speed = 4, acceleration = 4 } },
    { "jump", "High jumps (gravity x0.4, double jump)", { gravity = 0.4, double_jump = true } },
    { "float", "Floating (gravity x0.1, double jump)", { gravity = 0.1, double_jump = true } },
    { "builder", "Builder (reach +10)", { range = 10 } },
}

local function say(text)
    gtproxy.game.message(text)
end

local function number(n)
    if n == math.floor(n) then
        return string.format("%d", math.floor(n))
    end
    return (string.format("%.2f", n):gsub("0+$", ""):gsub("%.$", ""))
end

-- The settings in force -> your character.
local function apply()
    local a = gtproxy.player.abilities({
        speed = gtproxy.setting("speed"),
        gravity = gtproxy.setting("gravity"),
        acceleration = gtproxy.setting("acceleration"),
        water_speed = gtproxy.setting("water_speed"),
        double_jump = gtproxy.setting("double_jump"),
        noclip = gtproxy.setting("noclip"),
        range = gtproxy.setting("range"),
        god = gtproxy.setting("god"),
    })
    return a
end

local function changed_anything(a)
    return a.speed ~= 1 or a.gravity ~= 1 or a.acceleration ~= 1 or a.water_speed ~= 1 or a.double_jump or a.noclip or a.range ~= 0 or a.god
end

local function describe(a)
    local parts = {}
    if a.speed ~= 1 then parts[#parts + 1] = "speed x" .. number(a.speed) end
    if a.gravity ~= 1 then parts[#parts + 1] = "gravity x" .. number(a.gravity) end
    if a.acceleration ~= 1 then parts[#parts + 1] = "acceleration x" .. number(a.acceleration) end
    if a.water_speed ~= 1 then parts[#parts + 1] = "water x" .. number(a.water_speed) end
    if a.double_jump then parts[#parts + 1] = "double jump" end
    if a.noclip then parts[#parts + 1] = "through blocks" end
    if a.range ~= 0 then parts[#parts + 1] = "reach +" .. a.range end
    if a.god then parts[#parts + 1] = "god mode" end
    return #parts > 0 and table.concat(parts, ", ") or "normal"
end

-- What the settings ask for (applied or not).
local function wanted()
    return {
        speed = gtproxy.setting("speed"), gravity = gtproxy.setting("gravity"),
        acceleration = gtproxy.setting("acceleration"), water_speed = gtproxy.setting("water_speed"),
        double_jump = gtproxy.setting("double_jump"), noclip = gtproxy.setting("noclip"),
        range = gtproxy.setting("range"), god = gtproxy.setting("god"),
    }
end

local function tell_refused(a)
    if a.allowed or refused_said or not changed_anything(wanted()) then
        return
    end
    refused_said = true
    say("`4Admin mode is off here: ``" .. gtproxy.text.sanitize(a.why) .. ".")
end

local first = apply()
gtproxy.on("settings", function()
    tell_refused(apply())
end)
-- Each world: the decision and the game server it was made on, in the log.
gtproxy.on("world_enter", function()
    local a = gtproxy.player.abilities()
    gtproxy.log.info(string.format("Admin mode %s: %s", a.allowed and "allowed" or "refused", a.why))
    refused_said = false
    tell_refused(a)
end)

-- Saves what changed; returns the problems.
local function save(changes)
    local problems = {}
    for key, value in pairs(changes) do
        if value ~= gtproxy.setting(key) then
            local ok, why = gtproxy.save_setting(key, value)
            if not ok then
                problems[#problems + 1] = why
            end
        end
    end
    tell_refused(apply())
    return problems
end

local function report(problems)
    local a = gtproxy.player.abilities()
    if #problems > 0 then
        say("`4Admin mode: ``" .. table.concat(problems, "; "))
    elseif not a.allowed then
        say("`4Admin mode is off here: ``" .. a.why .. ".")
    else
        say("`oAdmin mode: `w" .. describe(a) .. "``.")
    end
end

---------------------------------------------------------------------------
-- The Admin page of /proxy
---------------------------------------------------------------------------

local function page()
    local a = gtproxy.player.abilities()
    local d = gtproxy.dialog():header("Admin mode", ICON)
    if a.allowed then
        d:small("`oYour own server: `2on``. Now: `w" .. describe(a) .. "``")
        if a.game_server ~= "" then
            d:small("`oGame server: `w" .. gtproxy.text.sanitize(a.game_server) .. "``")
        end
    else
        d:small("`4Off here: ``" .. gtproxy.text.sanitize(a.why))
    end
    if a.state_made then
        d:small("`oYour server sent no character state yet: the usual values are used.")
    end
    d:small("`oYour own movement (arrow keys). Saved to config/modules/admin.json.")
    d:spacer()
        :input("speed", "Walking speed x (0.05-20):", number(a.speed), 5)
        :input("gravity", "Gravity x (lower = higher, longer jumps):", number(a.gravity), 5)
        :input("acceleration", "Acceleration x:", number(a.acceleration), 5)
        :input("water_speed", "Speed in water x:", number(a.water_speed), 5)
        :input("range", "Reach + (punch / build, 0-100):", tostring(a.range), 3)
        :checkbox("double_jump", "Double jump (one more jump in the air)", a.double_jump)
        :checkbox("noclip", "Walk through blocks", a.noclip)
        :checkbox("god", "God mode: no damage (lava, acid, spikes)", a.god)
        :button("admin_save", "`2Save``")
        :spacer()
        :small("`oQuick settings:")
    for _, preset in ipairs(PRESETS) do
        d:button("admin_preset_" .. preset[1], preset[2])
    end
    return d:spacer()
        :small(string.format("`oAlso: %sspeed 2, %sjump 0.4, %sdj, %snoclip, %srange 10, %sgod", P, P, P, P, P, P))
        :button("page_main", "<< Menu")
        :finish("gtproxy_admin", "Save")
end

local function show_page()
    gtproxy.game.dialog(page())
end

local function read_page(values)
    local changes = {
        double_jump = values.double_jump == "1",
        noclip = values.noclip == "1",
        god = values.god == "1",
    }
    local bad = {}
    for key, limit in pairs(LIMITS) do
        local raw = values[key]
        if raw ~= nil then
            local value = tonumber(gtproxy.text.sanitize(raw))
            if value and value == value then
                if key == "range" then
                    value = math.floor(value)
                end
                changes[key] = math.max(limit[1], math.min(limit[2], value))
            else
                bad[#bad + 1] = key
            end
        end
    end
    return changes, bad
end

gtproxy.on("dialog", function(e)
    if e.button == "page_admin" then
        show_page()
        return
    end
    if e.name ~= "gtproxy_admin" then
        return
    end
    local preset = e.button:match("^admin_preset_(%a+)$")
    if preset then
        for _, p in ipairs(PRESETS) do
            if p[1] == preset then
                report(save(p[3]))
            end
        end
        show_page()
        return
    end
    if e.values.speed == nil then
        return
    end
    local changes, bad = read_page(e.values)
    local problems = save(changes)
    if #bad > 0 then
        problems[#problems + 1] = "not a number: " .. table.concat(bad, ", ")
    end
    if not e.frame then
        report(problems)
        if e.button == "admin_save" then
            show_page()
        end
    end
end)

gtproxy.menu.add({
    title = "Admin mode",
    icon = ICON,
    button = "page_admin",
    label = "Speed, jumps, through blocks, reach, god mode",
    order = 12,
    status = function()
        local a = gtproxy.player.abilities()
        if not changed_anything(a) then
            return nil
        end
        return a.allowed and ("`oAdmin mode: `w" .. describe(a) .. "``") or "`4Admin mode off: ``Growtopia's own server"
    end,
})

---------------------------------------------------------------------------
-- Commands
---------------------------------------------------------------------------

local function add_command(name, description, run)
    for _, candidate in ipairs({ name, "adm_" .. name }) do
        if gtproxy.command(candidate, description, run) then
            if candidate ~= name then
                gtproxy.log.warn(string.format("/%s is taken by another command; admin uses /%s", name, candidate))
            end
            return
        end
    end
end

local function factor_command(key, word, what)
    return function(args, ctx)
        local value = tonumber(args[1] or "")
        if args[1] == "reset" or args[1] == "off" then
            value = 1
        end
        if not value then
            ctx.reply(string.format("`oUsage: %s%s <x> (1 = normal, now %s) - %s", P, word, number(gtproxy.setting(key)), what))
            return
        end
        local problems = save({ [key] = math.max(LIMITS[key][1], math.min(LIMITS[key][2], value)) })
        report(problems)
    end
end

local function toggle_command(key)
    return function(args, ctx)
        local on = not gtproxy.setting(key)
        if args[1] == "on" then on = true elseif args[1] == "off" then on = false end
        report(save({ [key] = on }))
    end
end

add_command("admin", "Admin mode page: speed, jumps, reach, god mode", function(_, ctx)
    if ctx.source == "game" then
        show_page()
    else
        ctx.reply("Admin mode: " .. describe(gtproxy.player.abilities()))
    end
end)
add_command("speed", "Your walking speed: speed <x> (1 = normal)", factor_command("speed", "speed", "your walking speed"))
add_command("jump", "Your gravity: jump <x> (lower = higher jumps, 1 = normal)", factor_command("gravity", "jump", "gravity: lower = higher, longer jumps"))
add_command("dj", "Double jump on / off", toggle_command("double_jump"))
add_command("noclip", "Walk through blocks on / off", toggle_command("noclip"))
add_command("god", "God mode (no damage) on / off", toggle_command("god"))
add_command("range", "Punch / build reach: range <tiles> (0 = normal)", function(args, ctx)
    local value = tonumber(args[1] or "")
    if not value then
        ctx.reply(string.format("`oUsage: %srange <tiles> (0-100, now +%d)", P, gtproxy.setting("range")))
        return
    end
    report(save({ range = math.max(0, math.min(100, math.floor(value))) }))
end)

if changed_anything(first) then
    gtproxy.log.info("Admin mode at start: " .. describe(first) .. (first.allowed and "" or " (refused: " .. first.why .. ")"))
end
