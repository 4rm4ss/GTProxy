-- Items and drop (module API 2, see docs/modules.md).
--
-- Your inventory with marks, dropping many stacks in a row (/dropall, the
-- lock commands /daw /dd /dw /ddl /db, /drop), fast drop and fast trash,
-- the keep list and your lock balance; the Items page of /proxy.
--
-- Every drop is the same request + "how many?" answer the game sends when
-- you press Drop, one stack at a time, each waiting for the server's reply
-- so nothing is lost or dropped twice.
--
-- Settings (config/modules/drop.json): dialog_timeout_ms, ack_timeout_ms.
-- Your choices (fast drop, fast trash, pause between drops, keep list) are
-- kept in data/drop.json.

local items = gtproxy.load("items.lua")
local P = gtproxy.prefix

local MIN_GAP, MAX_GAP = 100, 2000
local ICON_ITEMS, ICON_LOCK = 448, 242

local function clamp(value, low, high)
    return math.max(low, math.min(high, value))
end

-- Your choices, saved in data/drop.json.
local fast_drop = gtproxy.store.get("fast_drop", false) == true
local fast_trash = gtproxy.store.get("fast_trash", false) == true
local gap = clamp(math.tointeger(gtproxy.store.get("drop_gap_ms", 200)) or 200, MIN_GAP, MAX_GAP)
local keep_list = items.parse_keep(gtproxy.store.get("keep", ""))

local function save()
    gtproxy.store.set("fast_drop", fast_drop)
    gtproxy.store.set("fast_trash", fast_trash)
    gtproxy.store.set("drop_gap_ms", gap)
    gtproxy.store.set("keep", items.format_keep(keep_list))
end

local function message(text)
    gtproxy.game.message(text)
end

local sanitize = gtproxy.text.sanitize

-- "Dirt", the name the proxy knows, or "item 2".
local function name_of(id)
    local info = gtproxy.items.get(id)
    if info then
        return info.name
    end
    return items.known_name(id) or string.format("item %d", id)
end

-- The same for dialogs.
local function shown_name(id)
    local info = gtproxy.items.get(id)
    if info then
        return sanitize(info.name)
    end
    return items.known_name(id) or string.format("Item %d", id)
end

local function inventory()
    return gtproxy.inventory.info()
end

