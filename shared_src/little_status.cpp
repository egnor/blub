#include "little_status.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>

#include <U8g2lib.h>

#include "ok_logging.h"

static const OkLoggingContext OK_CONTEXT("little_status");

namespace {

class LittleStatusDef : public LittleStatus {
 public:
  LittleStatusDef(u8g2_t* u8g2) : u8g2(u8g2) {
    drawn_y = u8g2->height;  // Erase any existing content
    rows_size = u8g2->height / 6 + 1;
    rows = new Row[rows_size];
    for (int i = 0; i < rows_size; ++i) rows[i].top_y = -1;
  }

  virtual ~LittleStatusDef() {
    for (int i = 0; i < rows_size; ++i) {
      delete[] rows[i].text;
    }
    delete[] rows;
    delete[] temp;
  }

  virtual void line_printf(int line, char const* format, ...) override {
    if (line < 0 || line >= rows_size) return;
    auto& row = rows[line];

    while (true) {
      va_list args;
      va_start(args, format);
      auto const len = vsnprintf(temp, temp_size, format, args);
      va_end(args);

      if (len >= temp_size) {
        delete[] temp;
        temp_size = len + 1;
        temp = new char[temp_size];
      } else if (len < 0) {
        OK_ERROR("Bad LittleStatus format (line %d): %s", line, format);
        return;
      } else {
        break;
      }
    }

    if (row.text == nullptr || strcmp(temp, row.text) != 0) {
      int const old_height = row.height;
      row.height = 0;
      row.baseline = 0;

      delete[] row.text;
      row.text = temp;
      temp = nullptr;
      temp_size = 0;

      TextChunk chunk;
      while (next_chunk(row.text, &chunk)) {
        row.height = std::max(row.height, chunk.height);
        uint8_t const* font = font_for_height(chunk.height, chunk.bold);
        if (font != nullptr && chunk.end > chunk.begin) {
          u8g2_SetFont(u8g2, font);
          int const ascent = u8g2->font_ref_ascent;
          row.baseline = std::max(row.baseline, ascent + 1);
        }
      }
      row.tabs = chunk.tabs;

      int bot_y = draw_one_line(rows[line]);
      if (row.height != old_height) {
        for (int i = line + 1; i < rows_size; ++i) {
          rows[i].top_y = bot_y;
          bot_y = draw_one_line(rows[i]);
        }
        if (bot_y < drawn_y) {
          int const blank_h = drawn_y - bot_y;
          u8g2_SetDrawColor(u8g2, 0);
          u8g2_DrawBox(u8g2, 0, bot_y, u8g2->width, blank_h);
          drawn_y = bot_y;
          bot_y += blank_h;
        }
      }

      push_rect(0, row.top_y, u8g2->width, bot_y - row.top_y);
    }
  }

  virtual u8g2_t* raw_driver() const override { return u8g2; }

 private:
  static constexpr int MIN_HEIGHT = 5;
  static constexpr int MAX_HEIGHT = 15;

  struct Row {
    int top_y = 0;
    int height = 0;
    int baseline = 0;
    int tabs = 0;
    char* text = nullptr;
  };

  struct TextChunk {
    int begin = 0, end = 0;
    int tabs = 0;
    int height = 8;
    bool bold = false;
    bool inverse = false;
  };

  u8g2_t* const u8g2;
  Row* rows = nullptr;
  int rows_size = 0;
  char* temp = nullptr;
  int temp_size = 0;
  int drawn_y = 0;

