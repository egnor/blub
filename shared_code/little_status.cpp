#include "little_status.h"

#include <algorithm>
#include <cstdarg>
#include <string>
#include <vector>

#include <U8g2lib.h>

namespace {

class LittleStatusDef : public LittleStatus {
 public:
  virtual ~LittleStatusDef() {}

  LittleStatusDef(std::unique_ptr<U8G2> drv)
    : driver(std::move(drv)),
      screen_w(driver->getDisplayWidth()),
      screen_h(driver->getDisplayHeight()) {
    drawn_y = driver->getDisplayHeight();  // Erase any existing content
    rows.resize((drawn_y / 6) + 1);

    int const line_max = (screen_w / 2) + 1;
    temp_text.reserve(line_max);
    for (auto& row : rows) row.text.reserve(line_max);
  }

  virtual void line_printf(int line, char const* format, ...) override {
    if (line < 0 || line >= rows.size()) return;
    auto& row = rows[line];

    va_list args;
    va_start(args, format);
    temp_text.resize(temp_text.capacity());
    vsnprintf(temp_text.data(), temp_text.capacity(), format, args);
    temp_text.resize(strlen(temp_text.data()));
    va_end(args);

    if (temp_text != row.text) {
      int const old_size = row.max_size;
      row.text = temp_text;
      row.max_size = 0;
      row.baseline = 0;
      text_chunk chunk;
      while (next_chunk(row.text, &chunk)) {
        uint8_t const* font = font_for_size(chunk.size, chunk.bold);
        if (font != nullptr && chunk.end > chunk.begin) {
          driver->setFont(font);
          int const ascent = driver->getU8g2()->font_ref_ascent;
          row.max_size = std::max(row.max_size, chunk.size);
          row.baseline = std::max(row.baseline, ascent + 1);
        }
      }

      int top_y = -1;
      for (int i = 0; i < line; ++i) top_y += rows[i].max_size;
      int bot_y = draw_one_line(line, top_y);
      if (row.max_size != old_size) {
        for (int i = line + 1; i < rows.size(); ++i) {
          bot_y = draw_one_line(i, bot_y);
        }
        if (bot_y < drawn_y) {
          int const blank_h = drawn_y - bot_y;
          driver->setDrawColor(0);
          driver->drawBox(0, bot_y, screen_w, blank_h);
          drawn_y = bot_y;
          bot_y += blank_h;
        }
      }

      push_rect(0, top_y, screen_w, bot_y - top_y);
    }
  }

  virtual U8G2* raw_driver() const override { return driver.get(); }

 private:
  struct row {
    int max_size = 0;
    int baseline = 0;
    std::string text;
  };

  struct text_chunk {
    int begin = 0, end = 0;
    int column = 0;
    int size = 8;
    bool bold = false;
    bool inverse = false;
  };

  std::unique_ptr<U8G2> const driver;
  int const screen_w, screen_h;

  std::vector<row> rows;
  std::string temp_text;
  int drawn_y = 0;

  int draw_one_line(int line, int top_y) {
    if (line < 0 || line >= rows.size()) return top_y;
    auto const& row = rows[line];

    driver->setDrawColor(1);
    driver->setFontMode(1);  // For _tr fonts
    driver->setFontPosBaseline();

    auto* const u8g2 = driver->getU8g2();
    int text_x = -1;
    int const text_y = top_y + row.baseline;
    text_chunk chunk;
    while (next_chunk(row.text, &chunk)) {
      uint8_t const* font = font_for_size(chunk.size, chunk.bold);
      if (font != nullptr && chunk.end > chunk.begin) {
        driver->setFont(font);
        int chunk_w = u8g2_GetGlyphWidth(u8g2, row.text[chunk.begin]);
        if (text_x < 0) text_x = -u8g2->glyph_x_offset;
        for (int c = chunk.begin + 1; c < chunk.end; ++c) {
          chunk_w += u8g2_GetGlyphWidth(u8g2, row.text[c]);
        }

        driver->setDrawColor(chunk.inverse ? 1 : 0);
        driver->drawBox(text_x, top_y, chunk_w, row.max_size);

        driver->setDrawColor(chunk.inverse ? 0 : 1);
        for (int c = chunk.begin; c < chunk.end; ++c) {
          text_x += driver->drawGlyph(text_x, text_y, row.text[c]);
        }
      }
    }

    if (text_x < screen_w) {
      driver->setDrawColor(chunk.inverse ? 1 : 0);
      driver->drawBox(text_x, top_y, screen_w - text_x, row.max_size);
    }

    int const bot_y = std::min(top_y + row.max_size, screen_h);
    if (bot_y > drawn_y) drawn_y = bot_y;
    return bot_y;
  }

  static bool next_chunk(std::string const& text, text_chunk* chunk) {
    int const size = text.size();
    chunk->begin = chunk->end;
    for (;;) {
      if (chunk->begin >= size) {
        chunk->begin = chunk->end = size;
        return false;
      }

      if (text[chunk->begin] >= 0x20) {
        chunk->end = chunk->begin + 1;
        while (chunk->end < size && text[chunk->end] >= 0x20) ++chunk->end;
        return true;
      }

      switch (text[chunk->begin++]) {
        case '\b':
          chunk->bold = !chunk->bold;
          break;
        case '\f':
          if (chunk->begin < size &&
              text[chunk->begin] >= '0' && text[chunk->begin] <= '9') {
            chunk->size = text[chunk->begin++] - '0';
            if (chunk->size < MIN_SIZE && chunk->begin < size &&
                text[chunk->begin] >= '0' && text[chunk->begin] <= '9') {
              chunk->size = chunk->size * 10 + (text[chunk->begin++] - '0');
            }
            if (chunk->size != 0) {
              chunk->size = std::max(chunk->size, MIN_SIZE);
              chunk->size = std::min(chunk->size, MAX_SIZE);
            }
          }
          break;
        case '\t':
          ++chunk->column;
          break;
        case '\v':
          chunk->inverse = !chunk->inverse;
          break;
        default:
          chunk->end = chunk->begin + 1;
          return true;
      }
    }
  }

  static uint8_t const* font_for_size(int size, bool bold) {
    if (size < MIN_SIZE) return nullptr;
    switch (size) {
      case 5: return u8g2_font_3x3basic_tr;
      case 6: return u8g2_font_u8glib_4_tr;
      case 7: return bold ? u8g2_font_wedge_tr : u8g2_font_tiny5_tr;
      case 8: return bold ? u8g2_font_squeezed_b6_tr : u8g2_font_5x7_tr;
      case 9:
      case 10: return bold ? u8g2_font_NokiaSmallBold_tr
                           : u8g2_font_NokiaSmallPlain_tr;
      case 11: return bold ? u8g2_font_helvB08_tr : u8g2_font_helvR08_tr;
      case 12: return bold ? u8g2_font_t0_11b_tr : u8g2_font_t0_11_tr;
      case 13:
      case 14: return bold ? u8g2_font_t0_13b_tr : u8g2_font_t0_13_tr;
      default: return bold ? u8g2_font_helvB10_tr : u8g2_font_helvR10_tr;
    }
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
