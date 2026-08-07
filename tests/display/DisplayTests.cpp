#include "aw/display/Framebuffer.hpp"
#include "aw/display/Painter.hpp"
#include "framework/Test.hpp"

#include <algorithm>
#include <array>

using namespace aw;

AW_TEST(Framebuffer, StartsFullyDamaged) {
    display::Framebuffer framebuffer{320, 240};
    AW_REQUIRE(framebuffer.hasDamage());
    AW_CHECK_EQ(framebuffer.damage().width, std::int16_t{320});
    AW_CHECK_EQ(framebuffer.damage().height, std::int16_t{240});
}

AW_TEST(Framebuffer, DamageIsTheUnionOfChanges) {
    display::Framebuffer framebuffer{320, 240};
    framebuffer.clearDamage();
    AW_REQUIRE(!framebuffer.hasDamage());

    framebuffer.setPixel(10, 10, 0xFFFF);
    framebuffer.setPixel(20, 30, 0xFFFF);

    const hal::Rect damage = framebuffer.damage();
    AW_CHECK_EQ(damage.x, std::int16_t{10});
    AW_CHECK_EQ(damage.y, std::int16_t{10});
    AW_CHECK_EQ(damage.width, std::int16_t{11});   // 10..20 inclusive
    AW_CHECK_EQ(damage.height, std::int16_t{21});  // 10..30 inclusive
}

AW_TEST(Framebuffer, ClipsWritesAndDamageToItsBounds) {
    display::Framebuffer framebuffer{32, 32};
    framebuffer.clearDamage();

    framebuffer.setPixel(-5, 5, 0xFFFF);   // Off the left edge.
    framebuffer.setPixel(100, 5, 0xFFFF);  // Off the right edge.
    AW_CHECK(!framebuffer.hasDamage());    // Nothing was written, nothing damaged.

    framebuffer.markDamaged(hal::Rect{-10, -10, 100, 100});
    const hal::Rect damage = framebuffer.damage();
    AW_CHECK_EQ(damage.x, std::int16_t{0});
    AW_CHECK_EQ(damage.y, std::int16_t{0});
    AW_CHECK_EQ(damage.width, std::int16_t{32});
    AW_CHECK_EQ(damage.height, std::int16_t{32});
}

AW_TEST(Framebuffer, ExtractsThePackedDamagedRegion) {
    display::Framebuffer framebuffer{8, 8};
    framebuffer.fill(0x0000);
    framebuffer.clearDamage();

    framebuffer.setPixel(2, 2, 0x1234);
    framebuffer.setPixel(3, 3, 0x5678);

    std::vector<display::Color> packed(64, 0xAAAA);
    const hal::Rect area = framebuffer.extractDamage(packed);

    AW_CHECK_EQ(area.width, std::int16_t{2});
    AW_CHECK_EQ(area.height, std::int16_t{2});
    AW_CHECK_EQ(packed[0], display::Color{0x1234});  // (2,2)
    AW_CHECK_EQ(packed[3], display::Color{0x5678});  // (3,3)
}

AW_TEST(Painter, FillRectWritesOnlyInsideTheRectangle) {
    display::Framebuffer framebuffer{16, 16};
    framebuffer.fill(0x0000);
    framebuffer.clearDamage();

    display::Painter painter{framebuffer};
    painter.fillRect(hal::Rect{4, 4, 3, 2}, 0x00FF);

    const std::span<const display::Color> pixels = framebuffer.pixels();
    AW_CHECK_EQ(pixels[4 * 16 + 4], display::Color{0x00FF});
    AW_CHECK_EQ(pixels[5 * 16 + 6], display::Color{0x00FF});
    AW_CHECK_EQ(pixels[6 * 16 + 4], display::Color{0x0000});  // One row below.
    AW_CHECK_EQ(pixels[4 * 16 + 7], display::Color{0x0000});  // One column right.

    const hal::Rect damage = framebuffer.damage();
    AW_CHECK_EQ(damage.x, std::int16_t{4});
    AW_CHECK_EQ(damage.width, std::int16_t{3});
    AW_CHECK_EQ(damage.height, std::int16_t{2});
}

AW_TEST(Painter, ClipsRectanglesThatCrossTheEdge) {
    display::Framebuffer framebuffer{16, 16};
    display::Painter painter{framebuffer};

    // Must not write out of bounds; the test would crash under ASan if it did.
    painter.fillRect(hal::Rect{-4, -4, 8, 8}, 0x00FF);
    painter.fillRect(hal::Rect{12, 12, 8, 8}, 0x00FF);

    const std::span<const display::Color> pixels = framebuffer.pixels();
    AW_CHECK_EQ(pixels[0], display::Color{0x00FF});
    AW_CHECK_EQ(pixels[15 * 16 + 15], display::Color{0x00FF});
}

AW_TEST(Painter, BitmapTextDrawsVisiblePixels) {
    display::Framebuffer framebuffer{32, 16};
    framebuffer.fill(display::palette::kBackground);
    display::Painter painter{framebuffer};
    const auto width = painter.drawText(1, 1, "A1", display::smallFont(), display::palette::kText);
    AW_CHECK_EQ(width, std::int16_t{12});
    AW_CHECK(std::any_of(framebuffer.pixels().begin(), framebuffer.pixels().end(),
                         [](display::Color pixel) { return pixel == display::palette::kText; }));
}

AW_TEST(Painter, WaveformAndMeterDrawInsideTheirAreas) {
    display::Framebuffer framebuffer{32, 16};
    framebuffer.fill(display::palette::kBackground);
    display::Painter painter{framebuffer};
    const std::array<std::pair<float, float>, 2> peaks{{{-1.0F, 1.0F}, {-0.5F, 0.5F}}};
    painter.drawWaveform({0, 0, 16, 16}, peaks, display::palette::kWaveform);
    painter.drawMeter({18, 4, 14, 6}, 0.5F, false);
    AW_CHECK_EQ(framebuffer.pixels()[0], display::palette::kWaveform);
    AW_CHECK_EQ(framebuffer.pixels()[5 * 32 + 19], display::palette::kMeterOk);
}

AW_TEST(Color, PacksIntoRgb565) {
    AW_CHECK_EQ(display::rgb(0, 0, 0), display::Color{0x0000});
    AW_CHECK_EQ(display::rgb(255, 255, 255), display::Color{0xFFFF});
    AW_CHECK_EQ(display::rgb(255, 0, 0), display::Color{0xF800});
    AW_CHECK_EQ(display::rgb(0, 255, 0), display::Color{0x07E0});
    AW_CHECK_EQ(display::rgb(0, 0, 255), display::Color{0x001F});
}
