-- Example module (module API 1, see docs/modules.md): your world and tile.
-- Install: copy this folder to modules/ next to Proxy.exe and restart it.

gtproxy.command("whereami", "Your world and tile", function(args, ctx)
    local world = gtproxy.world.info()
    if not world.in_world or not world.me then
        ctx.reply("You are not in a world.")
        return
    end

    local text = string.format("%s: tile %d, %d", world.name, world.me.tile_x, world.me.tile_y)
    if gtproxy.setting("show_world_size") then
        text = text .. string.format(" (world %dx%d)", world.width, world.height)
    end
    ctx.reply(text)
end)
