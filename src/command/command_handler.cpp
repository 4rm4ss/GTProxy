#include "command_handler.hpp"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../packet/packet_helper.hpp"
#include "../packet/message/input.hpp"
#include "../packet/message/dialog.hpp"
#include "commands/debug_command.hpp"
#include "commands/exit_command.hpp"
#include "commands/feature_commands.hpp"
#include "commands/help_command.hpp"
#include "commands/skin_command.hpp"

namespace command {
namespace {
    constexpr auto input_event_type = event::packet_event_type(packet::PacketId::Input);
    constexpr auto dialog_return_event_type = event::packet_event_type(packet::PacketId::DialogReturn);
}

CommandHandler::CommandHandler(
    core::Config& config,
    event::Dispatcher& dispatcher,
    std::shared_ptr<core::Scheduler> scheduler,
    network::Server& server,
    network::Client& client,
    core::handlers::FeatureHandler& features
)
    : config_{ config }
    , dispatcher_{ dispatcher }
    , scheduler_{ std::move(scheduler) }
    , server_{ server }
    , client_{ client }
    , features_{ features }
{
    registry_.set_prefix(config_.get_command_config().prefix);
    register_default_commands();

    listener_handle_ = dispatcher_.prepend_listener(
        input_event_type,
        [this](const event::Event& e) { on_text_packet(e); }
    );

    dialog_listener_handle_ = dispatcher_.prepend_listener(
        dialog_return_event_type,
        [this](const event::Event& e) { on_dialog_return(e); }
    );

    spdlog::info("Command handler initialized with prefix '{}'", registry_.prefix());
}

CommandHandler::~CommandHandler()
{
    dispatcher_.remove_listener(input_event_type, listener_handle_);
    dispatcher_.remove_listener(dialog_return_event_type, dialog_listener_handle_);
}

void CommandHandler::register_default_commands()
{
    registry_.add(std::make_unique<ProxyMenuCommand>(features_));
    registry_.add(std::make_unique<WarpCommand>(features_));
    registry_.add(std::make_unique<BackCommand>(features_));
    registry_.add(std::make_unique<RejoinCommand>(features_));
    registry_.add(std::make_unique<ExitCommand>());
    registry_.add(std::make_unique<NickCommand>(features_));
    registry_.add(std::make_unique<NetIdCommand>(features_));
    registry_.add(std::make_unique<PosCommand>(features_));
    registry_.add(std::make_unique<PlayersCommand>(features_));
    registry_.add(std::make_unique<WorldInfoCommand>(features_));
    registry_.add(std::make_unique<FindItemCommand>(features_));
    registry_.add(std::make_unique<ItemInfoCommand>(features_));
    registry_.add(std::make_unique<SkinCommand>());
    registry_.add(std::make_unique<HelpCommand>());
    registry_.add(std::make_unique<DebugCommand>());
}

void CommandHandler::on_text_packet(const event::Event& e)
{
    const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::Input>*>(&e);
    if (!evt) {
        return;
    }

    const auto input_pkt{ evt->get<packet::message::Input>() };
    if (!input_pkt) {
        return;
    }

    const std::string& text = input_pkt->text;
    if (registry_.execute(text, server_, client_, dispatcher_, scheduler_)) {
        spdlog::info("Command handler executed successfully");
        evt->cancel();
    }
}
}

namespace command {
bool CommandHandler::run(const std::string& command_line)
{
    return registry_.execute(std::string{ registry_.prefix() } + command_line, server_, client_, dispatcher_, scheduler_);
}

// Buttons of the /proxy menu. The game sends them to the server like any
// dialog; the proxy answers them itself and never forwards them.
void CommandHandler::on_dialog_return(const event::Event& e)
{
    const auto* evt = dynamic_cast<const event::TypedPacketEvent<packet::PacketId::DialogReturn>*>(&e);
    if (!evt || evt->direction != event::Direction::ServerBound) {
        return;
    }

    const auto dialog{ evt->get<packet::message::DialogReturn>() };
    if (!dialog || dialog->dialog_name != ProxyMenuCommand::DIALOG_NAME) {
        return;
    }

    evt->cancel();

    if (const bool show{ dialog->checked("show_netid") }; show != features_.show_net_ids()) {
        features_.set_show_net_ids(show);
    }

    const std::string& button{ dialog->button_clicked };
    spdlog::info("Proxy menu: {}", button.empty() ? "apply" : button);

    const auto with_text = [&](const std::string& command, const std::string& field, const std::string& missing) {
        const std::string value{ core::features::sanitize(dialog->field(field)) };
        if (value.find_first_not_of(' ') == std::string::npos) {
            features_.message(missing);
            return;
        }
        run(command + " " + value);
    };

    if (button == "warp") {
        with_text("warp", "world", "`4Oops: ``Type a world name first.");
    } else if (button == "back") {
        run("back");
    } else if (button == "rejoin") {
        run("rejoin");
    } else if (button == "set_nick") {
        with_text("nick", "nick", "`4Oops: ``Type a nick first.");
    } else if (button == "reset_nick") {
        run("nick");
    } else if (button == "rainbow") {
        run("rainbow");
    } else if (button == "players") {
        run("players");
    } else if (button == "world_info") {
        run("world");
    } else if (button == "find_item") {
        with_text("find", "find", "`4Oops: ``Type part of an item name first.");
    } else if (button == "commands") {
        run("proxyhelp");
    }
}
}
