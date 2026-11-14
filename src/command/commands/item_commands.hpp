#pragma once
#include <charconv>
#include <optional>
#include <string>
#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../command.hpp"
#include "../command_registry.hpp"
#include "../menu.hpp"
#include "../../core/features.hpp"
#include "../../core/inventory.hpp"
#include "../../core/handlers/feature_handler.hpp"
#include "../../core/handlers/inventory_handler.hpp"
#include "../../item/item_database.hpp"

// Item tools. Drops are the same request + "how many?" answer the game sends
// when you press Drop, one stack at a time.
namespace command {
class ItemCommand : public ICommand {
public:
    ItemCommand(core::handlers::FeatureHandler& features, core::handlers::InventoryHandler& inventory)
        : features_{ features }
        , inventory_{ inventory }
    { }

protected:
    void reply(const std::string& text) const { features_.message(text); }

    [[nodiscard]] static std::string join(const std::vector<std::string>& args, const std::size_t from)
    {
        std::string out;
        for (std::size_t i{ from }; i < args.size(); ++i) {
            out += (out.empty() ? "" : " ") + args[i];
        }
        return out;
    }

    // Item by ID or (part of) its name; explains in chat when it is unclear.
    [[nodiscard]] std::optional<std::uint16_t> resolve(const std::string& query) const
    {
        std::uint16_t id{};
        if (std::from_chars(query.data(), query.data() + query.size(), id).ec == std::errc{}) {
            return id;
        }

        const auto found{ item::ItemDatabase::instance().find(query, 8) };
        if (found.empty()) {
            reply(fmt::format("`4Oops: ``No item called '{}'.", core::features::sanitize(query)));
            return std::nullopt;
        }

        const auto exact{ std::ranges::find_if(found, [&](const item::ItemInfo* info) {
            return core::features::strip_colors(info->item_name).size() == query.size()
                && std::ranges::equal(info->item_name, query, [](const char a, const char b) { return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b)); });
        }) };
        if (exact != found.end()) {
            return static_cast<std::uint16_t>((*exact)->item_id);
        }
        if (found.size() == 1) {
            return static_cast<std::uint16_t>(found.front()->item_id);
        }

        std::string names;
        for (const auto* info : found) {
            names += fmt::format("{}`w{}`` ({})", names.empty() ? "" : ", ", info->item_name, info->item_id);
        }
        reply(fmt::format("`oWhich one? {}", names));
        return std::nullopt;
    }

    [[nodiscard]] static std::string name_of(const std::uint16_t id)
    {
        if (const auto* info{ item::ItemDatabase::instance().get_item(id) }) {
            return info->item_name;
        }
        if (const auto known{ core::inventory::known_name(id) }; !known.empty()) {
            return std::string{ known };
        }
        return fmt::format("item {}", id);
    }

    // Locks you carry (by ID, or by name for newer locks), most valuable first.
    [[nodiscard]] std::vector<std::pair<std::uint16_t, std::uint64_t>> locks() const
    {
        std::vector<std::pair<std::uint16_t, std::uint64_t>> out;
        for (const auto& [id, item] : inventory_.items()) {
            if (const auto value{ core::inventory::lock_value(id, name_of(id)) }; value > 0 && item.count > 0) {
                out.emplace_back(id, value);
            }
        }
        std::ranges::sort(out, [](const auto& a, const auto& b) { return a.second > b.second; });
        return out;
    }

    // Says why there is nothing to work with.
    [[nodiscard]] bool inventory_ready() const
    {
        if (!inventory_.known()) {
            reply("`4Oops: ``The proxy has not seen your inventory yet - log in again with the proxy running (see the log).");
            return false;
        }
        return true;
    }

    bool start(const std::vector<core::handlers::InventoryHandler::Job>& jobs, const char prefix = '/') const
    {
        if (!features_.in_world()) {
            reply("`4Oops: ``Enter a world first.");
            return false;
        }
        if (!inventory_.known()) {
            reply("`4Oops: ``The inventory is not known yet - log in again with the proxy running.");
            return false;
        }
        const auto queued{ inventory_.drop(jobs) };
        if (queued == 0) {
            reply("`oNothing to drop.");
            return false;
        }
        reply(fmt::format("`oDropping `w{}`` stack(s)... (`w{}dropall stop`` to stop)", queued, prefix));
        return true;
    }

    core::handlers::FeatureHandler& features_;
    core::handlers::InventoryHandler& inventory_;
};

class InventoryCommand final : public ItemCommand {
public:
    InventoryCommand(core::handlers::FeatureHandler& f, core::handlers::InventoryHandler& i, Menu& menu)
        : ItemCommand{ f, i }, menu_{ menu } { }

