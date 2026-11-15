-- Tests of modules/world_moderation/rules.lua (run like test_drop_items.lua).

local rules = gtproxy.load("rules.lua")

local function same(a, b, what)
    assert(a == b, string.format("%s: got %s, expected %s", what, tostring(a), tostring(b)))
end

do
    local list = assert(rules.parse_rules("world=MYWORLD action=kick name=Spammer1; world=* action=ban id=12345"))
    same(#list, 2, "two rules")
    same(list[1].world, "MYWORLD", "world"); same(list[1].action, "kick", "action"); same(list[1].name, "spammer1", "name, any case")
    same(list[2].world, "*", "every world"); same(list[2].id, 12345, "account id")
    same(#assert(rules.parse_rules("")), 0, "no rules")

    local bad, why = rules.parse_rules("world=X action=explode name=a")
    same(bad, nil, "bad action refused"); assert(why:find("rule 1", 1, true), why)
    same((rules.parse_rules("action=kick")), nil, "rule without name or id refused")
    same((rules.parse_rules("action=kick name=a id=3")), nil, "name and id together refused")
    same((rules.parse_rules("action=kick id=-4")), nil, "bad id refused")
    local _, why2 = rules.parse_rules("action=kick name=a colour=red")
    assert(why2 and why2:find("colour", 1, true), "unknown word named: " .. tostring(why2))

    local ex = assert(rules.parse_exceptions("name=MyFriend; id=777"))
    same(ex.names.myfriend, true, "exception by name"); same(ex.ids[777], true, "exception by id")
    same((rules.parse_exceptions("friend")), nil, "exception without name= or id= refused")

    -- Exact names only, the account ID wins over a new name, exceptions first.
    same(rules.match(list, ex, "myworld", "SPAMMER1", 5).action, "kick", "exact name, any case, this world")
    same(rules.match(list, ex, "OTHER", "Spammer1", 5), nil, "rule of another world")
    same(rules.match(list, ex, "myworld", "Spammer12", 5), nil, "not a part of a name")
    same(rules.match(list, ex, "ANY", "renamed", 12345).action, "ban", "by account ID in any world")
    local list2 = assert(rules.parse_rules("action=kick name=MyFriend"))
    same(rules.match(list2, ex, "W", "myfriend", 1), nil, "exception wins")
    same(rules.match(list, ex, "W", "x", 777), nil, "exception by id")
end

do
    local popup = gtproxy.ui.parse_dialog("embed_data|netID|20\nadd_button|pull|`5Pull``|noflags|0|0|\nadd_button|worldban|`4World Ban``|noflags|0|0|\nend_dialog|popup||Continue|\n")
    same(rules.button_for(popup, "pull"), "pull", "pull button")
    same(rules.button_for(popup, "ban"), "worldban", "ban is worldban")
    same(rules.button_for(popup, "kick"), nil, "no kick button: no rights")
    same(rules.is_refusal("You can't kick a moderator!"), true, "refusal")
    same(rules.is_refusal("Bob was pulled."), false, "not a refusal")
    same(rules.inside(3, 4, { x1 = 5, y1 = 6, x2 = 2, y2 = 3 }), true, "inside, corners any order")
    same(rules.inside(6, 4, { x1 = 2, y1 = 3, x2 = 5, y2 = 6 }), false, "outside")
end
