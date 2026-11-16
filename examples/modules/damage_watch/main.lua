-- Damage watch (module API 5) - an example of the packet interceptor.
--
-- Install: copy this folder to modules/ next to Proxy.exe and restart it.
--
-- What it is for: every server kills and revives a player its own way. This
-- module writes to the proxy log every packet that has to do with damage or
-- death, both ways, with its numbers - so you can see what your server
-- really sends. Then it can block them in different ways, to find the one
-- that gives you no damage without leaving you dead and stuck:
--
--   /dmg log on|off                 write the packets (default: on)
--   /dmg block none                 block nothing (watch only)
--   /dmg block flags                hide the damage from the server
--                                   (the game's state keeps the damage bits
--                                   out; the server may then never revive you)
--   /dmg block death                let the game report the damage, but keep
--                                   the server's death and respawn from it
--   /dmg block all                  both
--   /dmg                            what it is doing now
--
-- Run into lava once with each setting and look at the log (logs\ next to
-- Proxy.exe) and at your character. Nothing here is saved to the server:
-- the proxy only drops or changes packets on their way.

local P = gtproxy.prefix

-- The bits the game sets in its state when it is hurt.
local FIRE_DAMAGE = 1 << 6
local KILLED = 1 << 8
local DAMAGE = 1 << 16
local ACID_DAMAGE = 1 << 25
local HURT = FIRE_DAMAGE | KILLED | DAMAGE | ACID_DAMAGE

-- Calls from the server that have to do with dying.
local DEATH_CALLS = {
    OnKilled = true, OnDie = true, OnRespawn = true, OnSetFreezeState = true,
    OnPlayPositioned = true, OnSetPos = true,
}

local function logging()
    return gtproxy.setting("log") == true
end

local function blocking()
    return gtproxy.setting("block")
end

local function flags_text(flags)
    local parts = {}
    if flags & FIRE_DAMAGE ~= 0 then parts[#parts + 1] = "fire" end
    if flags & KILLED ~= 0 then parts[#parts + 1] = "killed" end
    if flags & DAMAGE ~= 0 then parts[#parts + 1] = "damage" end
    if flags & ACID_DAMAGE ~= 0 then parts[#parts + 1] = "acid" end
    return #parts > 0 and table.concat(parts, "+") or "none"
end

-- From your game to the server.
gtproxy.packet.on("game", function(p)
    if p.type == "state" and p.flags and p.flags & HURT ~= 0 then
        if logging() then
            gtproxy.log.info(string.format("game -> server: state flags 0x%X (%s) at %.0f, %.0f",
                p.flags, flags_text(p.flags), p.pos_x or 0, p.pos_y or 0))
        end
        local block = blocking()
        if block == "flags" or block == "all" then
            p.flags = p.flags & ~HURT
            return "replace"
        end
        return
    end

    if p.text and (p.text:find("respawn", 1, true) or p.text:find("killed", 1, true)) then
        if logging() then
            gtproxy.log.info("game -> server: " .. p.text:gsub("\n", " | "))
        end
        local block = blocking()
        if block == "flags" or block == "all" then
            return "cancel"
        end
    end
end)

-- From the server to your game.
gtproxy.packet.on("server", function(p)
    if p.call and DEATH_CALLS[p.call] then
        if logging() then
            gtproxy.log.info(string.format("server -> game: %s (netID %s, %.0f, %.0f)",
                p.call, tostring(p.net_id), p.pos_x or 0, p.pos_y or 0))
        end
        local block = blocking()
        if (block == "death" or block == "all") and p.call ~= "OnSetPos" then
            return "cancel"
        end
        return
    end

    if p.type == "state" and p.flags and p.flags & HURT ~= 0 then
        if logging() then
            gtproxy.log.info(string.format("server -> game: state flags 0x%X (%s) netID %s", p.flags, flags_text(p.flags), tostring(p.net_id)))
        end
        local block = blocking()
        if block == "death" or block == "all" then
            p.flags = p.flags & ~HURT
            return "replace"
        end
    end
end)

gtproxy.command("dmg", "Damage watch: dmg log on|off | dmg block none|flags|death|all", function(args, ctx)
    local word = (args[1] or ""):lower()
    if word == "log" then
        local on = (args[2] or ""):lower() ~= "off"
        gtproxy.save_setting("log", on)
    elseif word == "block" then
        local how = (args[2] or ""):lower()
        if how ~= "none" and how ~= "flags" and how ~= "death" and how ~= "all" then
            ctx.reply("`4Usage: ``" .. P .. "dmg block none|flags|death|all")
            return
        end
        gtproxy.save_setting("block", how)
    elseif word ~= "" then
        ctx.reply("`4Usage: ``" .. P .. "dmg log on|off | " .. P .. "dmg block none|flags|death|all")
        return
    end
    ctx.reply(string.format("`oDamage watch: log `w%s``, blocking `w%s``. The log is in logs\\ next to Proxy.exe.",
        logging() and "on" or "off", blocking()))
end)
