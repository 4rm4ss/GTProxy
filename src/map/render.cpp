#include "render.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <zlib.h>

#include "snapshot.hpp"
#include "../core/features.hpp"
#include "../world/world.hpp"

namespace map {
namespace {
constexpr int HEADER{ 20 };

// The classic 5x7 font, ASCII 32..126, five columns per glyph, bit 0 on top.
constexpr std::array<std::uint8_t, 95 * 5> FONT{
    0x00,0x00,0x00,0x00,0x00, 0x00,0x00,0x5F,0x00,0x00, 0x00,0x07,0x00,0x07,0x00, 0x14,0x7F,0x14,0x7F,0x14,
    0x24,0x2A,0x7F,0x2A,0x12, 0x23,0x13,0x08,0x64,0x62, 0x36,0x49,0x55,0x22,0x50, 0x00,0x05,0x03,0x00,0x00,
    0x00,0x1C,0x22,0x41,0x00, 0x00,0x41,0x22,0x1C,0x00, 0x08,0x2A,0x1C,0x2A,0x08, 0x08,0x08,0x3E,0x08,0x08,
    0x00,0x50,0x30,0x00,0x00, 0x08,0x08,0x08,0x08,0x08, 0x00,0x60,0x60,0x00,0x00, 0x20,0x10,0x08,0x04,0x02,
    0x3E,0x51,0x49,0x45,0x3E, 0x00,0x42,0x7F,0x40,0x00, 0x42,0x61,0x51,0x49,0x46, 0x21,0x41,0x45,0x4B,0x31,
    0x18,0x14,0x12,0x7F,0x10, 0x27,0x45,0x45,0x45,0x39, 0x3C,0x4A,0x49,0x49,0x30, 0x01,0x71,0x09,0x05,0x03,
    0x36,0x49,0x49,0x49,0x36, 0x06,0x49,0x49,0x29,0x1E, 0x00,0x36,0x36,0x00,0x00, 0x00,0x56,0x36,0x00,0x00,
    0x00,0x08,0x14,0x22,0x41, 0x14,0x14,0x14,0x14,0x14, 0x41,0x22,0x14,0x08,0x00, 0x02,0x01,0x51,0x09,0x06,
    0x32,0x49,0x79,0x41,0x3E, 0x7E,0x11,0x11,0x11,0x7E, 0x7F,0x49,0x49,0x49,0x36, 0x3E,0x41,0x41,0x41,0x22,
    0x7F,0x41,0x41,0x22,0x1C, 0x7F,0x49,0x49,0x49,0x41, 0x7F,0x09,0x09,0x01,0x01, 0x3E,0x41,0x41,0x51,0x32,
    0x7F,0x08,0x08,0x08,0x7F, 0x00,0x41,0x7F,0x41,0x00, 0x20,0x40,0x41,0x3F,0x01, 0x7F,0x08,0x14,0x22,0x41,
    0x7F,0x40,0x40,0x40,0x40, 0x7F,0x02,0x04,0x02,0x7F, 0x7F,0x04,0x08,0x10,0x7F, 0x3E,0x41,0x41,0x41,0x3E,
    0x7F,0x09,0x09,0x09,0x06, 0x3E,0x41,0x51,0x21,0x5E, 0x7F,0x09,0x19,0x29,0x46, 0x46,0x49,0x49,0x49,0x31,
    0x01,0x01,0x7F,0x01,0x01, 0x3F,0x40,0x40,0x40,0x3F, 0x1F,0x20,0x40,0x20,0x1F, 0x7F,0x20,0x18,0x20,0x7F,
    0x63,0x14,0x08,0x14,0x63, 0x03,0x04,0x78,0x04,0x03, 0x61,0x51,0x49,0x45,0x43, 0x00,0x00,0x7F,0x41,0x41,
    0x02,0x04,0x08,0x10,0x20, 0x41,0x41,0x7F,0x00,0x00, 0x04,0x02,0x01,0x02,0x04, 0x40,0x40,0x40,0x40,0x40,
    0x00,0x01,0x02,0x04,0x00, 0x20,0x54,0x54,0x54,0x78, 0x7F,0x48,0x44,0x44,0x38, 0x38,0x44,0x44,0x44,0x20,
    0x38,0x44,0x44,0x48,0x7F, 0x38,0x54,0x54,0x54,0x18, 0x08,0x7E,0x09,0x01,0x02, 0x08,0x14,0x54,0x54,0x3C,
    0x7F,0x08,0x04,0x04,0x78, 0x00,0x44,0x7D,0x40,0x00, 0x20,0x40,0x44,0x3D,0x00, 0x00,0x7F,0x10,0x28,0x44,
    0x00,0x41,0x7F,0x40,0x00, 0x7C,0x04,0x18,0x04,0x78, 0x7C,0x08,0x04,0x04,0x78, 0x38,0x44,0x44,0x44,0x38,
    0x7C,0x14,0x14,0x14,0x08, 0x08,0x14,0x14,0x18,0x7C, 0x7C,0x08,0x04,0x04,0x08, 0x48,0x54,0x54,0x54,0x20,
    0x04,0x3F,0x44,0x40,0x20, 0x3C,0x40,0x40,0x20,0x7C, 0x1C,0x20,0x40,0x20,0x1C, 0x3C,0x40,0x30,0x40,0x3C,
    0x44,0x28,0x10,0x28,0x44, 0x0C,0x50,0x50,0x50,0x3C, 0x44,0x64,0x54,0x4C,0x44, 0x00,0x08,0x36,0x41,0x00,
    0x00,0x00,0x7F,0x00,0x00, 0x00,0x41,0x36,0x08,0x00, 0x08,0x08,0x2A,0x1C,0x08,
};

std::uint32_t parse_color(const std::string& hex, const std::uint32_t fallback)
{
    if (hex.size() != 7 || hex[0] != '#') {
        return fallback;
    }
    try {
        return static_cast<std::uint32_t>(std::stoul(hex.substr(1), nullptr, 16));
    }
    catch (...) {
        return fallback;
    }
}

std::uint32_t shade(const std::uint32_t rgb, const float k)
{
    const auto part = [&](const int shift) { return static_cast<std::uint32_t>(std::clamp(static_cast<float>((rgb >> shift) & 0xff) * k, 0.0f, 255.0f)) << shift; };
    return part(16) | part(8) | part(0);
}

class Canvas {
public:
    explicit Canvas(Image& image)
        : image_{ image }
    { }

