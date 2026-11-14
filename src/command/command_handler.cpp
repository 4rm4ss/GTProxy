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
#include "commands/item_commands.hpp"
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
    core::handlers::FeatureHandler& features,
    core::handlers::InventoryHandler& inventory
)
    : config_{ config }
    , dispatcher_{ dispatcher }
    , scheduler_{ std::move(scheduler) }
    , server_{ server }
    , client_{ client }
    , features_{ features }
    , inventory_{ inventory }
    , menu_{ features, inventory, registry_, [this](const std::string& line) { return run(line); } }
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
    registry_.add(std::make_unique<ProxyMenuCommand>(features_, menu_));
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
    registry_.add(std::make_unique<InventoryCommand>(features_, inventory_, menu_));
    registry_.add(std::make_unique<DropAllCommand>(features_, inventory_, menu_));
    registry_.add(std::make_unique<DropLocksCommand>(features_, inventory_));
    registry_.add(std::make_unique<DropLockCommand>(features_, inventory_, "dw", "World Lock"));
    registry_.add(std::make_unique<DropLockCommand>(features_, inventory_, "ddl", "Diamond Lock"));
    registry_.add(std::make_unique<DropLockCommand>(features_, inventory_, "db", "Blue Gem Lock"));
    registry_.add(std::make_unique<DropItemCommand>(features_, inventory_));
    registry_.add(std::make_unique<ToggleCommand>(features_, inventory_, ToggleCommand::What::FastDrop));
    registry_.add(std::make_unique<ToggleCommand>(features_, inventory_, ToggleCommand::What::FastTrash));
    registry_.add(std::make_unique<KeepCommand>(features_, inventory_));
    registry_.add(std::make_unique<BalanceCommand>(features_, inventory_));
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
    if (!dialog || !Menu::owns(dialog->dialog_name)) {
        return;
    }

    evt->cancel();
    menu_.handle(*dialog);
}
}
