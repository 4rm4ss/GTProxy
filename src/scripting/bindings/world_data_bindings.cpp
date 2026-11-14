#include "world_data_bindings.hpp"
#include "field.hpp"

#include <glm/glm.hpp>
#include <sol/sol.hpp>

#include "../../world/object.hpp"
#include "../../world/object_map.hpp"
#include "../../world/tile.hpp"
#include "../../world/tile_extra.hpp"
#include "../../world/tile_map.hpp"

namespace scripting::bindings {
void WorldDataBindings::bind(sol::state& lua)
{
    bind_tile(lua);
    bind_tile_extra(lua);
    bind_object(lua);
    bind_tile_map(lua);
    bind_object_map(lua);
}

void WorldDataBindings::bind_tile(sol::state& lua)
{
    lua.new_usertype<world::Tile>("Tile",
        sol::no_constructor,
        "foreground", field<&world::Tile::foreground>(),
        "background", field<&world::Tile::background>(),
        "parent_tile", field<&world::Tile::parent_tile>(),
        "flag", field<&world::Tile::flag>(),
        "lock_parent_tile", field<&world::Tile::lock_parent_tile>(),
        "flag_to_string", field<&world::Tile::flag_to_string>(),
        "extra", sol::property([](world::Tile& t) -> world::tile_extra::TileExtra* {
            return &t.extra;
        })
    );
}

void WorldDataBindings::bind_tile_extra(sol::state& lua)
{
    lua.new_usertype<world::tile_extra::TileExtra>("TileExtra",
        sol::no_constructor,
        "get_type", field<&world::tile_extra::TileExtra::get_type>(),
        "has_value", field<&world::tile_extra::TileExtra::has_value>(),
        "get_as_door", [](world::tile_extra::TileExtra& te) -> world::tile_extra::Door* {
            return te.get_as<world::tile_extra::Door>();
        },
        "get_as_lock", [](world::tile_extra::TileExtra& te) -> world::tile_extra::Lock* {
            return te.get_as<world::tile_extra::Lock>();
        },
        "get_as_seed", [](world::tile_extra::TileExtra& te) -> world::tile_extra::Seed* {
            return te.get_as<world::tile_extra::Seed>();
        },
        "get_as_sign", [](world::tile_extra::TileExtra& te) -> world::tile_extra::Sign* {
            return te.get_as<world::tile_extra::Sign>();
        }
    );
}

void WorldDataBindings::bind_tile_extra_variants(sol::state& lua)
{
    lua.new_usertype<world::tile_extra::Door>("TileExtraDoor",
        sol::no_constructor,
        "label", field<&world::tile_extra::Door::label>(),
        "unk", field<&world::tile_extra::Door::unk>()
    );
    
    lua.new_usertype<world::tile_extra::Lock>("TileExtraLock",
        sol::no_constructor,
        "owner_id", field<&world::tile_extra::Lock::owner_id>(),
        "unk", field<&world::tile_extra::Lock::unk>(),
        "accesses", [](const world::tile_extra::Lock& lock, sol::this_state s) {
            sol::state_view lua{ s };
            sol::table accesses{ lua.create_table() };
            for (size_t i = 0; i < lock.accesses.size(); ++i) {
                accesses[i + 1] = lock.accesses[i];
            }
            return accesses;
        }
    );
    
    lua.new_usertype<world::tile_extra::Seed>("TileExtraSeed",
        sol::no_constructor,
        "growth_time", field<&world::tile_extra::Seed::growth_time>(),
        "fruit_count", field<&world::tile_extra::Seed::fruit_count>()
    );
    
    lua.new_usertype<world::tile_extra::Sign>("TileExtraSign",
        sol::no_constructor,
        "label", field<&world::tile_extra::Sign::label>(),
        "unk", field<&world::tile_extra::Sign::unk>()
    );
}

void WorldDataBindings::bind_object(sol::state& lua)
{
    lua.new_usertype<world::Object>("Object",
        sol::no_constructor,
        "item_id", field<&world::Object::item_id>(),
        "pos", sol::property([](const world::Object& o, sol::this_state s) {
            sol::state_view lua{ s };
            sol::table t = lua.create_table();
            t["x"] = o.pos.x;
            t["y"] = o.pos.y;
            return t;
        }),
        "amount", field<&world::Object::amount>(),
        "flags", field<&world::Object::flags>(),
        "object_id", field<&world::Object::object_id>()
    );
}

void WorldDataBindings::bind_tile_map(sol::state& lua)
{
    lua.new_usertype<WorldTileMap>("WorldTileMap",
        sol::no_constructor,
        "get_size", [](const WorldTileMap& tm, sol::this_state s) {
            sol::state_view lua{ s };
            sol::table t = lua.create_table();
            t["x"] = tm.get_size().x;
            t["y"] = tm.get_size().y;
            return t;
        },
        "get_tiles", [](const WorldTileMap& tm, sol::this_state s) {
            sol::state_view lua{ s };
            sol::table tiles{ lua.create_table() };
            const auto& tile_vec = tm.get_tiles();
            for (size_t i = 0; i < tile_vec.size(); ++i) {
                tiles[i + 1] = std::cref(tile_vec[i]);
            }
            return tiles;
        }
    );
}

void WorldDataBindings::bind_object_map(sol::state& lua)
{
    lua.new_usertype<WorldObjectMap>("WorldObjectMap",
        sol::no_constructor,
        "get_drop_id", field<&WorldObjectMap::get_drop_id>(),
        "get_objects", [](const WorldObjectMap& om, sol::this_state s) {
            sol::state_view lua{ s };
            sol::table objects{ lua.create_table() };
            const auto& obj_vec = om.get_objects();
            for (size_t i = 0; i < obj_vec.size(); ++i) {
                objects[i + 1] = std::cref(obj_vec[i]);
            }
            return objects;
        }
    );
}
}
