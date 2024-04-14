#include "little_status.h"

#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>

#include <U8g2lib.h>

namespace {

struct status_line {
  int size = 0;
  std::string text;
};

class LittleStatusDef : public LittleStatus {

 public:
  virtual ~LittleStatusDef() {}

  LittleStatusDef(std::unique_ptr<U8G2> drv)
    : driver(std::move(drv)),
      screen_w(driver->getDisplayWidth()),
      screen_h(driver->getDisplayHeight()) {
    drawn_y = driver->getDisplayHeight();  // Erase any existing content
    lines.resize((drawn_y / 6) + 1);

    int const line_max = (screen_w / 2) + 1;
    temp_text.reserve(line_max);
    for (auto& init : lines) init.text.reserve(line_max);
  }

  virtual void line_printf(int line, char const* format, ...) override {
    if (line < 0 || line >= lines.size()) return;
    auto& update = lines[line];

    va_list args;
    va_start(args, format);
    temp_text.resize(temp_text.capacity());
    vsnprintf(temp_text.data(), temp_text.capacity(), format, args);
    temp_text.resize(strlen(temp_text.data()));
    va_end(args);

    if (temp_text != update.text) {
      update.text = temp_text;

      int top_y = 0;
      for (int i = 0; i < line; ++i) top_y += lines[i].size;
      int const bot_y = draw_one_line(line, top_y);
      push_rect(0, top_y, screen_w, bot_y - top_y);
    }
  }

  virtual void set_line_size(int line, int size) override {
    if (line < 0 || line >= lines.size()) return;
    auto& update = lines[line];

    if (size <= 0) {
      size = 0;
    } else if (size < MIN_SIZE) {
      size = MIN_SIZE;
    } else if (size > MAX_SIZE) {
      size = MAX_SIZE;
    }

    if (size != update.size) {
      update.size = size;

      int top_y = 0;
      for (int i = 0; i < line; ++i) top_y += lines[i].size;

      int bot_y = top_y;
      for (int i = line; i < lines.size(); ++i) {
        bot_y = draw_one_line(i, bot_y);
      }

      if (bot_y < drawn_y) {
        int const blank_h = drawn_y - bot_y;
        driver->setDrawColor(0);
        driver->drawBox(0, bot_y, screen_w, blank_h);
        drawn_y = bot_y;
        bot_y += blank_h;
      }

      push_rect(0, top_y, screen_w, bot_y - top_y);
    }
  }

  virtual U8G2* raw_driver() const override { return driver.get(); }

 private:
  std::unique_ptr<U8G2> const driver;
  int const screen_w, screen_h;

  std::vector<status_line> lines;
  std::string temp_text;
  int drawn_y = 0;

  int draw_one_line(int line, int top_y) {
    if (line < 0 || line >= lines.size()) return top_y;
    auto const& refresh = lines[line];
    if (refresh.size < MIN_SIZE) return top_y;

    if (drawn_y > top_y) {
      int const clear_to_y = std::min(top_y + refresh.size, drawn_y);
      driver->setDrawColor(0);
      driver->drawBox(0, top_y, screen_w, clear_to_y - top_y);
    }

    if (refresh.text.empty()) return top_y + refresh.size;

    uint8_t const* font;
    bool const bold = false;  // TODO: Implement bold
    switch (refresh.size) {
      case 6: font = u8g2_font_u8glib_4_tr;  // No bold at this size
      case 7: font = bold ? u8g2_font_wedge_tr : u8g2_font_tinypixie2_tr; break;
      case 8: font = bold ? u8g2_font_squeezed_b6_tr
                          : u8g2_font_squeezed_r6_tr; break;
      case 9:
      case 10: font = bold ? u8g2_font_NokiaSmallBold_tr
                           : u8g2_font_NokiaSmallPlain_tr; break;
      case 11: font = bold ? u8g2_font_helvB08_tr : u8g2_font_helvR08_tr; break;
      case 12: font = bold ? u8g2_font_t0_11b_tr : u8g2_font_t0_11_tr; break;
      case 13:
      case 14: font = bold ? u8g2_font_t0_13b_tr : u8g2_font_t0_13_tr; break;
      default: font = bold ? u8g2_font_helvB10_tr : u8g2_font_helvR10_tr; break;
    }

    driver->setDrawColor(1);
    driver->setFont(font);
    driver->setFontMode(1);  // For _tr fonts
    driver->setFontPosBaseline();

    // https://github.com/olikraus/u8g2/issues/2299
    auto* const u8g2 = driver->getU8g2();
    int const x_offset = u8g2_GetStrX(u8g2, refresh.text.c_str());
    int const baseline_y = top_y + u8g2->font_ref_ascent;
    driver->drawStr(-x_offset, baseline_y, refresh.text.c_str());

    int const bot_y = std::min(top_y + refresh.size, screen_h);
    if (bot_y > drawn_y) drawn_y = bot_y;
    return bot_y;
  }

  void push_rect(int x, int y, int w, int h) {
    // TODO: account for rotation if any
    int const tx = std::min(x, screen_w) / 8;
    int const ty = std::min(y, screen_h) / 8;
    int const tw = std::min(x + w + 7, screen_w) / 8 - tx;
    int const th = std::min(y + h + 7, screen_h) / 8 - ty;
    driver->updateDisplayArea(tx, ty, tw, th);
  }
};

}  // namespace

std::unique_ptr<LittleStatus> make_little_status(std::unique_ptr<U8G2> disp) {
  return std::make_unique<LittleStatusDef>(std::move(disp));
}
