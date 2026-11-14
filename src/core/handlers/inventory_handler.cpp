#include "inventory_handler.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include "../../item/item_database.hpp"
#include "../../packet/game/dialog.hpp"
#include "../../packet/game/inventory.hpp"
#include "../../packet/game/world.hpp"
#include "../../world/world.hpp"

namespace core::handlers {
namespace {
constexpr std::uint16_t FIST{ 18 };
constexpr std::uint16_t WRENCH{ 32 };
constexpr auto DIALOG_TIMEOUT{ std::chrono::milliseconds{ 2000 } };
constexpr auto ACK_TIMEOUT{ std::chrono::milliseconds{ 1500 } };

template <packet::PacketId Id>
const event::TypedPacketEvent<Id>* as(const event::Event& event, const event::Direction direction)
{
    const auto evt{ dynamic_cast<const event::TypedPacketEvent<Id>*>(&event) };
    if (!evt || evt->direction != direction) {
        return nullptr;
    }
    return evt;
}

std::string item_name(const std::uint16_t id)
{
    if (const auto* info{ item::ItemDatabase::instance().get_item(id) }) {
        return info->item_name;
    }
    return fmt::format("item {}", id);
}
}

InventoryHandler::InventoryHandler(event::Dispatcher& dispatcher, FeatureHandler& features, std::filesystem::path settings_file)
    : dispatcher_{ dispatcher }
    , features_{ features }
    , settings_file_{ std::move(settings_file) }
{
    const auto listen = [this](const packet::PacketId id, void (InventoryHandler::*handler)(const event::Event&), const int8_t priority) {
        const auto type{ event::packet_event_type(id) };
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this, handler](const event::Event& e) { (this->*handler)(e); }, priority)
        );
    };

    listen(packet::PacketId::SendInventoryState, &InventoryHandler::on_inventory, event::Priority::Normal);
    listen(packet::PacketId::ModifyItemInventory, &InventoryHandler::on_modify, event::Priority::Normal);
    // Before the world handler removes a collected object from the map.
    listen(packet::PacketId::ItemChangeObject, &InventoryHandler::on_object, event::Priority::FairlyHigh);
    // Before scripts see it: the proxy may answer the dialog itself.
    listen(packet::PacketId::OnDialogRequest, &InventoryHandler::on_dialog, event::Priority::FairlyHigh);
    listen(packet::PacketId::OnRequestWorldSelectMenu, &InventoryHandler::on_world_menu, event::Priority::Normal);

    for (const auto type : { event::Type::ClientDisconnect, event::Type::ServerDisconnect }) {
        handles_.emplace_back(
            dispatcher_,
            type,
            dispatcher_.append_listener(type, [this](const event::Event&) {
                queue_.clear();
                current_.reset();
            })
        );
    }

    load_settings();
}

std::uint32_t InventoryHandler::count_of(const std::uint16_t item_id) const
{
    const auto it{ items_.find(item_id) };
    return it == items_.end() ? 0 : it->second.count;
}

std::uint64_t InventoryHandler::lock_balance() const
{
    std::uint64_t total{ 0 };
    for (const auto& [id, item] : items_) {
        if (const auto* info{ item::ItemDatabase::instance().get_item(id) }) {
            total += inventory::lock_value(info->item_name) * item.count;
        }
    }
    return total;
}

InventoryHandler::KeepReason InventoryHandler::keep_reason(const inventory::Item& item) const
{
    if (item.id == FIST || item.id == WRENCH) {
        return KeepReason::FistOrWrench;
    }
    if (keep_.contains(item.id)) {
        return KeepReason::KeepList;
    }
    // The worn bit is known; any other bit marks the item specially in the
    // game (favourites) - keep those rather than risk dropping them.
    if ((item.flags & ~inventory::FLAG_WORN) != 0) {
        return KeepReason::Favorite;
    }
    if ((item.flags & inventory::FLAG_WORN) != 0) {
        return KeepReason::Worn;
    }
    if (const auto* info{ item::ItemDatabase::instance().get_item(item.id) }; info && info->flags.untradeable) {
        return KeepReason::Untradeable;
    }
    return KeepReason::None;
}

std::string_view InventoryHandler::describe(const KeepReason reason)
{
    switch (reason) {
    case KeepReason::FistOrWrench: return "fist/wrench";
    case KeepReason::Untradeable: return "untradeable";
    case KeepReason::Favorite: return "favourite";
    case KeepReason::Worn: return "worn";
    case KeepReason::KeepList: return "keep list";
    case KeepReason::None: break;
    }
    return "";
}

