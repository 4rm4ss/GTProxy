#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <vector>

#include "../inventory.hpp"
#include "../../event/event.hpp"
#include "session_handler.hpp"

namespace core::handlers {
// Your inventory as the server reports it: the full list when you log in,
// then every change (items used, dropped, picked up). It only keeps count;
// the item tools (drop, locks ...) are modules/drop.
class InventoryHandler {
public:
    // What changed: the whole inventory arrived, or one item went up/down.
    struct Change {
        bool full{ false };
        std::uint16_t item_id{ 0 };
        int removed{ 0 };
        int added{ 0 };
    };

    InventoryHandler(event::Dispatcher& dispatcher, SessionHandler& session);

    InventoryHandler(const InventoryHandler&) = delete;
    InventoryHandler& operator=(const InventoryHandler&) = delete;
    ~InventoryHandler() = default;

    [[nodiscard]] bool known() const { return known_; }
    [[nodiscard]] std::uint32_t slots() const { return slots_; }
    [[nodiscard]] const std::map<std::uint16_t, inventory::Item>& items() const { return items_; }
    [[nodiscard]] std::uint32_t count_of(std::uint16_t item_id) const;

    // Called after every change (modules hear about it from here).
    void set_on_change(std::function<void(const Change&)> on_change) { on_change_ = std::move(on_change); }

private:
    void on_inventory(const event::Event& event);
    void on_modify(const event::Event& event);
    void on_object(const event::Event& event);

    event::Dispatcher& dispatcher_;
    SessionHandler& session_;
    std::vector<event::ScopedHandle> handles_;
    std::function<void(const Change&)> on_change_;

    bool known_{ false };
    std::uint32_t slots_{ 0 };
    std::map<std::uint16_t, inventory::Item> items_;
};
}
