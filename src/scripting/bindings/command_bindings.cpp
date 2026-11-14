#include "command_bindings.hpp"

#include <string>
#include <vector>

#include "../../packet/message/chat.hpp"

namespace scripting::bindings {
void CommandBindings::bind(sol::state& lua)
{
    auto cmd_table{ lua.create_table() };

    auto register_impl = [this, &lua](
        const std::string& name,
        const std::string& description,
        sol::protected_function callback
    ) {
        handler_.registry().add(name, description, [callback, &lua, name](const command::Context& ctx) -> command::Result {
            sol::table args_table{ lua.create_table() };
            for (size_t i{ 0 }; i < ctx.args.size(); ++i) {
                args_table[i + 1] = ctx.args[i];
            }

            sol::table lua_ctx{ lua.create_table() };
            lua_ctx["args"] = args_table;
            lua_ctx["raw"] = ctx.raw_input;
            // Supports both ctx:reply(msg, ...) and ctx.reply(msg, ...). Extra
            // arguments replace "{}" placeholders in order. Captures the server
            // (which outlives the command), so a stored ctx stays safe to use.
            lua_ctx["reply"] = [&server = ctx.server](sol::this_state ts, sol::variadic_args va) -> bool {
                sol::state_view state{ ts };
                const sol::protected_function to_string{ state["tostring"] };
                const auto as_string = [&to_string](const sol::object& value) -> std::string {
                    if (value.is<std::string>()) {
                        return value.as<std::string>();
                    }

                    const sol::protected_function_result converted{ to_string(value) };
                    return converted.valid() ? converted.get<std::string>() : std::string{};
                };

                std::vector<sol::object> values(va.begin(), va.end());
                std::size_t index{ 0 };
                if (index < values.size() && values[index].get_type() == sol::type::table) {
                    ++index; // method call: first argument is ctx itself
                }

                if (index >= values.size()) {
                    return false;
                }

                std::string msg{ as_string(values[index++]) };
                for (; index < values.size(); ++index) {
                    const auto value{ as_string(values[index]) };
                    if (const auto pos{ msg.find("{}") }; pos != std::string::npos) {
                        msg.replace(pos, 2, value);
                    } else {
                        msg += ' ';
                        msg += value;
                    }
                }

                auto log_pkt = std::make_shared<packet::message::Log>();
                log_pkt->msg = msg;
                return packet::PacketHelper::write(*log_pkt, server);
            };

            try {
                const sol::protected_function_result result{ callback(lua_ctx) };

                if (!result.valid()) {
                    const sol::error err = result;
                    spdlog::error("[Lua Command] Error in '{}': {}", name, err.what());
                    return command::Result::Failed;
                }

                if (result.get_type() == sol::type::boolean) {
                    return result.get<bool>() ? command::Result::Success : command::Result::Failed;
                }
            }
            catch (const std::exception& e) {
                spdlog::error("[Lua Command] Exception in '{}': {}", name, e.what());
                return command::Result::Failed;
            }
            catch (...) {
                spdlog::error("[Lua Command] Unknown exception in '{}'", name);
                return command::Result::Failed;
            }

            return command::Result::Success;
        });

        spdlog::info("[Lua] Registered command: /{}", name);
        return true;
    };

    cmd_table.set_function("register", sol::overload(
        register_impl,
        [register_impl](const std::string& name, sol::protected_function callback) {
            return register_impl(name, "Script command", callback);
        }
    ));

    cmd_table.set_function("list", [this, &lua]() {
        sol::table commands{ lua.create_table() };

        const auto& registry{ handler_.registry() };

        for (
            const auto command_list{ registry.get_all_commands() };
            const auto& [name, description] : command_list
        ) {
            commands[name] = description;
        }

        return commands;
    });

    cmd_table.set_function("execute", [this](const std::string& input) -> bool {
        return handler_.registry().execute(input, server_, client_, dispatcher_, scheduler_);
    });

    cmd_table.set_function("get_prefix", [this]() {
        return std::string{ 1, handler_.registry().prefix() };
    });

    cmd_table.set_function("set_prefix", [this](char prefix) {
        handler_.registry().set_prefix(prefix);
    });

    lua["command"] = cmd_table;
}
}