std::size_t InventoryHandler::drop(const std::vector<Job>& jobs)
{
    if (!busy()) {
        dropped_ = 0;
        failed_ = 0;
    }

    std::size_t queued{ 0 };
    for (const auto& job : jobs) {
        if (job.item_id == FIST || job.item_id == WRENCH) {
            continue;
        }
        queue_.push_back(job);
        ++queued;
    }
    return queued;
}

void InventoryHandler::stop()
{
    queue_.clear();
    current_.reset();
}

void InventoryHandler::set_fast_drop(const bool on)
{
    fast_drop_ = on;
    save_settings();
}

void InventoryHandler::set_fast_trash(const bool on)
{
    fast_trash_ = on;
    save_settings();
}

void InventoryHandler::set_drop_gap(const std::chrono::milliseconds gap)
{
    drop_gap_ = std::clamp(gap, MIN_GAP, MAX_GAP);
    save_settings();
}

bool InventoryHandler::keep(const std::uint16_t item_id, const bool keep)
{
    const bool changed{ keep ? keep_.insert(item_id).second : keep_.erase(item_id) > 0 };
    if (changed) {
        save_settings();
    }
    return changed;
}

void InventoryHandler::update()
{
    const auto now{ std::chrono::steady_clock::now() };

    if (current_ && now >= deadline_) {
        if (stage_ == Stage::WaitDialog) {
            spdlog::warn("Drop: the server did not offer to drop {} - skipped", item_name(current_->item_id));
            finish_current(false);
        } else {
            // The answer was sent; the server just did not confirm in time.
            finish_current(true);
        }
    }

    if (!current_ && !queue_.empty() && now >= next_send_) {
        send_next();
    }
}

void InventoryHandler::send_next()
{
    while (!queue_.empty()) {
        const Job job{ queue_.front() };
        queue_.pop_front();

        if (known_ && count_of(job.item_id) == 0) {
            continue; // already gone
        }

        if (!features_.send_text_to_server(inventory::drop_request(job.item_id))) {
            features_.message("`4Drop stopped: ``not connected to the game server.");
            stop();
            return;
        }

        spdlog::info("Drop: asking the server to drop {} ({})", item_name(job.item_id), job.item_id);
        current_ = job;
        stage_ = Stage::WaitDialog;
        deadline_ = std::chrono::steady_clock::now() + DIALOG_TIMEOUT;
        return;
    }

    features_.message(fmt::format(
        "`oDrop finished: `w{}`` stack(s) dropped{}.",
        dropped_,
        failed_ > 0 ? fmt::format(", `4{}`` refused by the server", failed_) : ""
    ));
}

void InventoryHandler::finish_current(const bool dropped)
{
    (dropped ? dropped_ : failed_) += 1;
    current_.reset();
    next_send_ = std::chrono::steady_clock::now() + drop_gap_;
    if (queue_.empty()) {
        send_next(); // prints the summary
    }
}

void InventoryHandler::on_inventory(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::SendInventoryState>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::SendInventoryState>() : nullptr };
    if (!pkt) {
        return;
    }

    const auto parsed{ inventory::parse(pkt->extra) };
    if (!parsed) {
        spdlog::warn("Could not read the inventory packet ({} bytes)", pkt->extra.size());
        return;
    }

    known_ = true;
    slots_ = parsed->slots;
    items_.clear();
    std::size_t marked{ 0 };
    for (const auto& item : parsed->items) {
        items_[item.id] = item;
        marked += item.flags != 0 ? 1 : 0;
    }
    spdlog::info("Inventory: {} items in {} slots ({} with flags)", items_.size(), slots_, marked);
}

void InventoryHandler::on_modify(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::ModifyItemInventory>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::ModifyItemInventory>() : nullptr };
    if (!pkt) {
        return;
    }

    // value = item, jump_count = amount removed, animation_type = amount added.
    const auto id{ static_cast<std::uint16_t>(pkt->game_packet.item_id) };
    const int removed{ pkt->game_packet.pad[1] };
    const int added{ pkt->game_packet.pad[2] };

    auto& item{ items_[id] };
    item.id = id;
    item.count = static_cast<std::uint8_t>(std::clamp(int{ item.count } - removed + added, 0, 255));
    if (item.count == 0) {
        items_.erase(id);
    }

    if (current_ && current_->item_id == id && stage_ == Stage::WaitAck && removed > 0) {
        finish_current(true);
    }
}