    void pixel(const int x, const int y, const std::uint32_t rgb)
    {
        if (x < 0 || y < 0 || x >= image_.width || y >= image_.height) {
            return;
        }
        const auto i{ (static_cast<std::size_t>(y) * static_cast<std::size_t>(image_.width) + static_cast<std::size_t>(x)) * 3 };
        image_.rgb[i] = static_cast<std::uint8_t>(rgb >> 16);
        image_.rgb[i + 1] = static_cast<std::uint8_t>(rgb >> 8);
        image_.rgb[i + 2] = static_cast<std::uint8_t>(rgb);
    }

    void fill(const int x, const int y, const int w, const int h, const std::uint32_t rgb)
    {
        for (int yy{ y }; yy < y + h; ++yy) {
            for (int xx{ x }; xx < x + w; ++xx) {
                pixel(xx, yy, rgb);
            }
        }
    }

    void frame(const int x, const int y, const int w, const int h, const std::uint32_t rgb, const int thickness = 1)
    {
        fill(x, y, w, thickness, rgb);
        fill(x, y + h - thickness, w, thickness, rgb);
        fill(x, y, thickness, h, rgb);
        fill(x + w - thickness, y, thickness, h, rgb);
    }

    void line(int x0, int y0, const int x1, const int y1, const std::uint32_t rgb, const int width)
    {
        const int dx{ std::abs(x1 - x0) };
        const int dy{ -std::abs(y1 - y0) };
        const int sx{ x0 < x1 ? 1 : -1 };
        const int sy{ y0 < y1 ? 1 : -1 };
        int error{ dx + dy };
        while (true) {
            fill(x0 - width / 2, y0 - width / 2, width, width, rgb);
            if (x0 == x1 && y0 == y1) {
                break;
            }
            const int e2{ 2 * error };
            if (e2 >= dy) {
                error += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }

    // ASCII text with a dark outline so it reads on any tile.
    void text(const int x, const int y, const std::string& value, const std::uint32_t rgb, const int scale = 1)
    {
        for (const int pass : { 0, 1 }) {
            int cx{ x };
            for (const char raw : value) {
                const auto c{ static_cast<unsigned char>(raw) };
                const int glyph{ c >= 32 && c <= 126 ? c - 32 : '?' - 32 };
                for (int col{ 0 }; col < 5; ++col) {
                    const auto bits{ FONT[static_cast<std::size_t>(glyph * 5 + col)] };
                    for (int row{ 0 }; row < 7; ++row) {
                        if ((bits >> row) & 1) {
                            if (pass == 0) {
                                fill(cx + col * scale - 1, y + row * scale - 1, scale + 2, scale + 2, 0x000000);
                            }
                            else {
                                fill(cx + col * scale, y + row * scale, scale, scale, rgb);
                            }
                        }
                    }
                }
                cx += 6 * scale;
            }
        }
    }

private:
    Image& image_;
};

std::string ascii(const std::string& text)
{
    std::string out;
    for (const char c : core::features::strip_colors(text)) {
        const auto u{ static_cast<unsigned char>(c) };
        if (u >= 32 && u <= 126) {
            out += c;
        }
        else if ((u & 0xC0) != 0x80) {
            out += '?';     // one ? per non-ASCII letter (UTF-8 lead byte)
        }
    }
    return out;
}
}

std::uint32_t Image::at(const int x, const int y) const
{
    const auto i{ (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)) * 3 };
    return (static_cast<std::uint32_t>(rgb[i]) << 16) | (static_cast<std::uint32_t>(rgb[i + 1]) << 8) | rgb[i + 2];
}

Image render_image(const world::World& world, const Grid& grid, const RenderInput& input)
{
    Image image{};
    const auto& tile_map{ world.get_tile_map() };
    const auto size{ tile_map.get_size() };
    const auto& tiles{ tile_map.get_tiles() };
    if (size.x <= 0 || size.y <= 0 || tiles.size() < static_cast<std::size_t>(size.x) * static_cast<std::size_t>(size.y)) {
        return image;
    }

    const int ts{ std::clamp(input.tile_pixels, 4, 64) };
    image.width = size.x * ts;
    image.height = size.y * ts + HEADER;
    image.rgb.assign(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height) * 3, 0);
    Canvas canvas{ image };
    canvas.fill(0, 0, image.width, image.height, 0x151a22);
    canvas.fill(0, 0, image.width, HEADER, 0x171b22);
    canvas.text(6, 6, ascii(input.title), 0xffcf4a);