    [[nodiscard]] std::string_view name() const override { return "inv"; }
    [[nodiscard]] std::string description() const override { return "Show your inventory with item IDs and marks"; }

    Result execute(const Context&) override
    {
        menu_.show(Menu::Page::Inventory);
        return Result::Success;
    }

private:
    Menu& menu_;
};

class DropAllCommand final : public ItemCommand {
public:
    DropAllCommand(core::handlers::FeatureHandler& f, core::handlers::InventoryHandler& i, Menu& menu)
        : ItemCommand{ f, i }, menu_{ menu } { }

    [[nodiscard]] std::string_view name() const override { return "dropall"; }
    [[nodiscard]] std::string description() const override { return "Drop everything except favourites/worn/keep list (dropall now | stop | speed <ms>)"; }

    Result execute(const Context& ctx) override
    {
        const std::string sub{ ctx.args.empty() ? "" : ctx.args[0] };
        if (sub == "stop") {
            const auto left{ inventory_.remaining() };
            inventory_.stop();
            reply(fmt::format("`oDrop stopped ({} left).", left));
            return Result::Success;
        }
        if (sub == "speed") {
            int ms{};
            if (ctx.args.size() != 2 || std::from_chars(ctx.args[1].data(), ctx.args[1].data() + ctx.args[1].size(), ms).ec != std::errc{}) {
                reply(fmt::format("`oPause between drops: `w{} ms``. Change: {}dropall speed <100-2000>", inventory_.drop_gap().count(), ctx.registry.prefix()));
                return Result::InvalidArguments;
            }
            inventory_.set_drop_gap(std::chrono::milliseconds{ ms });
            reply(fmt::format("`oPause between drops set to `w{} ms``.", inventory_.drop_gap().count()));
            return Result::Success;
        }
        if (!inventory_.known()) {
            reply("`4Oops: ``The inventory is not known yet - log in again with the proxy running.");
            return Result::Failed;
        }
        if (sub == "now" || sub == "yes") {
            return start(menu_.plan_drop_all().drop, ctx.registry.prefix()) ? Result::Success : Result::Failed;
        }

        // Without "now": show what would be dropped and ask.
        menu_.show(Menu::Page::DropAll);
        return Result::Success;
    }

private:
    Menu& menu_;
};

// /dd: every World Lock, Diamond Lock, Blue Gem Lock and Black Gem Lock.
class DropLocksCommand final : public ItemCommand {
public:
    using ItemCommand::ItemCommand;

    [[nodiscard]] std::string_view name() const override { return "dd"; }
    [[nodiscard]] std::string description() const override { return "Drop all locks (World, Diamond, Blue Gem, Black Gem)"; }

    Result execute(const Context& ctx) override
    {
        if (!inventory_ready()) {
            return Result::Failed;
        }

        std::vector<core::handlers::InventoryHandler::Job> jobs;
        for (const auto& [id, value] : locks()) {
            jobs.push_back({ id, 0 });
        }
        if (jobs.empty()) {
            spdlog::info("/dd: no locks among {} inventory items", inventory_.items().size());
            reply("`oYou have no locks.");
            return Result::Failed;
        }
        return start(jobs, ctx.registry.prefix()) ? Result::Success : Result::Failed;
    }
};

// /dw, /ddl, /db [amount]: one kind of lock.
class DropLockCommand final : public ItemCommand {
public:
    DropLockCommand(core::handlers::FeatureHandler& f, core::handlers::InventoryHandler& i, std::string name, std::string lock, const std::uint16_t lock_id)
        : ItemCommand{ f, i }, name_{ std::move(name) }, lock_{ std::move(lock) }, lock_id_{ lock_id } { }

    [[nodiscard]] std::string_view name() const override { return name_; }
    [[nodiscard]] std::string description() const override { return fmt::format("Drop {}s: {} [amount]", lock_, name_); }

    Result execute(const Context& ctx) override
    {
        std::uint32_t amount{ 0 };
        if (!ctx.args.empty() && std::from_chars(ctx.args[0].data(), ctx.args[0].data() + ctx.args[0].size(), amount).ec != std::errc{}) {
            reply(fmt::format("`4Usage: ``{}{} [amount]", ctx.registry.prefix(), name_));
            return Result::InvalidArguments;
        }

        if (!inventory_ready()) {
            return Result::Failed;
        }
        if (inventory_.count_of(lock_id_) > 0) {
            return start({ { lock_id_, amount } }, ctx.registry.prefix()) ? Result::Success : Result::Failed;
        }
        reply(fmt::format("`oYou have no {}s.", lock_));
        return Result::Failed;
    }

private:
    std::string name_;
    std::string lock_;
    std::uint16_t lock_id_;
};