void InventoryHandler::on_object(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::ItemChangeObject>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::ItemChangeObject>() : nullptr };
    if (!pkt) {
        return;
    }

    const auto change{ static_cast<std::int32_t>(pkt->game_packet.net_id) };

    // A new object of the item we are dropping: the drop went through.
    if (change == -1 && current_ && stage_ == Stage::WaitAck && pkt->item_id == current_->item_id) {
        finish_current(true);
        return;
    }

    // Collected by us: the object's item goes into the inventory.
    const auto local{ features_.local_net_id() };
    if (local < 0 || change != local) {
        return;
    }

    const auto& objects{ world::World::instance().get_object_map().get_objects() };
    const auto it{ std::ranges::find_if(objects, [&](const world::Object& o) { return o.object_id == pkt->item_net_id; }) };
    if (it == objects.end() || it->item_id == 0) {
        return;
    }

    auto& item{ items_[it->item_id] };
    item.id = it->item_id;
    item.count = static_cast<std::uint8_t>(std::min(int{ item.count } + it->amount, 255));
}

void InventoryHandler::on_dialog(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::OnDialogRequest>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::OnDialogRequest>() : nullptr };
    if (!pkt) {
        return;
    }

    const auto dialog{ inventory::parse_item_dialog(pkt->dialog) };
    if (!dialog) {
        return;
    }

    const bool ours{ current_ && stage_ == Stage::WaitDialog && dialog->dialog_name == "drop_item" && dialog->item_id == current_->item_id };
    if (ours) {
        const std::uint32_t amount{ current_->limit > 0 ? std::min(current_->limit, dialog->count) : dialog->count };
        evt->cancel();
        if (amount == 0) {
            finish_current(false);
            return;
        }
        spdlog::info("Drop: dropping {} of {} ({} owned)", amount, item_name(dialog->item_id), dialog->count);
        features_.send_text_to_server(inventory::item_dialog_reply(dialog->dialog_name, dialog->item_id, amount));
        stage_ = Stage::WaitAck;
        deadline_ = std::chrono::steady_clock::now() + ACK_TIMEOUT;
        return;
    }

    // Fast drop / fast trash for what you drop or trash yourself. Never for
    // locks: those keep the normal "how many?" window.
    const bool is_lock{ inventory::lock_value(item_name(dialog->item_id)) > 0 };
    const bool fast{
        dialog->count > 0
        && ((dialog->dialog_name == "drop_item" && fast_drop_)
            || (dialog->dialog_name == "trash_item" && fast_trash_ && !is_lock))
    };
    if (!fast) {
        return;
    }

    evt->cancel();
    spdlog::info("Fast {}: {} x{}", dialog->dialog_name == "drop_item" ? "drop" : "trash", item_name(dialog->item_id), dialog->count);
    features_.send_text_to_server(inventory::item_dialog_reply(dialog->dialog_name, dialog->item_id, dialog->count));
}

void InventoryHandler::on_world_menu(const event::Event& event)
{
    if (as<packet::PacketId::OnRequestWorldSelectMenu>(event, event::Direction::ClientBound) && busy()) {
        features_.message(fmt::format("`4Drop stopped: ``you left the world ({} left).", remaining()));
        stop();
    }
}

void InventoryHandler::load_settings()
{
    std::ifstream file{ settings_file_ };
    std::string line;
    while (std::getline(file, line)) {
        const auto eq{ line.find('=') };
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key{ line.substr(0, eq) };
        const std::string value{ line.substr(eq + 1) };

        if (key == "fast_drop") {
            fast_drop_ = value == "1";
        } else if (key == "fast_trash") {
            fast_trash_ = value == "1";
        } else if (key == "drop_gap_ms") {
            int ms{};
            if (std::from_chars(value.data(), value.data() + value.size(), ms).ec == std::errc{}) {
                drop_gap_ = std::clamp(std::chrono::milliseconds{ ms }, MIN_GAP, MAX_GAP);
            }
        } else if (key == "keep") {
            std::stringstream ids{ value };
            std::string id;
            while (std::getline(ids, id, ',')) {
                std::uint16_t number{};
                if (std::from_chars(id.data(), id.data() + id.size(), number).ec == std::errc{}) {
                    keep_.insert(number);
                }
            }
        }
    }
}

void InventoryHandler::save_settings() const
{
    std::ofstream file{ settings_file_, std::ios::trunc };
    if (!file) {
        spdlog::warn("Could not save {}", settings_file_.string());
        return;
    }

    std::string keep;
    for (const auto id : keep_) {
        keep += (keep.empty() ? "" : ",") + std::to_string(id);
    }

    file << "# GTProxy item settings (written by the proxy)\n"
         << "fast_drop=" << (fast_drop_ ? 1 : 0) << '\n'
         << "fast_trash=" << (fast_trash_ ? 1 : 0) << '\n'
         << "drop_gap_ms=" << drop_gap_.count() << '\n'
         << "keep=" << keep << '\n';
}
}