-- Locks you carry, most valuable first: { { id =, value =, count = } }.
local function locks()
    local out = {}
    for _, item in ipairs(inventory().items) do
        local value = items.lock_value(item.id, name_of(item.id))
        if value > 0 and item.count > 0 then
            out[#out + 1] = { id = item.id, value = value, count = item.count }
        end
    end
    table.sort(out, function(a, b) return a.value > b.value end)
    return out
end

local function lock_balance()
    local total = 0
    for _, item in ipairs(inventory().items) do
        total = total + items.lock_value(item.id, name_of(item.id)) * item.count
    end
    return total
end

local function keep_reason(item)
    local info = gtproxy.items.get(item.id)
    return items.keep_reason(item, keep_list, info and info.untradeable)
end

local function plan_drop_all()
    return items.plan_drop_all(inventory().items, keep_reason)
end

---------------------------------------------------------------------------
-- The drop queue
---------------------------------------------------------------------------

local queue = {}            -- { { item =, limit = } }, limit 0 = the whole stack
local current = nil         -- the job being dropped
local stage = nil           -- "dialog": request sent; "ack": answer sent
local timeout_timer = nil
local next_timer = nil
local next_send = 0         -- gtproxy.now() of the next request
local dropped, failed = 0, 0

local function busy()
    return current ~= nil or #queue > 0
end

local function remaining()
    return #queue + (current and 1 or 0)
end

local function stop()
    queue = {}
    current, stage = nil, nil
    gtproxy.cancel(timeout_timer)
    gtproxy.cancel(next_timer)
    timeout_timer, next_timer = nil, nil
end

local send_next

local function wait(ms, on_timeout)
    gtproxy.cancel(timeout_timer)
    timeout_timer = gtproxy.after(ms, function()
        timeout_timer = nil
        on_timeout()
    end)
end

local function schedule_next()
    gtproxy.cancel(next_timer)
    next_timer = gtproxy.after(math.max(10, next_send - gtproxy.now()), function()
        next_timer = nil
        if not current then
            send_next()
        end
    end)
end

local function finish_current(ok)
    if ok then dropped = dropped + 1 else failed = failed + 1 end
    current, stage = nil, nil
    gtproxy.cancel(timeout_timer)
    timeout_timer = nil
    next_send = gtproxy.now() + gap
    if #queue == 0 then
        send_next()     -- prints the summary
    else
        schedule_next()
    end
end

local function on_timeout()
    if not current then
        return
    end
    if stage == "dialog" then
        gtproxy.log.warn(string.format("Drop: the server did not offer to drop %s - skipped", name_of(current.item)))
        finish_current(false)
    else
        -- The answer was sent; the server just did not confirm in time.
        finish_current(true)
    end
end

send_next = function()
    local inv = inventory()
    while #queue > 0 do
        local job = table.remove(queue, 1)
        if not inv.known or gtproxy.inventory.count(job.item) > 0 then
            if not gtproxy.server.text(items.drop_request(job.item)) then
                message("`4Drop stopped: ``not connected to the game server.")
                stop()
                return
            end
            gtproxy.log.info(string.format("Drop: asking the server to drop %s (%d)", name_of(job.item), job.item))
            current, stage = job, "dialog"
            wait(gtproxy.setting("dialog_timeout_ms"), on_timeout)
            return
        end
        -- already gone
    end

    message(string.format(
        "`oDrop finished: `w%d`` stack(s) dropped%s.",
        dropped,
        failed > 0 and string.format(", `4%d`` refused by the server", failed) or ""
    ))
end

-- Queues drops; returns how many were queued.
local function drop(jobs)
    if not busy() then
        dropped, failed = 0, 0
    end
    local queued = 0
    for _, job in ipairs(jobs) do
        if job.item ~= items.FIST and job.item ~= items.WRENCH then
            queue[#queue + 1] = job
            queued = queued + 1
        end
    end
    if queued > 0 and not current and not next_timer then
        schedule_next()
    end
    return queued
end

-- Some drops (e.g. a single item) happen without the "how many?" dialog.
gtproxy.on("inventory", function(e)
    if not e.full and current and current.item == e.item and e.removed > 0 then
        finish_current(true)
    end
end)

-- A new object of the item being dropped: the drop went through.
gtproxy.on("object", function(e)
    if e.change == -1 and current and e.item == current.item then
        finish_current(true)
    end
end)

gtproxy.on("world_menu", function()
    if busy() then
        message(string.format("`4Drop stopped: ``you left the world (%d left).", remaining()))
        stop()
    end
end)

gtproxy.on("disconnect", function()
    stop()
end)

-- The server's "how many?" dialog: answered here for a drop of the queue,
-- and with fast drop / fast trash for what you drop or trash yourself.
gtproxy.hook_call("OnDialogRequest", function(call)
    local markup = call.args[1]
    if type(markup) ~= "string" then
        return
    end

    local dialog = items.parse_item_dialog(markup)
    local waiting = current ~= nil and stage == "dialog"
    if not dialog then
        if waiting then
            -- Shows in the log what the server sent instead of the drop dialog.
            gtproxy.log.info(string.format("Drop: waiting for the drop dialog of %s, got another dialog: %s", name_of(current.item), markup:sub(1, 400)))
        end
        return
    end

    -- Our drop: the item matches and it is a drop dialog (any name with
    -- "drop" in it, in case the server renames it).
    local ours = waiting and dialog.item_id == current.item and dialog.dialog_name:find("drop", 1, true) ~= nil
    if waiting and not ours then
        gtproxy.log.info(string.format("Drop: waiting for the drop dialog of %s, got '%s' for item %d", name_of(current.item), dialog.dialog_name, dialog.item_id))
    end
    if ours then
        local amount = current.limit > 0 and math.min(current.limit, dialog.count) or dialog.count
        if amount == 0 then
            finish_current(false)
            return "cancel"
        end
        gtproxy.log.info(string.format("Drop: dropping %d of %s (%d owned)", amount, name_of(dialog.item_id), dialog.count))
        gtproxy.server.text(items.dialog_reply(dialog.dialog_name, dialog.item_id, amount))
        stage = "ack"
        wait(gtproxy.setting("ack_timeout_ms"), on_timeout)
        return "cancel"
    end

    -- Never for locks: those keep the normal "how many?" window.
    if dialog.dialog_name ~= "drop_item" and dialog.dialog_name ~= "trash_item" then
        return
    end
    local is_lock = items.lock_value(dialog.item_id, name_of(dialog.item_id)) > 0
    local fast = dialog.count > 0
        and ((dialog.dialog_name == "drop_item" and fast_drop)
            or (dialog.dialog_name == "trash_item" and fast_trash and not is_lock))
    if not fast then
        return
    end

    gtproxy.log.info(string.format("Fast %s: %s x%d", dialog.dialog_name == "drop_item" and "drop" or "trash", name_of(dialog.item_id), dialog.count))
    gtproxy.server.text(items.dialog_reply(dialog.dialog_name, dialog.item_id, dialog.count))
    return "cancel"
end)

---------------------------------------------------------------------------
-- Pages of /proxy
---------------------------------------------------------------------------

local function items_page()
    local d = gtproxy.dialog():header("Items", ICON_ITEMS)
    local inv = inventory()
    if inv.known then
        d:small(string.format("`oBackpack: `w%d`` items in `w%d`` slots", #inv.items, inv.slots))
        d:item(string.format("`oLocks: `w%s``", items.lock_text(lock_balance())), ICON_LOCK)
    else
        d:small("`oThe inventory arrives when you log in.")
    end
    if busy() then
        d:small(string.format("`4Dropping... %d item(s) left``", remaining()))
        d:button("drop_stop", "`4Stop dropping``")
    end

    d:spacer()
        :button("inventory", "Show inventory")
        :button("dropall", "Drop all except favourites...")
        :button("daw", "Drop all locks (WL, DL, BGL)")
        :input("dd_amount", "Drop locks worth (WL, e.g. 300 = 3 DL):", "", 9)
        :button("dd_amount_go", "Drop that amount")
        :spacer()
        :checkbox("fast_drop", "Fast drop: no \"how many?\", drop the whole stack", fast_drop)
        :checkbox("fast_trash", "Fast trash: same for trash (never for locks)", fast_trash)
        :input("drop_gap", "Pause between drops (ms, 100-2000):", tostring(gap), 4)
        :small(string.format("`oKeep list: items %sdropall never drops - %skeep add <item>", P, P))
        :spacer()
        :button("page_main", "<< Menu")
    return d:finish("gtproxy_items", "Apply")
end

local function inventory_page()
    local d = gtproxy.dialog():header("Inventory", ICON_ITEMS)
    local inv = inventory()
    if not inv.known then
        d:small("`oThe inventory arrives when you log in.")
        return d:spacer():button("page_items", "<< Items"):finish("gtproxy_inventory")
    end

    d:small(string.format("`o%d items in %d slots. `5fav`` = favourite/marked, `2worn`` = worn, `4keep`` = keep list", #inv.items, inv.slots))
    d:spacer()

    local MAX_SHOWN = 150
    for i, item in ipairs(inv.items) do
        if i > MAX_SHOWN then
            d:small(string.format("`o... and %d more", #inv.items - MAX_SHOWN))
            break
        end
        local marks = ""
        if item.flags & ~items.FLAG_WORN ~= 0 then
            marks = marks .. string.format(" `5fav(0x%02X)``", item.flags)
        end
        if item.flags & items.FLAG_WORN ~= 0 then
            marks = marks .. " `2worn``"
        end
        if keep_list[item.id] then
            marks = marks .. " `4keep``"
        end
        d:item(string.format("`w%s`` x%d `o(ID %d)``%s", shown_name(item.id), item.count, item.id, marks), item.id)
    end

    return d:spacer():button("page_items", "<< Items"):finish("gtproxy_inventory")
end

local function drop_all_page()
    local d = gtproxy.dialog():header("Drop all", ICON_ITEMS)
    if not inventory().known then
        d:small("`oThe inventory arrives when you log in.")
        return d:spacer():button("page_items", "<< Items"):finish("gtproxy_dropall")
    end

    local plan = plan_drop_all()
    if #plan.drop == 0 then
        d:small("`oNothing to drop: everything is kept.")
        return d:spacer():button("page_items", "<< Items"):finish("gtproxy_dropall")
    end

    local per_item = gap + 250
    d:text(string.format("`wDrops %d stack(s)`` where you stand, about %d s.", #plan.drop, (#plan.drop * per_item + 999) // 1000))

    local MAX_SHOWN = 40
    for i, item in ipairs(plan.drop) do
        if i > MAX_SHOWN then
            break
        end
        d:item(string.format("`w%s`` x%d", shown_name(item.id), item.count), item.id)
    end
    if #plan.drop > MAX_SHOWN then
        d:small(string.format("`o... and %d more", #plan.drop - MAX_SHOWN))
    end

    if #plan.keep > 0 then
        d:spacer():text(string.format("`2Kept (%d)``", #plan.keep))
        local kept = {}
        for _, entry in ipairs(plan.keep) do
            if entry.reason ~= "fist/wrench" then
                kept[#kept + 1] = string.format("%s (%s)", shown_name(entry.item.id), entry.reason)
            end
        end
        if #kept > 0 then
            d:small("`o" .. table.concat(kept, ", "))
        end
    end

    return d:spacer()
        :button("dropall_go", string.format("`4Drop %d stack(s) now``", #plan.drop))
        :button("page_items", "<< Items")
        :finish("gtproxy_dropall")
end

local function show(page)
    gtproxy.game.dialog(page())
end

---------------------------------------------------------------------------
-- Commands
---------------------------------------------------------------------------

local function join(args, from)
    return table.concat(args, " ", from or 1)
end

-- Item by ID or (part of) its name; explains when it is unclear.
local function resolve(query, reply)
    local id = items.number(query, 0xFFFF)
    if id then
        return id
    end

    local found = gtproxy.items.find(query, 8)
    if #found == 0 then
        reply(string.format("`4Oops: ``No item called '%s'.", sanitize(query)))
        return nil
    end

    local wanted = string.lower(query)
    for _, info in ipairs(found) do
        if #items.strip_colors(info.name) == #query and string.lower(info.name) == wanted then
            return info.id
        end
    end
    if #found == 1 then
        return found[1].id
    end

    local names = {}
    for i, info in ipairs(found) do
        names[i] = string.format("`w%s`` (%d)", info.name, info.id)
    end
    reply("`oWhich one? " .. table.concat(names, ", "))
    return nil
end

local function inventory_ready(reply)
    if not inventory().known then
        reply("`4Oops: ``The proxy has not seen your inventory yet - log in again with the proxy running (see the log).")
        return false
    end
    return true
end

local function start(jobs, reply)
    if not gtproxy.world.info().in_world then
        reply("`4Oops: ``Enter a world first.")
        return false
    end
    if not inventory().known then
        reply("`4Oops: ``The inventory is not known yet - log in again with the proxy running.")
        return false
    end
    local queued = drop(jobs)
    if queued == 0 then
        reply("`oNothing to drop.")
        return false
    end
    reply(string.format("`oDropping `w%d`` stack(s)... (`w%sdropall stop`` to stop)", queued, P))
    return true
end

local function cmd_inventory()
    show(inventory_page)
end

local function cmd_dropall(args, reply)
    local sub = args[1] or ""
    if sub == "stop" then
        local left = remaining()
        stop()
        reply(string.format("`oDrop stopped (%d left).", left))
        return
    end
    if sub == "speed" then
        local ms = #args == 2 and math.tointeger(tonumber(args[2])) or nil
        if not ms then
            reply(string.format("`oPause between drops: `w%d ms``. Change: %sdropall speed <100-2000>", gap, P))
            return
        end
        gap = clamp(ms, MIN_GAP, MAX_GAP)
        save()
        reply(string.format("`oPause between drops set to `w%d ms``.", gap))
        return
    end
    if not inventory().known then
        reply("`4Oops: ``The inventory is not known yet - log in again with the proxy running.")
        return
    end
    if sub == "now" or sub == "yes" then
        local jobs = {}
        for _, item in ipairs(plan_drop_all().drop) do
            jobs[#jobs + 1] = { item = item.id, limit = 0 }
        end
        start(jobs, reply)
        return
    end

    -- Without "now": show what would be dropped and ask.
    show(drop_all_page)
end

-- /daw: every lock. /dd <amount>: locks worth that many World Locks, big
-- ones first (/dd 300 = 3 Diamond Locks); /dd alone = every lock.
local function cmd_locks(name, by_amount)
    return function(args, reply)
        if not inventory_ready(reply) then
            return
        end

        local have = locks()
        if #have == 0 then
            gtproxy.log.info(string.format("/%s: no locks among %d inventory items", name, #inventory().items))
            reply("`oYou have no locks.")
            return
        end

        local jobs = {}
        if not by_amount or #args == 0 then
            for _, lock in ipairs(have) do
                jobs[#jobs + 1] = { item = lock.id, limit = 0 }
            end
            start(jobs, reply)
            return
        end

        local amount = #args == 1 and items.number(args[1]) or nil
        if not amount or amount == 0 then
            reply(string.format("`4Usage: ``%sdd <World Locks> - e.g. %sdd 300 drops 3 Diamond Locks", P, P))
            return
        end

        local picked = items.pick_locks(have, amount)
        if not picked then
            reply(string.format(
                "`4Oops: ``Can't make exactly `w%d WL`` from your locks (you have %d WL in total). Break a bigger lock first.",
                amount,
                lock_balance()
            ))
            return
        end

        local plan = {}
        for _, pick in ipairs(picked) do
            jobs[#jobs + 1] = { item = pick[1], limit = pick[2] }
            plan[#plan + 1] = string.format("`w%d`` %s", pick[2], name_of(pick[1]))
        end
        reply(string.format("`o%d WL = %s", amount, table.concat(plan, " + ")))
        start(jobs, reply)
    end
end

-- /dw, /ddl, /db [amount]: one kind of lock.
local function cmd_lock(name, lock, id)
    return function(args, reply)
        local amount = 0
        if #args > 0 then
            amount = items.number(args[1], 0xFFFFFFFF)
            if not amount then
                reply(string.format("`4Usage: ``%s%s [amount]", P, name))
                return
            end
        end
        if not inventory_ready(reply) then
            return
        end
        if gtproxy.inventory.count(id) > 0 then
            start({ { item = id, limit = amount } }, reply)
            return
        end
        reply(string.format("`oYou have no %ss.", lock))
    end
end

local function cmd_drop(args, reply)
    if #args == 0 then
        reply(string.format("`4Usage: ``%sdrop <id or name> [amount]", P))
        return
    end

    -- A trailing number is the amount ("/drop dirt 50").
    local amount, name_end = 0, #args
    if #args > 1 then
        local last = items.number(args[#args], 0xFFFFFFFF)
        if last then
            amount, name_end = last, #args - 1
        end
    end

    local id = resolve(table.concat(args, " ", 1, name_end), reply)
    if not id then
        return
    end
    if inventory().known and gtproxy.inventory.count(id) == 0 then
        reply(string.format("`oYou have no %s.", name_of(id)))
        return
    end
    start({ { item = id, limit = amount } }, reply)
end

local function cmd_fastdrop(_, reply)
    fast_drop = not fast_drop
    save()
    reply(fast_drop and "`oFast drop `2on``." or "`oFast drop `4off``.")
end

local function cmd_fasttrash(_, reply)
    fast_trash = not fast_trash
    save()
    reply(fast_trash and "`oFast trash `2on`` - trashing destroys the whole stack at once." or "`oFast trash `4off``.")
end

local function cmd_keep(args, reply)
    if #args < 2 or (args[1] ~= "add" and args[1] ~= "del") then
        local ids = {}
        for id in pairs(keep_list) do
            ids[#ids + 1] = id
        end
        table.sort(ids)
        local list = {}
        for i, id in ipairs(ids) do
            list[i] = string.format("%s (%d)", name_of(id), id)
        end
        reply(string.format(
            "`oKeep list: `w%s``. %skeep add <item> / %skeep del <item>",
            #list == 0 and "empty" or table.concat(list, ", "),
            P,
            P
        ))
        return
    end

    local id = resolve(join(args, 2), reply)
    if not id then
        return
    end
    local add = args[1] == "add"
    keep_list[id] = add or nil
    save()
    reply(string.format("`w%s`` %s the keep list.", name_of(id), add and "is on" or "removed from"))
end

local function cmd_balance(_, reply)
    if not inventory().known then
        reply("`4Oops: ``The inventory is not known yet.")
        return
    end
    local parts = {}
    for _, lock in ipairs(locks()) do
        parts[#parts + 1] = string.format("`w%d`` %s", lock.count, name_of(lock.id))
    end
    reply(string.format("`oLocks: %s = `w%d WL``", #parts == 0 and "none" or table.concat(parts, ", "), lock_balance()))
end

local COMMANDS = {
    { "inv", "Show your inventory with item IDs and marks", cmd_inventory },
    { "dropall", "Drop everything except favourites/worn/keep list (dropall now | stop | speed <ms>)", cmd_dropall },
    { "daw", "Drop all locks (World, Diamond, Blue Gem, Black Gem)", cmd_locks("daw", false) },
    { "dd", "Drop locks worth N World Locks: dd 300 = 3 Diamond Locks (dd alone = all locks)", cmd_locks("dd", true) },
    { "dw", "Drop World Locks: dw [amount]", cmd_lock("dw", "World Lock", items.WORLD_LOCK) },
    { "ddl", "Drop Diamond Locks: ddl [amount]", cmd_lock("ddl", "Diamond Lock", items.DIAMOND_LOCK) },
    { "db", "Drop Blue Gem Locks: db [amount]", cmd_lock("db", "Blue Gem Lock", items.BLUE_GEM_LOCK) },
    { "drop", "Drop an item: drop <id or name> [amount]", cmd_drop },
    { "fastdrop", "Fast drop on/off: dropping skips \"how many?\" and drops the whole stack", cmd_fastdrop },
    { "fasttrash", "Fast trash on/off: trashing skips \"how many?\" (never for locks)", cmd_fasttrash },
    { "keep", "Items /dropall never drops: keep [add|del] <id or name>", cmd_keep },
    { "balance", "Your locks in World Locks", cmd_balance },
}

local by_name = {}
for _, command in ipairs(COMMANDS) do
    local name, description, run = command[1], command[2], command[3]
    by_name[name] = run
    gtproxy.command(name, description, function(args, ctx)
        run(args, ctx.reply)
    end)
end

-- A command from a page's button; answers go to the game.
local function run(line)
    local words = {}
    for word in string.gmatch(line, "%S+") do
        words[#words + 1] = word
    end
    local name = table.remove(words, 1)
    by_name[name](words, message)
end

---------------------------------------------------------------------------
-- The menu
---------------------------------------------------------------------------

gtproxy.menu.add({
    title = "Items",
    icon = ICON_ITEMS,
    button = "page_items",
    label = "Drop all, locks, fast drop",
    order = 20,
    status = function()
        local lines = {}
        local inv = inventory()
        if inv.known then
            lines[#lines + 1] = string.format("`oBackpack: `w%d/%d`` slots   Locks: `w%s``", #inv.items, inv.slots, items.lock_text(lock_balance()))
        end
        if busy() then
            lines[#lines + 1] = string.format("`4Dropping... %d item(s) left``", remaining())
        end
        return lines
    end,
})

gtproxy.on("dialog", function(e)
    -- The page's settings apply with any of its buttons (or Apply).
    if e.name == "gtproxy_items" then
        local on = e.values.fast_drop == "1"
        if on ~= fast_drop then
            fast_drop = on
            save()
        end
        on = e.values.fast_trash == "1"
        if on ~= fast_trash then
            fast_trash = on
            save()
        end
        local ms = math.tointeger(tonumber(sanitize(e.values.drop_gap or "")))
        if ms and ms ~= gap then
            gap = clamp(ms, MIN_GAP, MAX_GAP)
            save()
        end
    end
    if e.frame then
        return
    end

    local button = e.button
    local function with_text(command, field, missing)
        local text = sanitize(e.values[field] or "")
        if not text:find("%S") then
            message(missing)
            return
        end
        run(command .. " " .. text)
    end

    if button == "page_items" then show(items_page)
    elseif button == "inventory" then show(inventory_page)
    elseif button == "dropall" then show(drop_all_page)
    elseif button == "dropall_go" then run("dropall now")
    elseif button == "drop_stop" then run("dropall stop")
    elseif button == "daw" then run("daw")
    elseif button == "dd_amount_go" then with_text("dd", "dd_amount", "`4Oops: ``Type an amount in World Locks first.")
    elseif button == "" and e.name == "gtproxy_items" then message("`oSettings saved.")
    end
end)