    const auto origin_y{ HEADER };
    const auto tile_x = [&](const int x) { return x * ts; };
    const auto tile_y = [&](const int y) { return origin_y + y * ts; };

    for (int y{ 0 }; y < size.y; ++y) {
        for (int x{ 0 }; x < size.x; ++x) {
            const auto& tile{ tiles[static_cast<std::size_t>(y) * static_cast<std::size_t>(size.x) + static_cast<std::size_t>(x)] };
            const int px{ tile_x(x) };
            const int py{ tile_y(y) };
            if (tile.background != 0) {
                canvas.fill(px, py, ts, ts, shade(parse_color(item_color(tile.background), 0x888888), 0.38f));
            }
            if (tile.foreground != 0) {
                const auto cell{ grid.at({ x, y }) };
                const auto color{ parse_color(item_color(tile.foreground), 0x888888) };
                if (cell == Cell::Platform) {
                    canvas.fill(px, py, ts, std::max(2, ts * 3 / 10), color);
                }
                else {
                    canvas.fill(px, py, ts, ts, cell == Cell::Door || cell == Cell::OpenDoor ? shade(color, 0.7f) : color);
                }
                if (cell == Cell::Hazard) {
                    canvas.frame(px, py, ts, ts, 0xff3c3c, std::max(1, ts / 8));
                }
                if (cell == Cell::Door || cell == Cell::OpenDoor) {
                    canvas.frame(px, py, ts, ts, cell == Cell::OpenDoor ? 0x5fd38d : 0x7aa7ff, std::max(1, ts / 10));
                }
            }
        }
    }

    for (const auto& area : input.areas) {
        const int x1{ std::min(area.from.x, area.to.x) };
        const int y1{ std::min(area.from.y, area.to.y) };
        const int x2{ std::max(area.from.x, area.to.x) };
        const int y2{ std::max(area.from.y, area.to.y) };
        const auto color{ parse_color(area.color, 0x4fc3ff) };
        canvas.frame(tile_x(x1), tile_y(y1), (x2 - x1 + 1) * ts, (y2 - y1 + 1) * ts, color, 2);
        if (!area.label.empty()) {
            canvas.text(tile_x(x1) + 2, tile_y(y1) + 2, ascii(area.label), color);
        }
    }

    for (const auto& route : input.routes) {
        const auto color{ parse_color(route.color, 0xc77dff) };
        for (std::size_t i{ 1 }; i < route.tiles.size(); ++i) {
            canvas.line(tile_x(route.tiles[i - 1].x) + ts / 2, tile_y(route.tiles[i - 1].y) + ts / 2,
                tile_x(route.tiles[i].x) + ts / 2, tile_y(route.tiles[i].y) + ts / 2, color, std::max(2, ts / 6));
        }
    }