  int draw_one_line(Row const& row) {
    if (row.height == 0) return row.top_y;

    u8g2_SetFontMode(u8g2, 1);  // For _tr fonts
    u8g2_SetFontPosBaseline(u8g2);

    int text_x = -10;
    int const text_y = row.top_y + row.baseline;
    int last_tabs = -1;
    TextChunk chunk;
    while (next_chunk(row.text, &chunk)) {
      if (chunk.end <= chunk.begin) continue;
      uint8_t const* font = font_for_height(chunk.height, chunk.bold);
      if (font == nullptr) continue;

      u8g2_SetFont(u8g2, font);
      int const first_w = u8g2_GetGlyphWidth(u8g2, row.text[chunk.begin]);
      int const first_offset = u8g2->glyph_x_offset;

      int const prev_x = text_x;
      if (chunk.tabs > last_tabs) {
        int const tab_x = (chunk.tabs * u8g2->width) / (row.tabs + 1);
        text_x = std::max(text_x, tab_x - first_offset);
        last_tabs = chunk.tabs;
      }

      int end_x = text_x + first_w;
      for (int c = chunk.begin + 1; c < chunk.end; ++c) {
        char const ch = row.text[c];
        end_x += (ch <= 5) ? ch : u8g2_GetGlyphWidth(u8g2, ch);
      }

      u8g2_SetDrawColor(u8g2, chunk.inverse ? 1 : 0);
      u8g2_DrawBox(u8g2, prev_x, row.top_y, end_x - prev_x, row.height);

      u8g2_SetDrawColor(u8g2, chunk.inverse ? 0 : 1);
      for (int c = chunk.begin; c < chunk.end; ++c) {
        char const ch = row.text[c];
        text_x += (ch <= 5) ? ch : u8g2_DrawGlyph(u8g2, text_x, text_y, ch);
      }
    }

    if (text_x < u8g2->width) {
      u8g2_SetDrawColor(u8g2, chunk.inverse ? 1 : 0);
      u8g2_DrawBox(u8g2, text_x, row.top_y, u8g2->width - text_x, row.height);
    }

    int const bot_y = std::min<int>(row.top_y + row.height, u8g2->height);
    if (bot_y > drawn_y) drawn_y = bot_y;
    return bot_y;
  }

  static bool next_chunk(char const* text, TextChunk* chunk) {
    chunk->begin = chunk->end;
    for (;;) {
      for (chunk->end = chunk->begin; text[chunk->end] != '\0'; ++chunk->end) {
        if (text[chunk->end] < 0x20 && text[chunk->end] > 5) break;
      }
      if (chunk->end > chunk->begin) return true;
      if (text[chunk->end] == '\0') return false;

      switch (text[chunk->begin++]) {
        case '\b':
          chunk->bold = !chunk->bold;
          break;
        case '\f':
          if (text[chunk->begin] >= '0' && text[chunk->begin] <= '9') {
            chunk->height = text[chunk->begin++] - '0';
            if (chunk->height * 10 <= MAX_HEIGHT &&
                text[chunk->begin] >= '0' && text[chunk->begin] <= '9') {
              chunk->height = chunk->height * 10 + (text[chunk->begin++] - '0');
            }
            chunk->height = std::min(chunk->height, MAX_HEIGHT);
          }
          break;
        case '\t':
          ++chunk->tabs;
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

  static uint8_t const* font_for_height(int height, bool bold) {
    if (height < MIN_HEIGHT) return nullptr;
    switch (height) {
      case 5: return u8g2_font_3x3basic_tr;
      case 6: return u8g2_font_u8glib_4_tr;
      case 7: return bold ? u8g2_font_wedge_tr : u8g2_font_tiny5_tr;
      case 8: return bold ? u8g2_font_squeezed_b6_tr
                          : u8g2_font_squeezed_r6_tr;
      case 9:
      case 10: return bold ? u8g2_font_NokiaSmallBold_tr
                           : u8g2_font_NokiaSmallPlain_tr;
      case 11: return bold ? u8g2_font_helvB08_tr : u8g2_font_helvR08_tr;
      case 12: return bold ? u8g2_font_crox1hb_tr : u8g2_font_crox1h_tr;
      case 13:
      case 14: return bold ? u8g2_font_crox2hb_tr : u8g2_font_crox2h_tr;
      default: return bold ? u8g2_font_helvB10_tr : u8g2_font_helvR10_tr;
    }
  }

  void push_rect(int x, int y, int w, int h) {
    // TODO: account for rotation if any
    int const tx = std::min<int>(x, u8g2->width) / 8;
    int const ty = std::min<int>(y, u8g2->height) / 8;
    int const tw = std::min<int>(x + w + 7, u8g2->width) / 8 - tx;
    int const th = std::min<int>(y + h + 7, u8g2->height) / 8 - ty;
    u8g2_UpdateDisplayArea(u8g2, tx, ty, tw, th);
  }
};

}  // namespace

LittleStatus* make_little_status(u8g2_t* driver) {
  OK_FATAL_IF(driver == nullptr);
  return new LittleStatusDef(driver);
}
