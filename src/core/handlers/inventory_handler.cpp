#include "inventory_handler.hpp"

#include <algorithm>
#include <span>

#include <fmt/format.h>
#include <spdlog/spdlog.h>
#include <spdlog/fmt/bin_to_hex.h>

#include "../../item/item_database.hpp"
#include "../../packet/game/inventory.hpp"
#include "../../packet/game/world.hpp"
#include "../../world/world.hpp"

namespace core::handlers {
namespace {
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

InventoryHandler::InventoryHandler(event::Dispatcher& dispatcher, SessionHandler& session)
    : dispatcher_{ dispatcher }
    , session_{ session }
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
}

std::uint32_t InventoryHandler::count_of(const std::uint16_t item_id) const
{
    const auto it{ items_.find(item_id) };
    return it == items_.end() ? 0 : it->second.count;
}

void InventoryHandler::on_inventory(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::SendInventoryState>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::SendInventoryState>() : nullptr };
    if (!pkt) {
        return;
    }

    // The raw start of the packet goes into the session log so the format
    // can be checked against what the server really sends.
    const auto head{ std::span{ pkt->extra }.first(std::min<std::size_t>(pkt->extra.size(), 48)) };
    spdlog::info("Inventory packet: {} bytes, starts with {:n}", pkt->extra.size(), spdlog::to_hex(head.begin(), head.end()));

    const auto parsed{ inventory::parse(pkt->extra) };
    if (!parsed) {
        spdlog::warn("Could not read the inventory packet ({} bytes)", pkt->extra.size());
        return;
    }

    const std::size_t expected{ 7 + parsed->items.size() * 4 };
    if (pkt->extra.size() != expected && pkt->extra.size() != expected + 1) {
        spdlog::warn("Inventory packet is {} bytes, the known format needs {}: the layout may have changed", pkt->extra.size(), expected);
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
    std::string sample;
    for (const auto& item : parsed->items) {
        if (sample.size() > 400) {
            sample += " ...";
            break;
        }
        sample += fmt::format("{}{} x{}{}", sample.empty() ? "" : ", ", item_name(item.id), item.count, item.flags ? fmt::format(" [flags 0x{:02X}]", item.flags) : "");
    }
    spdlog::info("Inventory items: {}", sample.empty() ? "none" : sample);

    if (on_change_) {
        on_change_(Change{ true });
    }
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
    spdlog::debug("Inventory change: {} -{} +{} (had {})", item_name(id), removed, added, count_of(id));

    auto& item{ items_[id] };
    item.id = id;
    item.count = static_cast<std::uint8_t>(std::clamp(int{ item.count } - removed + added, 0, 255));
    if (item.count == 0) {
        items_.erase(id);
    }

    if (on_change_) {
        on_change_(Change{ false, id, removed, added });
    }
}

void InventoryHandler::on_object(const event::Event& event)
{
    const auto evt{ as<packet::PacketId::ItemChangeObject>(event, event::Direction::ClientBound) };
    const auto pkt{ evt ? evt->get<packet::game::ItemChangeObject>() : nullptr };
    if (!pkt) {
        return;
    }

    // Collected by us: the object's item goes into the inventory.
    const auto change{ static_cast<std::int32_t>(pkt->game_packet.net_id) };
    const auto local{ session_.local_net_id() };
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

    if (on_change_) {
        on_change_(Change{ false, it->item_id, 0, it->amount });
    }
}
}
