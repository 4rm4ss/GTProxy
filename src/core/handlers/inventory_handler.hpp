#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../inventory.hpp"
#include "../../event/event.hpp"
#include "feature_handler.hpp"

namespace core::handlers {
// Your inventory as the server reports it, and the item tools built on it:
// dropping many items in a row (/dropall, /dd, /dw ...) and fast drop /
// fast trash. Every drop is the same request + "how many?" answer the game
// sends when you press Drop, one item at a time, each waiting for the
// server's reply so nothing is lost or duplicated.
class InventoryHandler {
public:
    struct Job {
        std::uint16_t item_id{ 0 };
        std::uint32_t limit{ 0 };   // 0 = the whole stack
    };

    enum class KeepReason { None, FistOrWrench, Untradeable, Favorite, Worn, KeepList };

    InventoryHandler(event::Dispatcher& dispatcher, FeatureHandler& features, std::filesystem::path settings_file);

    InventoryHandler(const InventoryHandler&) = delete;
    InventoryHandler& operator=(const InventoryHandler&) = delete;
    ~InventoryHandler() = default;

    [[nodiscard]] bool known() const { return known_; }
    [[nodiscard]] std::uint32_t slots() const { return slots_; }
    [[nodiscard]] const std::map<std::uint16_t, inventory::Item>& items() const { return items_; }
    [[nodiscard]] std::uint32_t count_of(std::uint16_t item_id) const;
    // Total value of World/Diamond/Blue Gem/Black Gem Locks in World Locks.
    [[nodiscard]] std::uint64_t lock_balance() const;

    [[nodiscard]] KeepReason keep_reason(const inventory::Item& item) const;
    [[nodiscard]] static std::string_view describe(KeepReason reason);

    // Queues drops; returns how many were queued.
    std::size_t drop(const std::vector<Job>& jobs);
    void stop();
    [[nodiscard]] bool busy() const { return current_.has_value() || !queue_.empty(); }
    [[nodiscard]] std::size_t remaining() const { return queue_.size() + (current_ ? 1 : 0); }

    [[nodiscard]] bool fast_drop() const { return fast_drop_; }
    [[nodiscard]] bool fast_trash() const { return fast_trash_; }
    [[nodiscard]] std::chrono::milliseconds drop_gap() const { return drop_gap_; }
    [[nodiscard]] const std::set<std::uint16_t>& keep_list() const { return keep_; }
    void set_fast_drop(bool on);
    void set_fast_trash(bool on);
    void set_drop_gap(std::chrono::milliseconds gap);
    bool keep(std::uint16_t item_id, bool keep);

    static constexpr std::chrono::milliseconds MIN_GAP{ 100 };
    static constexpr std::chrono::milliseconds MAX_GAP{ 2000 };

    // Called from the main loop.
    void update();

private:
    enum class Stage { WaitDialog, WaitAck };

    void on_inventory(const event::Event& event);
    void on_modify(const event::Event& event);
    void on_object(const event::Event& event);
    void on_dialog(const event::Event& event);
    void on_world_menu(const event::Event& event);
    void finish_current(bool dropped);
    void send_next();
    void load_settings();
    void save_settings() const;

    event::Dispatcher& dispatcher_;
    FeatureHandler& features_;
    std::filesystem::path settings_file_;
    std::vector<event::ScopedHandle> handles_;

    bool known_{ false };
    std::uint32_t slots_{ 0 };
    std::map<std::uint16_t, inventory::Item> items_;

    std::deque<Job> queue_;
    std::optional<Job> current_;
    Stage stage_{ Stage::WaitDialog };
    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point next_send_{};
    std::size_t dropped_{ 0 };
    std::size_t failed_{ 0 };

    bool fast_drop_{ false };
    bool fast_trash_{ false };
    std::chrono::milliseconds drop_gap_{ 200 };
    std::set<std::uint16_t> keep_;
};
}
