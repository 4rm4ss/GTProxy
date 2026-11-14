#include "menu.hpp"

#include <charconv>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "command_registry.hpp"
#include "../core/features.hpp"
#include "../item/item_database.hpp"
#include "../world/world.hpp"

namespace command {
namespace {
using core::features::sanitize;
using core::handlers::InventoryHandler;

// Item icons used as section headers.
constexpr int ICON_MENU{ 32 };       // Wrench
constexpr int ICON_WORLDS{ 12 };     // Door
constexpr int ICON_ITEMS{ 448 };     // Backpack
constexpr int ICON_LOCK{ 242 };      // World Lock
constexpr int ICON_LOOKS{ 1796 };    // Diamond Lock
constexpr int ICON_INFO{ 6016 };     // GrowScan 9000

constexpr std::string_view PREFIX{ "gtproxy_" };

std::string dialog_name(const Menu::Page page)
{
    switch (page) {
    case Menu::Page::Main: return "gtproxy_menu";
    case Menu::Page::Worlds: return "gtproxy_worlds";
    case Menu::Page::Items: return "gtproxy_items";
    case Menu::Page::Looks: return "gtproxy_looks";
    case Menu::Page::Info: return "gtproxy_info";
    case Menu::Page::Inventory: return "gtproxy_inventory";
    case Menu::Page::DropAll: return "gtproxy_dropall";
    }
    return "gtproxy_menu";
}

// Small builder for the game's dialog markup.
class Dialog {
public:
    Dialog& header(const std::string& title, const int icon)
    {
        text_ += "set_default_color|`o\n";
        text_ += fmt::format("add_label_with_icon|big|`w{}``|left|{}|\n", title, icon);
        return *this;
    }
    Dialog& section(const std::string& title, const int icon)
    {
        text_ += fmt::format("add_label_with_icon|small|`w{}``|left|{}|\n", title, icon);
        return *this;
    }
    Dialog& item(const std::string& text, const int icon)
    {
        text_ += fmt::format("add_label_with_icon|small|{}|left|{}|\n", text, icon);
        return *this;
    }
    Dialog& small(const std::string& text) { text_ += fmt::format("add_smalltext|{}|\n", text); return *this; }
    Dialog& text(const std::string& text) { text_ += fmt::format("add_textbox|{}|left|\n", text); return *this; }
    Dialog& spacer() { text_ += "add_spacer|small|\n"; return *this; }
    Dialog& button(const std::string& name, const std::string& label)
    {
        text_ += fmt::format("add_button|{}|{}|noflags|0|0|\n", name, label);
        return *this;
    }
    Dialog& checkbox(const std::string& name, const std::string& label, const bool on)
    {
        text_ += fmt::format("add_checkbox|{}|{}|{}|\n", name, label, on ? 1 : 0);
        return *this;
    }
    Dialog& input(const std::string& name, const std::string& label, const std::string& value, const int max)
    {
        text_ += fmt::format("add_text_input|{}|{}|{}|{}|\n", name, label, sanitize(value), max);
        return *this;
    }
    std::string end(const Menu::Page page, const std::string& ok = "")
    {
        text_ += "add_quick_exit|\n";
        text_ += fmt::format("end_dialog|{}|Close|{}|\n", dialog_name(page), ok);
        return std::move(text_);
    }

private:
    std::string text_;
};

std::string name_of(const std::uint16_t id)
{
    if (const auto* info{ item::ItemDatabase::instance().get_item(id) }) {
        return sanitize(info->item_name);
    }
    return fmt::format("Item {}", id);
}

std::string lock_text(const std::uint64_t world_locks)
{
    if (world_locks == 0) {
        return "no locks";
    }
    const auto bgl{ world_locks / 10'000 };
    const auto dl{ world_locks % 10'000 / 100 };
    const auto wl{ world_locks % 100 };
    std::string out;
    if (bgl > 0) {
        out += fmt::format("{} BGL ", bgl);
    }
    if (dl > 0) {
        out += fmt::format("{} DL ", dl);
    }
    if (wl > 0) {
        out += fmt::format("{} WL ", wl);
    }
    return fmt::format("{}(= {} WL)", out, world_locks);
}
}

Menu::Menu(
    core::handlers::FeatureHandler& features,
    core::handlers::InventoryHandler& inventory,
    const CommandRegistry& registry,
    Runner run
)
    : features_{ features }
    , inventory_{ inventory }
    , registry_{ registry }
    , run_{ std::move(run) }
{ }

bool Menu::owns(const std::string_view dialog_name)
{
    return dialog_name.starts_with(PREFIX);
}

void Menu::show(const Page page) const
{
    features_.show_dialog(build(page));
}

std::string Menu::build(const Page page) const
{
    switch (page) {
    case Page::Main: return main_page();
    case Page::Worlds: return worlds_page();
    case Page::Items: return items_page();
    case Page::Looks: return looks_page();
    case Page::Info: return info_page();
    case Page::Inventory: return inventory_page();
    case Page::DropAll: return drop_all_page();
    }
    return main_page();
}

std::string Menu::main_page() const
{
    Dialog d;
    d.header("GTProxy", ICON_MENU);

    if (features_.in_world()) {
        std::string where{ fmt::format("`oWorld: `w{}``", sanitize(features_.current_world())) };
        if (const auto pos{ features_.local_position() }) {
            where += fmt::format("   Tile: `w{}, {}``", static_cast<int>(pos->x / 32.0f), static_cast<int>(pos->y / 32.0f));
        }
        d.small(where);
    } else {
        d.small("`oNot in a world");
    }
    if (inventory_.known()) {
        d.small(fmt::format("`oBackpack: `w{}/{}`` slots   Locks: `w{}``", inventory_.items().size(), inventory_.slots(), lock_text(inventory_.lock_balance())));
    }
    if (inventory_.busy()) {
        d.small(fmt::format("`4Dropping... {} item(s) left``", inventory_.remaining()));
    }

    d.spacer()
        .section("Worlds", ICON_WORLDS).button("page_worlds", "Warp, back, recent worlds")
        .section("Items", ICON_ITEMS).button("page_items", "Drop all, locks, fast drop")
        .section("Looks", ICON_LOOKS).button("page_looks", "Nick, netIDs, skin")
        .section("Info", ICON_INFO).button("page_info", "Players, world, item search");
    return d.end(Page::Main);
}

std::string Menu::worlds_page() const
{
    Dialog d;
    d.header("Worlds", ICON_WORLDS);
    d.small(fmt::format(
        "`oNow: `w{}``   Before: `w{}``",
        features_.in_world() ? sanitize(features_.current_world()) : "-",
        features_.previous_world().empty() ? "-" : sanitize(features_.previous_world())
    ));
    d.spacer()
        .input("world", "World (WORLD or WORLD DOOR):", "", 30)
        .button("warp", "`2Warp``");
    if (!features_.previous_world().empty()) {
        d.button("back", fmt::format("Back to {}", sanitize(features_.previous_world())));
    }
    if (features_.in_world()) {
        d.button("rejoin", "Rejoin this world");
    }

    const auto& recent{ features_.recent_worlds() };
    if (recent.size() > 1 || (!recent.empty() && recent.front() != features_.current_world())) {
        d.spacer().text("`wRecent worlds``");
        for (std::size_t i{ 0 }; i < recent.size(); ++i) {
            if (recent[i] != features_.current_world()) {
                d.button(fmt::format("recent_{}", i), sanitize(recent[i]));
            }
        }
    }

    d.spacer().button("page_main", "<< Menu");
    return d.end(Page::Worlds);
}

std::string Menu::items_page() const
{
    Dialog d;
    d.header("Items", ICON_ITEMS);
    if (inventory_.known()) {
        d.small(fmt::format("`oBackpack: `w{}`` items in `w{}`` slots", inventory_.items().size(), inventory_.slots()));
        d.item(fmt::format("`oLocks: `w{}``", lock_text(inventory_.lock_balance())), ICON_LOCK);
    } else {
        d.small("`oThe inventory arrives when you log in.");
    }
    if (inventory_.busy()) {
        d.small(fmt::format("`4Dropping... {} item(s) left``", inventory_.remaining()));
        d.button("drop_stop", "`4Stop dropping``");
    }

    d.spacer()
        .button("inventory", "Show inventory")
        .button("dropall", "Drop all except favourites...")
        .button("dd", "Drop all locks (WL, DL, BGL)")
        .spacer()
        .checkbox("fast_drop", "Fast drop: no \"how many?\", drop the whole stack", inventory_.fast_drop())
        .checkbox("fast_trash", "Fast trash: same for trash (never for locks)", inventory_.fast_trash())
        .input("drop_gap", "Pause between drops (ms, 100-2000):", std::to_string(inventory_.drop_gap().count()), 4)
        .small("`oKeep list: items /dropall never drops - /keep add <item>")
        .spacer()
        .button("page_main", "<< Menu");
    return d.end(Page::Items, "Apply");
}

std::string Menu::looks_page() const
{
    Dialog d;
    d.header("Looks", ICON_LOOKS);
    d.small("`oOnly you see these changes.");
    d.spacer()
        .input("nick", "Nick:", features_.nick(), 64)
        .button("set_nick", "Set nick");
    if (!features_.nick().empty()) {
        d.button("reset_nick", "Back to my real name");
    }
    d.checkbox("show_netid", "Show netIDs next to names", features_.show_net_ids());
    if (registry_.get("rainbow")) {
        d.button("rainbow", "Rainbow skin on/off");
    }
    d.input("skin", "Skin colour code:", "", 10)
        .button("set_skin", "Set skin colour")
        .spacer()
        .button("page_main", "<< Menu");
    return d.end(Page::Looks, "Apply");
}

std::string Menu::info_page() const
{
    Dialog d;
    d.header("Info", ICON_INFO);
    if (features_.in_world()) {
        d.button("players", "Players in this world")
            .button("world_info", "World info")
            .button("pos", "My position");
    }
    d.spacer()
        .input("find", "Find item by name:", "", 30)
        .button("find_item", "Search")
        .input("item_id", "Item ID:", "", 6)
        .button("item_info", "Show item")
        .spacer()
        .button("commands", "All proxy commands")
        .button("page_main", "<< Menu");
    return d.end(Page::Info);
}

std::string Menu::inventory_page() const
{
    Dialog d;
    d.header("Inventory", ICON_ITEMS);
    if (!inventory_.known()) {
        d.small("`oThe inventory arrives when you log in.");
        return d.spacer().button("page_items", "<< Items").end(Page::Inventory);
    }

    d.small(fmt::format("`o{} items in {} slots. `5fav`` = favourite/marked, `2worn`` = worn, `4keep`` = keep list", inventory_.items().size(), inventory_.slots()));
    d.spacer();

    constexpr std::size_t MAX_SHOWN{ 150 };
    std::size_t shown{ 0 };
    for (const auto& [id, item] : inventory_.items()) {
        if (shown++ >= MAX_SHOWN) {
            d.small(fmt::format("`o... and {} more", inventory_.items().size() - MAX_SHOWN));
            break;
        }

        std::string marks;
        if ((item.flags & ~core::inventory::FLAG_WORN) != 0) {
            marks += fmt::format(" `5fav(0x{:02X})``", item.flags);
        }
        if ((item.flags & core::inventory::FLAG_WORN) != 0) {
            marks += " `2worn``";
        }
        if (inventory_.keep_list().contains(id)) {
            marks += " `4keep``";
        }
        d.item(fmt::format("`w{}`` x{} `o(ID {})``{}", name_of(id), item.count, id, marks), id);
    }

    d.spacer().button("page_items", "<< Items");
    return d.end(Page::Inventory);
}

Menu::DropPlan Menu::plan_drop_all() const
{
    DropPlan plan;
    for (const auto& [id, item] : inventory_.items()) {
        const auto reason{ inventory_.keep_reason(item) };
        if (reason == InventoryHandler::KeepReason::None) {
            plan.drop.push_back({ id, 0 });
        } else {
            plan.keep.emplace_back(item, reason);
        }
    }

    // Biggest stacks first.
    std::ranges::stable_sort(plan.drop, [this](const auto& a, const auto& b) {
        return inventory_.count_of(a.item_id) > inventory_.count_of(b.item_id);
    });
    return plan;
}

std::string Menu::drop_all_page() const
{
    Dialog d;
    d.header("Drop all", ICON_ITEMS);
    if (!inventory_.known()) {
        d.small("`oThe inventory arrives when you log in.");
        return d.spacer().button("page_items", "<< Items").end(Page::DropAll);
    }

    const auto plan{ plan_drop_all() };
    if (plan.drop.empty()) {
        d.small("`oNothing to drop: everything is kept.");
        return d.spacer().button("page_items", "<< Items").end(Page::DropAll);
    }

    const auto per_item{ inventory_.drop_gap().count() + 250 };
    d.text(fmt::format("`wDrops {} stack(s)`` where you stand, about {} s.", plan.drop.size(), (plan.drop.size() * per_item + 999) / 1000));

    constexpr std::size_t MAX_SHOWN{ 40 };
    for (std::size_t i{ 0 }; i < plan.drop.size() && i < MAX_SHOWN; ++i) {
        const auto id{ plan.drop[i].item_id };
        d.item(fmt::format("`w{}`` x{}", name_of(id), inventory_.count_of(id)), id);
    }
    if (plan.drop.size() > MAX_SHOWN) {
        d.small(fmt::format("`o... and {} more", plan.drop.size() - MAX_SHOWN));
    }

    if (!plan.keep.empty()) {
        d.spacer().text(fmt::format("`2Kept ({})``", plan.keep.size()));
        std::string kept;
        for (const auto& [item, reason] : plan.keep) {
            if (reason == InventoryHandler::KeepReason::FistOrWrench) {
                continue;
            }
            kept += fmt::format("{}{} ({})", kept.empty() ? "" : ", ", name_of(item.id), InventoryHandler::describe(reason));
        }
        if (!kept.empty()) {
            d.small("`o" + kept);
        }
    }

    d.spacer()
        .button("dropall_go", fmt::format("`4Drop {} stack(s) now``", plan.drop.size()))
        .button("page_items", "<< Items");
    return d.end(Page::DropAll);
}

void Menu::handle(const packet::message::DialogReturn& dialog)
{
    const std::string& name{ dialog.dialog_name };
    const std::string& button{ dialog.button_clicked };
    spdlog::info("Proxy menu: {} {}", name, button.empty() ? "apply" : button);

    const auto value = [&](const std::string& field) { return sanitize(dialog.field(field)); };
    const auto with_text = [&](const std::string& command, const std::string& field, const std::string& missing) {
        const std::string text{ value(field) };
        if (text.find_first_not_of(' ') == std::string::npos) {
            features_.message(missing);
            return;
        }
        run_(command + " " + text);
    };

    // Settings on the page apply with any button (or Apply).
    if (name == dialog_name(Page::Items)) {
        if (const bool on{ dialog.checked("fast_drop") }; on != inventory_.fast_drop()) {
            inventory_.set_fast_drop(on);
        }
        if (const bool on{ dialog.checked("fast_trash") }; on != inventory_.fast_trash()) {
            inventory_.set_fast_trash(on);
        }
        const std::string gap{ value("drop_gap") };
        int ms{};
        if (std::from_chars(gap.data(), gap.data() + gap.size(), ms).ec == std::errc{} && ms != inventory_.drop_gap().count()) {
            inventory_.set_drop_gap(std::chrono::milliseconds{ ms });
        }
    }
    if (name == dialog_name(Page::Looks)) {
        if (const bool show{ dialog.checked("show_netid") }; show != features_.show_net_ids()) {
            features_.set_show_net_ids(show);
        }
    }

    if (button == "page_main") { show(Page::Main); }
    else if (button == "page_worlds") { show(Page::Worlds); }
    else if (button == "page_items") { show(Page::Items); }
    else if (button == "page_looks") { show(Page::Looks); }
    else if (button == "page_info") { show(Page::Info); }
    else if (button == "inventory") { show(Page::Inventory); }
    else if (button == "dropall") { show(Page::DropAll); }
    else if (button == "dropall_go") { run_("dropall now"); }
    else if (button == "drop_stop") { run_("dropall stop"); }
    else if (button == "dd") { run_("dd"); }
    else if (button == "warp") { with_text("warp", "world", "`4Oops: ``Type a world name first."); }
    else if (button == "back") { run_("back"); }
    else if (button == "rejoin") { run_("rejoin"); }
    else if (button.starts_with("recent_")) {
        std::size_t index{};
        const auto digits{ std::string_view{ button }.substr(7) };
        const auto& recent{ features_.recent_worlds() };
        if (std::from_chars(digits.data(), digits.data() + digits.size(), index).ec == std::errc{} && index < recent.size()) {
            run_("warp " + recent[index]);
        }
    }
    else if (button == "set_nick") { with_text("nick", "nick", "`4Oops: ``Type a nick first."); }
    else if (button == "reset_nick") { run_("nick"); }
    else if (button == "rainbow") { run_("rainbow"); }
    else if (button == "set_skin") { with_text("skin", "skin", "`4Oops: ``Type a colour code first."); }
    else if (button == "players") { run_("players"); }
    else if (button == "world_info") { run_("world"); }
    else if (button == "pos") { run_("pos"); }
    else if (button == "find_item") { with_text("find", "find", "`4Oops: ``Type part of an item name first."); }
    else if (button == "item_info") { with_text("item", "item_id", "`4Oops: ``Type an item ID first."); }
    else if (button == "commands") { run_("proxyhelp"); }
    else if (button.empty() && (name == dialog_name(Page::Items) || name == dialog_name(Page::Looks))) {
        features_.message("`oSettings saved.");
    }
}
}