    for (const auto& object : world.get_object_map().get_objects()) {
        const int cx{ static_cast<int>((object.pos.x + 8.0f) / 32.0f * static_cast<float>(ts)) };
        const int cy{ origin_y + static_cast<int>((object.pos.y + 8.0f) / 32.0f * static_cast<float>(ts)) };
        const int r{ std::max(2, ts / 5) };
        canvas.fill(cx - r, cy - r, 2 * r, 2 * r, 0x000000);
        canvas.fill(cx - r + 1, cy - r + 1, 2 * r - 2, 2 * r - 2, parse_color(item_color(object.item_id), 0xffd76a));
    }

    for (const auto& [net_id, player] : world.get_players()) {
        if (!player || (player->invisible() != 0 && !player->is_local())) {
            continue;
        }
        const glm::vec2 position{ player->position() };
        const int px{ static_cast<int>(position.x / 32.0f * static_cast<float>(ts)) };
        const int py{ origin_y + static_cast<int>(position.y / 32.0f * static_cast<float>(ts)) };
        const auto color{ player->is_local() ? 0xffcf4a : 0xe6e9ef };
        canvas.fill(px, py, std::max(3, ts * 20 / 32), std::max(4, ts * 30 / 32), color);
        canvas.frame(px, py, std::max(3, ts * 20 / 32), std::max(4, ts * 30 / 32), 0x000000);
        canvas.text(px, py - 9, ascii(player->name()), color);
    }

    for (const auto& marker : input.markers) {
        const auto color{ parse_color(marker.color, 0xffcf4a) };
        canvas.frame(tile_x(marker.tile.x), tile_y(marker.tile.y), ts, ts, color, std::max(2, ts / 8));
        if (!marker.label.empty()) {
            canvas.text(tile_x(marker.tile.x) + ts + 2, tile_y(marker.tile.y) + ts / 2 - 3, ascii(marker.label), color);
        }
    }

    return image;
}

std::vector<std::uint8_t> encode_png(const Image& image)
{
    if (image.width <= 0 || image.height <= 0) {
        return {};
    }

    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(image.height) * (static_cast<std::size_t>(image.width) * 3 + 1));
    for (int y{ 0 }; y < image.height; ++y) {
        raw.push_back(0);   // no filter
        const auto row{ image.rgb.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * static_cast<std::size_t>(image.width) * 3) };
        raw.insert(raw.end(), row, row + image.width * 3);
    }

    uLongf compressed_size{ compressBound(static_cast<uLong>(raw.size())) };
    std::vector<std::uint8_t> compressed(compressed_size);
    if (compress2(compressed.data(), &compressed_size, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK) {
        return {};
    }
    compressed.resize(compressed_size);

    std::vector<std::uint8_t> png{ 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    const auto u32 = [&](const std::uint32_t value) {
        png.push_back(static_cast<std::uint8_t>(value >> 24));
        png.push_back(static_cast<std::uint8_t>(value >> 16));
        png.push_back(static_cast<std::uint8_t>(value >> 8));
        png.push_back(static_cast<std::uint8_t>(value));
    };
    const auto chunk = [&](const char* type, const std::vector<std::uint8_t>& data) {
        u32(static_cast<std::uint32_t>(data.size()));
        const auto start{ png.size() };
        png.insert(png.end(), type, type + 4);
        png.insert(png.end(), data.begin(), data.end());
        const auto crc{ crc32(0, png.data() + start, static_cast<uInt>(png.size() - start)) };
        u32(static_cast<std::uint32_t>(crc));
    };

    std::vector<std::uint8_t> header;
    for (const auto value : { static_cast<std::uint32_t>(image.width), static_cast<std::uint32_t>(image.height) }) {
        header.push_back(static_cast<std::uint8_t>(value >> 24));
        header.push_back(static_cast<std::uint8_t>(value >> 16));
        header.push_back(static_cast<std::uint8_t>(value >> 8));
        header.push_back(static_cast<std::uint8_t>(value));
    }
    header.insert(header.end(), { 8, 2, 0, 0, 0 });     // 8 bit RGB
    chunk("IHDR", header);
    chunk("IDAT", compressed);
    chunk("IEND", {});
    return png;
}

std::vector<std::uint8_t> render_png(const world::World& world, const Grid& grid, const RenderInput& input)
{
    return encode_png(render_image(world, grid, input));
}
}
