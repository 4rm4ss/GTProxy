-- Camera (module API 2, see docs/modules.md).
--
-- The server's camera call OnZoomCamera(zoom, ms), sent to your game only:
-- /zoom <value>, /zoom test (which values move the camera in your game) and
-- the Camera page of /proxy. Optionally the proxy takes the mouse wheel over
-- Growtopia's window and turns it into OnZoomCamera ("wheel"); that only
-- helps if your game obeys OnZoomCamera, so it is off by default and the
-- wheel stays the game's.
--
-- Settings (config/modules/camera.json, or the Camera page, which writes
-- the same file):
--   wheel          "off" (default: the game's wheel), "ctrl" / "shift" / "alt"
--                  (only while held), "always"
--   wheel_step     how much one notch zooms (1.15 = 15 %)
--   invert         the other way round, if the camera moves the wrong way
--   min_zoom, max_zoom   the range the wheel moves in
--   zoom, normal   the value of /zoom alone and of /zoom reset
--   time_ms        how long /zoom takes to get there
--   keep, keep_delay_ms  send the value again after entering a world (the
--                  game resets the camera then)

local P = gtproxy.prefix
local ICON_CAMERA = 1608     -- Movie Camera
local sanitize = gtproxy.text.sanitize

local value = gtproxy.store.get("zoom", gtproxy.setting("zoom"))
local keeping = gtproxy.store.get("keeping", false) == true
local keep_timer = nil
local wheel_available = false

local function number_text(n)
    if n == math.floor(n) and math.abs(n) < 1e15 then
        return string.format("%d", math.floor(n))
    end
    return string.format("%.4g", n)
end

local function message(text)
    gtproxy.game.message(text)
end

-- Sends OnZoomCamera(zoom, ms) to your game; false when not in a world.
-- own: addressed to your netID instead of -1 (some games only obey that).
local function send(zoom, ms, own)
    local info = gtproxy.world.info()
    if not info.in_world then
        return false
    end
    gtproxy.game.call("OnZoomCamera", own and info.net_id or -1, gtproxy.f32(zoom), gtproxy.i32(ms))
    return true
end

local function remember()
    gtproxy.store.set("zoom", value)
    gtproxy.store.set("keeping", keeping)
end

local function apply_settings()
    wheel_available = gtproxy.input.capture_wheel(gtproxy.setting("wheel"))
end

apply_settings()
gtproxy.on("settings", apply_settings)

-- One notch of the wheel: closer or further by wheel_step, sent at once.
gtproxy.on("wheel", function(e)
    local step = math.max(1.001, gtproxy.setting("wheel_step"))
    local turns = gtproxy.setting("invert") and -e.notches or e.notches
    value = value * step ^ turns
    value = math.max(gtproxy.setting("min_zoom"), math.min(gtproxy.setting("max_zoom"), value))
    keeping = gtproxy.setting("keep")
    remember()
    local sent = send(value, 0)
    gtproxy.log.debug(string.format("wheel %+d: zoom %s%s", e.notches, number_text(value), sent and "" or " (not in a world: sent when you enter one)"))
end)

local function zoom_to(zoom, ms, reply)
    value = zoom
    keeping = gtproxy.setting("keep")
    remember()
    if not send(zoom, ms or gtproxy.setting("time_ms")) then
        reply(string.format("`oZoom `w%s`` saved; it is sent when you enter a world.", number_text(zoom)))
        return
    end
    gtproxy.log.info(string.format("OnZoomCamera(%s) sent to the game", number_text(zoom)))
    reply(string.format("`oCamera zoom `w%s``%s. %szoom reset - back to normal.",
        number_text(zoom), keeping and " (kept when you enter a world)" or "", P))
end

local function reset(reply)
    keeping = false
    value = gtproxy.setting("normal")
    remember()
    send(value, gtproxy.setting("time_ms"))
    reply(string.format("`oCamera back to normal (`w%s``).", number_text(value)))
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

-- /zoom test: which values (if any) move the camera in your game. The
-- game may ignore OnZoomCamera; then the wheel must stay the game's.
local TEST_STEPS = {
    { 0.5, 300 }, { 2, 300 }, { 0.1, 1000 }, { 10, 1000 }, { 100, 1000 }, { 10000, 1000 },
    { 0.5, 300, true }, { 10, 1000, true }, { 10000, 1000, true }, { 1, 300 },
}
local test_timer = nil

local function zoom_test(reply)
    if not gtproxy.world.info().in_world then
        reply("`4Oops: ``Enter a world first.")
        return
    end
    gtproxy.cancel(test_timer)
    local step = 0
    local function next_step()
        step = step + 1
        local entry = TEST_STEPS[step]
        if not entry then
            test_timer = nil
            message(string.format("`oZoom test done. Tell me the step number where the camera moved. If it never moved, your game ignores OnZoomCamera: keep the wheel `woff`` (%szoom wheel off) - the zoom limit is inside the game client.", P))
            return
        end
        send(entry[1], entry[2], entry[3])
        message(string.format("`oZoom test `w%d/%d``: OnZoomCamera(`w%s``, %d ms)%s - did the camera move?", step, #TEST_STEPS,
            number_text(entry[1]), entry[2], entry[3] and " to your netID" or ""))
        test_timer = gtproxy.after(3000, next_step)
    end
    next_step()
end

local function cmd_zoom(args, reply)
    local first = args[1]
    if first == "test" then
        zoom_test(reply)
        return
    end
    if first == "wheel" and args[2] then
        local mode = args[2]
        if mode ~= "off" and mode ~= "always" and mode ~= "ctrl" and mode ~= "shift" and mode ~= "alt" then
            reply(string.format("`4Usage: ``%szoom wheel off|ctrl|always", P))
            return
        end
        local ok, why = gtproxy.save_setting("wheel", mode)
        apply_settings()
        reply(ok and string.format("`oMouse wheel: `w%s``%s.", mode, mode == "off" and " - the game's own zoom again" or "") or ("`4Camera: ``" .. why))
        return
    end
    if not first then
        reply(string.format(
            "`oCamera zoom: `w%s``%s. Mouse wheel over the game: `w%s``. %szoom <value> [ms]; %szoom reset (`w%s``).",
            number_text(value), keeping and " (kept)" or "",
            wheel_available and gtproxy.setting("wheel") or "not available here",
            P, P, number_text(gtproxy.setting("normal"))
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

gtproxy.command("zoom", "Camera: zoom <value> [ms] | zoom reset | zoom test | zoom wheel off|ctrl|always", function(args, ctx)
    cmd_zoom(args, ctx.reply)
end)

---------------------------------------------------------------------------
-- The Camera page of /proxy: the same settings as config/modules/camera.json
---------------------------------------------------------------------------

local WHEEL_MODES = {
    { "off", "Off: the game's own wheel (normal zoom)" },
    { "ctrl", "Test: Ctrl + wheel sends OnZoomCamera" },
    { "always", "Test: the wheel only sends OnZoomCamera" },
}

local function camera_page()
    local d = gtproxy.dialog():header("Camera", ICON_CAMERA)
    d:small(string.format("`oZoom now: `w%s``%s   Normal: `w%s``",
        number_text(value), keeping and " (kept)" or "", number_text(gtproxy.setting("normal"))))
    if not wheel_available then
        d:small("`4The mouse wheel can only be taken from the game on Windows.``")
    end
    d:small("`oSaved to config/modules/camera.json - the same file you can edit by hand.")
    d:spacer():small("`oMouse wheel over the game: `w" .. gtproxy.setting("wheel") .. "``")
    for _, mode in ipairs(WHEEL_MODES) do
        local on = mode[1] == gtproxy.setting("wheel")
        d:button("cam_wheel_" .. mode[1], (on and "`2> " or "") .. mode[2] .. (on and "``" or ""))
    end
    d:input("wheel_step", "One notch zooms by (1.01-3):", number_text(gtproxy.setting("wheel_step")), 6)
        :input("min_zoom", "Smallest zoom:", number_text(gtproxy.setting("min_zoom")), 10)
        :input("max_zoom", "Largest zoom:", number_text(gtproxy.setting("max_zoom")), 10)
        :checkbox("invert", "The other way round (if the wheel zooms the wrong way)", gtproxy.setting("invert"))
        :checkbox("keep", "Keep my zoom after entering a world", gtproxy.setting("keep"))
        :button("cam_save", "`2Save``")
        :spacer()
        :input("zoom_value", "Zoom to:", number_text(value), 12)
        :button("zoom_apply", "Zoom to this value")
        :button("zoom_reset", "Back to normal")
        :spacer()
        :small("`oThe game's own zoom limit is inside the game. The proxy can only ask the game to zoom, the way a server does (OnZoomCamera). The test shows whether your game obeys: watch the camera and note the step that moved it.")
        :button("zoom_test", "Zoom test (about 30 seconds)")
        :spacer()
        :button("page_main", "<< Menu")
    return d:finish("gtproxy_camera", "Save")
end

local function save(changes)
    local problems = {}
    for key, new in pairs(changes) do
        if new ~= gtproxy.setting(key) then
            local ok, why = gtproxy.save_setting(key, new)
            if not ok then
                problems[#problems + 1] = why
            end
        end
    end
    apply_settings()
    if #problems == 0 then
        message(string.format("`oCamera: wheel `w%s``, step `w%s``, zoom `w%s``-`w%s``%s (saved).",
            gtproxy.setting("wheel"), number_text(gtproxy.setting("wheel_step")),
            number_text(gtproxy.setting("min_zoom")), number_text(gtproxy.setting("max_zoom")),
            gtproxy.setting("invert") and ", inverted" or ""))
    else
        message("`4Camera: ``" .. table.concat(problems, "; "))
    end
end

gtproxy.menu.add({
    title = "Camera",
    icon = ICON_CAMERA,
    button = "page_camera",
    label = "Mouse wheel zoom without limits",
    order = 35,
})

gtproxy.on("dialog", function(e)
    local button = e.button
    if button == "page_camera" then
        gtproxy.game.dialog(camera_page())
        return
    end
    if e.name ~= "gtproxy_camera" or e.frame then
        return
    end

    local mode = button:match("^cam_wheel_(%a+)$")
    if mode then
        save({ wheel = mode })
        gtproxy.game.dialog(camera_page())
    elseif button == "zoom_apply" then
        cmd_zoom({ sanitize(e.values.zoom_value or "") }, message)
    elseif button == "zoom_reset" then
        reset(message)
    elseif button == "zoom_test" then
        zoom_test(message)
    elseif (button == "cam_save" or button == "") and e.values.wheel_step then
        local step = tonumber(sanitize(e.values.wheel_step or ""))
        local low = tonumber(sanitize(e.values.min_zoom or ""))
        local high = tonumber(sanitize(e.values.max_zoom or ""))
        local changes = { invert = e.values.invert == "1", keep = e.values.keep == "1" }
        if step and step == step then changes.wheel_step = math.max(1.01, math.min(3, step)) end
        if low and low > 0 then changes.min_zoom = low end
        if high and high > 0 then changes.max_zoom = high end
        if changes.min_zoom and changes.max_zoom and changes.min_zoom > changes.max_zoom then
            changes.min_zoom, changes.max_zoom = changes.max_zoom, changes.min_zoom
        end
        save(changes)
        if button == "cam_save" then
            gtproxy.game.dialog(camera_page())
        end
    end
end)