class DropItemCommand final : public ItemCommand {
public:
    using ItemCommand::ItemCommand;

    [[nodiscard]] std::string_view name() const override { return "drop"; }
    [[nodiscard]] std::string description() const override { return "Drop an item: drop <id or name> [amount]"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.empty()) {
            reply(fmt::format("`4Usage: ``{}drop <id or name> [amount]", ctx.registry.prefix()));
            return Result::InvalidArguments;
        }

        // A trailing number is the amount ("/drop dirt 50").
        std::uint32_t amount{ 0 };
        std::size_t name_end{ ctx.args.size() };
        if (ctx.args.size() > 1) {
            const auto& last{ ctx.args.back() };
            if (std::from_chars(last.data(), last.data() + last.size(), amount).ec == std::errc{}) {
                name_end = ctx.args.size() - 1;
            }
        }

        std::vector<std::string> words{ ctx.args.begin(), ctx.args.begin() + static_cast<std::ptrdiff_t>(name_end) };
        const auto id{ resolve(join(words, 0)) };
        if (!id) {
            return Result::InvalidArguments;
        }
        if (inventory_.known() && inventory_.count_of(*id) == 0) {
            reply(fmt::format("`oYou have no {}.", name_of(*id)));
            return Result::Failed;
        }
        return start({ { *id, amount } }, ctx.registry.prefix()) ? Result::Success : Result::Failed;
    }
};

class ToggleCommand final : public ItemCommand {
public:
    enum class What { FastDrop, FastTrash };

    ToggleCommand(core::handlers::FeatureHandler& f, core::handlers::InventoryHandler& i, What what)
        : ItemCommand{ f, i }, what_{ what } { }

    [[nodiscard]] std::string_view name() const override { return what_ == What::FastDrop ? "fastdrop" : "fasttrash"; }
    [[nodiscard]] std::string description() const override
    {
        return what_ == What::FastDrop
            ? "Fast drop on/off: dropping skips \"how many?\" and drops the whole stack"
            : "Fast trash on/off: trashing skips \"how many?\" (never for locks)";
    }

    Result execute(const Context&) override
    {
        if (what_ == What::FastDrop) {
            inventory_.set_fast_drop(!inventory_.fast_drop());
            reply(inventory_.fast_drop() ? "`oFast drop `2on``." : "`oFast drop `4off``.");
        } else {
            inventory_.set_fast_trash(!inventory_.fast_trash());
            reply(inventory_.fast_trash() ? "`oFast trash `2on`` - trashing destroys the whole stack at once." : "`oFast trash `4off``.");
        }
        return Result::Success;
    }

private:
    What what_;
};

class KeepCommand final : public ItemCommand {
public:
    using ItemCommand::ItemCommand;

    [[nodiscard]] std::string_view name() const override { return "keep"; }
    [[nodiscard]] std::string description() const override { return "Items /dropall never drops: keep [add|del] <id or name>"; }

    Result execute(const Context& ctx) override
    {
        if (ctx.args.size() < 2 || (ctx.args[0] != "add" && ctx.args[0] != "del")) {
            std::string list;
            for (const auto id : inventory_.keep_list()) {
                list += fmt::format("{}{} ({})", list.empty() ? "" : ", ", name_of(id), id);
            }
            reply(fmt::format(
                "`oKeep list: `w{}``. {}keep add <item> / {}keep del <item>",
                list.empty() ? "empty" : list,
                ctx.registry.prefix(),
                ctx.registry.prefix()
            ));
            return Result::Success;
        }

        const auto id{ resolve(join(ctx.args, 1)) };
        if (!id) {
            return Result::InvalidArguments;
        }
        const bool add{ ctx.args[0] == "add" };
        inventory_.keep(*id, add);
        reply(fmt::format("`w{}`` {} the keep list.", name_of(*id), add ? "is on" : "removed from"));
        return Result::Success;
    }
};

class BalanceCommand final : public ItemCommand {
public:
    using ItemCommand::ItemCommand;

    [[nodiscard]] std::string_view name() const override { return "balance"; }
    [[nodiscard]] std::string description() const override { return "Your locks in World Locks"; }

    Result execute(const Context&) override
    {
        if (!inventory_.known()) {
            reply("`4Oops: ``The inventory is not known yet.");
            return Result::Failed;
        }

        std::string parts;
        for (const auto& [id, value] : locks()) {
            parts += fmt::format("{}`w{}`` {}", parts.empty() ? "" : ", ", inventory_.count_of(id), name_of(id));
        }
        reply(fmt::format("`oLocks: {} = `w{} WL``", parts.empty() ? "none" : parts, inventory_.lock_balance()));
        return Result::Success;
    }
};
}
