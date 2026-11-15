-- Camera (module API 2, see docs/modules.md).
--
-- The game client limits how far the mouse wheel zooms out. The server can
-- move the camera with the call OnZoomCamera(zoom, time in ms); this module
-- sends that call to your game only (nothing reaches the server), so the
-- camera can show more of the world than the wheel allows.
--
--   /zoom              the value in use and how to change it
--   /zoom <value> [ms] zoom to that value (and keep it, see "keep")
--   /zoom reset        back to the normal value, stop keeping it
--
-- The game sets the camera again when you enter a world; with "keep" the
-- module sends your value again keep_delay_ms after that. Turning the mouse
-- wheel brings the game's own limits back until the next /zoom.
--
-- Settings (config/modules/camera.json): zoom (the value of /zoom and of
-- the page's Apply), normal (the game's normal zoom, sent by /zoom reset),
-- time_ms, keep, keep_delay_ms. Your last value is kept in data/camera.json.

local P = gtproxy.prefix
local ICON_CAMERA = 1608     -- Movie Camera
local sanitize = gtproxy.text.sanitize

local value = gtproxy.store.get("zoom", gtproxy.setting("zoom"))
local keeping = gtproxy.store.get("keeping", false) == true
local keep_timer = nil

local function number_text(n)
    if math.type(n) == "integer" or n == math.floor(n) then
        return string.format("%d", n)
    end
    return string.format("%g", n)
end

-- Sends OnZoomCamera(zoom, ms) to your game; false when not in a world.
local function send(zoom, ms)
    local here = gtproxy.world.info()
    if not here.in_world then
        return false
    end
    gtproxy.game.call("OnZoomCamera", -1, gtproxy.f32(zoom), gtproxy.i32(ms))
    gtproxy.log.info(string.format("OnZoomCamera(%s, %d) sent to the game", number_text(zoom), ms))
    return true
end

local function remember()
    gtproxy.store.set("zoom", value)
    gtproxy.store.set("keeping", keeping)
end

local function zoom_to(zoom, ms, reply)
    value = zoom
    keeping = gtproxy.setting("keep")
    remember()
    if not send(zoom, ms or gtproxy.setting("time_ms")) then
        reply(string.format("`oZoom `w%s`` saved; it is sent when you enter a world.", number_text(zoom)))
        return
    end
    reply(string.format("`oCamera zoom `w%s``%s. %szoom reset - back to normal.",
        number_text(zoom), keeping and " (kept when you enter a world)" or "", P))
end

local function reset(reply)
    keeping = false
    remember()
    send(gtproxy.setting("normal"), gtproxy.setting("time_ms"))
    reply(string.format("`oCamera back to normal (`w%s``).", number_text(gtproxy.setting("normal"))))
end

-- Entering a world resets the camera: send the kept value again.
gtproxy.on("world_enter", function()
    gtproxy.cancel(keep_timer)
    keep_timer = nil
    if keeping then
        keep_timer = gtproxy.after(gtproxy.setting("keep_delay_ms"), function()
            keep_timer = nil
            send(value, 0)
        end)
    end
end)

gtproxy.on("disconnect", function()
    gtproxy.cancel(keep_timer)
    keep_timer = nil
end)

local function cmd_zoom(args, reply)
    local first = args[1]
    if not first then
        reply(string.format(
            "`oCamera zoom: `w%s``%s. %szoom <value> [ms] - zoom; %szoom reset - normal (`w%s``).",
            number_text(value), keeping and " (kept)" or "", P, P, number_text(gtproxy.setting("normal"))
        ))
        return
    end
    if first == "reset" or first == "off" then
        reset(reply)
        return
    end

    local zoom = tonumber(first)
    local ms = args[2] and math.tointeger(tonumber(args[2])) or nil
    if not zoom or zoom ~= zoom or zoom <= 0 or zoom > 1000000 or (args[2] and (not ms or ms < 0 or ms > 60000)) then
        reply(string.format("`4Usage: ``%szoom <value> [ms] (a number above 0; ms 0-60000) or %szoom reset", P, P))
        return
    end
    zoom_to(zoom, ms, reply)
end

gtproxy.command("zoom", "Camera zoom beyond the mouse wheel: zoom <value> [ms] | zoom reset", function(args, ctx)
    cmd_zoom(args, ctx.reply)
end)

---------------------------------------------------------------------------
-- The Camera page of /proxy
---------------------------------------------------------------------------

local PRESETS = { "0.25", "0.5", "2", "4" }

local function camera_page()
    local d = gtproxy.dialog():header("Camera", ICON_CAMERA)
    d:small(string.format("`oZoom in use: `w%s``%s   Normal: `w%s``",
        number_text(value), keeping and " (kept)" or "", number_text(gtproxy.setting("normal"))))
    d:small("`oThe game's mouse wheel limits do not apply. Turning the wheel brings them back.")
    d:spacer()
        :input("zoom_value", "Zoom:", number_text(value), 12)
        :button("zoom_apply", "Zoom to this value")
    for _, preset in ipairs(PRESETS) do
        d:button("zoom_" .. preset:gsub("%.", "_"), "Zoom " .. preset)
    end
    return d:button("zoom_reset", "Back to normal")
        :spacer()
        :button("page_main", "<< Menu")
        :finish("gtproxy_camera")
end

gtproxy.menu.add({
    title = "Camera",
    icon = ICON_CAMERA,
    button = "page_camera",
    label = "Zoom out further than the game allows",
    order = 35,
})

local function message(text)
    gtproxy.game.message(text)
end

gtproxy.on("dialog", function(e)
    if e.frame then
        return
    end
    local button = e.button
    if button == "page_camera" then
        gtproxy.game.dialog(camera_page())
    elseif button == "zoom_apply" then
        cmd_zoom({ sanitize(e.values.zoom_value or "") }, message)
    elseif button == "zoom_reset" then
        reset(message)
    elseif button:match("^zoom_[%d_]+$") then
        cmd_zoom({ (button:sub(6):gsub("_", ".")) }, message)
    end
end)
