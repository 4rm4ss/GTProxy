#pragma once
#include <functional>
#include <string>
#include <string_view>

#include "../core/handlers/feature_handler.hpp"
#include "../core/handlers/inventory_handler.hpp"
#include "../packet/message/dialog.hpp"

namespace command {
class CommandRegistry;

// The /proxy menu: game dialogs (only you see them) with pages for worlds,
// items, looks and info. Buttons run the same code as the chat commands.
class Menu {
public:
    enum class Page { Main, Worlds, Items, Looks, Info, Inventory, DropAll, Commands };

    using Runner = std::function<bool(const std::string&)>;

    Menu(
        core::handlers::FeatureHandler& features,
        core::handlers::InventoryHandler& inventory,
        const CommandRegistry& registry,
        Runner run
    );

    void show(Page page) const;
    [[nodiscard]] std::string build(Page page) const;

    // True when the dialog belongs to the menu (it is then fully handled and
    // must not reach the game server).
    [[nodiscard]] static bool owns(std::string_view dialog_name);
    void handle(const packet::message::DialogReturn& dialog);

    // Items /dropall would drop and those it keeps (with the reason).
    struct DropPlan {
        std::vector<core::handlers::InventoryHandler::Job> drop;
        std::vector<std::pair<core::inventory::Item, core::handlers::InventoryHandler::KeepReason>> keep;
    };
    [[nodiscard]] DropPlan plan_drop_all() const;

private:
    [[nodiscard]] std::string main_page() const;
    [[nodiscard]] std::string worlds_page() const;
    [[nodiscard]] std::string items_page() const;
    [[nodiscard]] std::string looks_page() const;
    [[nodiscard]] std::string info_page() const;
    [[nodiscard]] std::string inventory_page() const;
    [[nodiscard]] std::string drop_all_page() const;
    [[nodiscard]] std::string commands_page() const;

    core::handlers::FeatureHandler& features_;
    core::handlers::InventoryHandler& inventory_;
    const CommandRegistry& registry_;
    Runner run_;
};
}
