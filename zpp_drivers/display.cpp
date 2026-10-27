// Copyright 2025 Haute école d'ingénierie et d'architecture de Fribourg
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/****************************************************************************
 * @file digital_out.cpp
 * @author Serge Ayer <serge.ayer@hefr.ch>
 *
 * @brief CPP class implementation for wrapping zephyr os display
 *
 * @date 2025-08-31
 * @version 1.0.0
 ***************************************************************************/

#if CONFIG_DISPLAY

#include "zpp_include/display.hpp"

// Zephyr

// zpp_lib
#include "zpp_include/zpp_assert.hpp"
#include "zpp_include/zpp_log.hpp"

ZPP_LOG_MODULE_DECLARE(zpp_drivers, CONFIG_ZPP_DRIVERS_LOG_LEVEL);

namespace zpp_lib {

// This is a Zephyr macro that we have no control over
#if CONFIG_QEMU_TARGET
static constexpr uint16_t kHeapSize = 4096;
#else   // CONFIG_QEMU_TARGET
// when using the physical display, the line buffer is 960 bytes,
// so we can reduce the heap size to save memory
static constexpr uint16_t kHeapSize = 2048;
#endif  // CONFIG_QEMU_TARGET
// NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-array-to-pointer-decay,cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays,readability-math-missing-parentheses,cppcoreguidelines-avoid-non-const-global-variables)
K_HEAP_DEFINE(buf_heap, kHeapSize);

// we define our own min
// NOLINTNEXTLINE(build/include_what_you_use)
uint16_t min(uint32_t a, uint16_t b) {
  return (b < a) ? b : static_cast<uint16_t>(a);
}

// Returns the char width or 0
inline uint8_t get_char_width(const Display::Font* pFont, uint32_t unicode) {
  ZPP_ASSERT(pFont != nullptr, "Font not set");
  for (uint32_t i = 0; i < pFont->char_count; i++) {
    if (pFont->chars[i].unicode == unicode) {
      return pFont->chars[i].width;
    }
  }
  return 0;
}

// Lookup a character's bitmap by Unicode code point
// Returns nullptr if the character is not in the font
inline const uint8_t* font_get_glyph(const Display::Font* p_font, uint32_t unicode, uint8_t& char_width) {
  ZPP_ASSERT(p_font != nullptr, "Font not set");
  for (uint32_t i = 0; i < p_font->char_count; i++) {
    if (p_font->chars[i].unicode == unicode) {
      char_width = p_font->chars[i].width;
      return p_font->table + p_font->chars[i].offset;
    }
  }
  return nullptr;
}

ZephyrResult Display::initialize() {
  ZephyrResult res;
  _display_device = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
  // device_is_ready() is a Zephyr macro
  // NOLINTNEXTLINE(bugprone-assignment-in-if-condition)
  if (!device_is_ready(_display_device)) {
    ZPP_LOG_ERR("Device %s not found", _display_device->name);
    res.assign_error(zpp_lib::ZephyrErrorCode::Nodev);
    return res;
  }

  ZPP_LOG_DBG("Display sample for %s", _display_device->name);
  display_get_capabilities(_display_device, &_display_capabilities);

  // Wrapper adapts to whatever driver reports (rotated or not)
  if (_display_capabilities.current_orientation == DISPLAY_ORIENTATION_ROTATED_90 ||
      _display_capabilities.current_orientation == DISPLAY_ORIENTATION_ROTATED_270) {
    _lcd_xsize = _display_capabilities.y_resolution;
    _lcd_ysize = _display_capabilities.x_resolution;
  } else {
    _lcd_xsize = _display_capabilities.x_resolution;
    _lcd_ysize = _display_capabilities.y_resolution;
  }

  // Use max dimension to accommodate both portrait and landscape rotations
  _line_buffer_size = (_lcd_xsize > _lcd_ysize) ? _lcd_xsize : _lcd_ysize;

  switch (_display_capabilities.current_pixel_format) {
  case PIXEL_FORMAT_ARGB_8888: {
    _fill_color_function = Display::fill_color_argb8888;
    _fill_line_function  = Display::fill_line_argb8888;
    _line_buffer_size *= 4;
  } break;

  case PIXEL_FORMAT_RGB_888: {
    _fill_color_function = Display::fill_color_rgb888;
    _fill_line_function  = Display::fill_line_rgb888;
    _line_buffer_size *= 3;
  } break;

  default:
    ZPP_LOG_ERR("Unsupported pixel format. Aborting sample.");
    res.assign_error(zpp_lib::ZephyrErrorCode::Nosys);
    return res;
  }

  k_timeout_t timeout = {0};
  ZPP_LOG_DBG("line buffer size %d\n", _line_buffer_size);
  _line_buffer = static_cast<uint8_t*>(k_heap_alloc(&buf_heap, _line_buffer_size, timeout));
  if (_line_buffer == nullptr) {
    ZPP_LOG_ERR("Could not allocate memory of size %d. Aborting sample.", _line_buffer_size);
    res.assign_error(zpp_lib::ZephyrErrorCode::Nomem);
    return res;
  }

  ZPP_LOG_DBG("Display capabilities: x_res %d y_res %d pixel_format %d",
              _display_capabilities.x_resolution,
              _display_capabilities.y_resolution,
              _display_capabilities.current_pixel_format);
  ZPP_LOG_DBG("Display size: x %d, y %d", _lcd_xsize, _lcd_ysize);
  ZPP_LOG_DBG("Buffer size: %d", _line_buffer_size);

  return res;
}

uint32_t Display::get_width() const {
  return _lcd_xsize;
}

uint32_t Display::get_height() const {
  return _lcd_ysize;
}

void Display::fill_display(Rgb color) {
  fill_rectangle(color, 0, 0, _lcd_xsize, _lcd_ysize);
  display_blanking_off(_display_device);
}

void Display::set_background_color(Rgb color) {
  _background_color_value = color.argb();
}

// width, height is the standard order for image
// dimensions, and we always call this function with width first, then height
void Display::fill_rectangle(Rgb color,
                             uint32_t x_pos,
                             uint32_t y_pos,  // NOLINT(bugprone-easily-swappable-parameters)
                             uint32_t width,  // NOLINT(bugprone-easily-swappable-parameters)
                             uint32_t height) {
  uint32_t color_value = color.argb();
  // ZPP_LOG_DBG("Fill rectangle with color 0x%08x starting at (%d, %d) - (width %d,
  // height %d)",
  //        color_value,
  //        x_pos,
  //        y_pos,
  //        width,
  //        height);
  _fill_color_function(color_value, _line_buffer, _line_buffer_size);

  struct display_buffer_descriptor buf_desc = {.buf_size = 0, .width = 0, .height = 0, .pitch = 0, .frame_incomplete = false};
  x_pos                                     = zpp_lib::min(_lcd_xsize - 1, x_pos);
  buf_desc.width                            = zpp_lib::min(_lcd_xsize - x_pos, width);
  buf_desc.pitch                            = buf_desc.width;
  buf_desc.height                           = kHStep;
  buf_desc.frame_incomplete                 = true;
  // buf_size is in bytes: width * height * bytes_per_pixel
  uint32_t bytes_per_pixel = (_display_capabilities.current_pixel_format == PIXEL_FORMAT_ARGB_8888) ? 4U : 3U;
  buf_desc.buf_size        = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
  uint16_t first_line      = zpp_lib::min(y_pos, _lcd_ysize);
  uint16_t last_line       = zpp_lib::min(y_pos + height, _lcd_ysize);
  for (uint32_t line = first_line; line < last_line; line += kHStep) {
    uint16_t line_height      = zpp_lib::min(kHStep, last_line - line);
    buf_desc.height           = line_height;
    buf_desc.buf_size         = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
    buf_desc.frame_incomplete = (line + line_height < last_line);
    auto ret                  = display_write(_display_device, x_pos, line, &buf_desc, _line_buffer);
    if (ret != 0) {
      ZPP_LOG_ERR("Cannot write to display: %d", ret);
    }
  }
}

void Display::draw_vertical_line(Rgb color, uint32_t x_pos, uint32_t y_pos, uint32_t line_length, uint32_t thickness) {
  // ZPP_LOG_DBG("Fill rectangle with color 0x%08x starting at (%d, %d) - (width %d,
  // height %d)",
  //        static_cast<uint32_t>(color),
  //        x_pos,
  //        y_pos,
  //        thickness,
  //        line_width);
  // NOLINTNEXTLINE(readability-suspicious-call-argument) - vertical line,
  // thickness/line_length is the width/height of the rectangle
  fill_rectangle(color, x_pos, y_pos, thickness, line_length);
}

void Display::draw_horizontal_line(Rgb color, uint32_t x_pos, uint32_t y_pos, uint32_t line_length, uint32_t thickness) {
  // ZPP_LOG_DBG("Fill rectangle with color 0x%08x starting at (%d, %d) - (width %d,
  // height %d)",
  //        static_cast<uint32_t>(color),
  //        x_pos,
  //        y_pos,
  //        line_width,
  //        thickness);
  fill_rectangle(color, x_pos, y_pos, line_length, thickness);
}

void Display::draw_icon(uint16_t x_pos,
                        uint16_t y_pos,
                        const uint32_t* p_src,
                        uint16_t logo_width,  // NOLINT(bugprone-easily-swappable-parameters) - width, height
                                              // is the standard order for image dimensions, and we always
                                              // call this function with width first, then height
                        uint16_t logo_height) {
  struct display_buffer_descriptor buf_desc = {.buf_size = 0, .width = 0, .height = 0, .pitch = 0, .frame_incomplete = false};
  x_pos                                     = zpp_lib::min(_lcd_xsize - 1, x_pos);
  buf_desc.width                            = zpp_lib::min(_lcd_xsize - x_pos, logo_width);
  buf_desc.pitch                            = buf_desc.width;
  buf_desc.height                           = kHStep;
  buf_desc.frame_incomplete                 = true;
  uint32_t bytes_per_pixel                  = (_display_capabilities.current_pixel_format == PIXEL_FORMAT_ARGB_8888) ? 4U : 3U;
  buf_desc.buf_size                         = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
  // draw the picture line by line
  uint16_t first_line = zpp_lib::min(y_pos, _lcd_ysize);
  uint16_t last_line  = zpp_lib::min(y_pos + logo_height, _lcd_ysize);
  for (uint32_t line = first_line; line < last_line; line += kHStep) {
    // fill only the number of pixels we will write (buf_desc.width)
    _fill_line_function(p_src, buf_desc.width, _line_buffer);
    uint16_t line_height      = zpp_lib::min(kHStep, last_line - line);
    buf_desc.height           = line_height;
    buf_desc.buf_size         = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
    buf_desc.frame_incomplete = (line + line_height < last_line);
    auto ret                  = display_write(_display_device, x_pos, line, &buf_desc, _line_buffer);
    if (ret != 0) {
      ZPP_LOG_ERR("Cannot write to display: %d", ret);
    }
    p_src += logo_width;
  }
}

void Display::set_font(const Font* p_font) {
  _p_font = p_font;
}

const Display::Font* Display::get_font() const {
  return _p_font;
}

uint16_t Display::get_string_width(const std::string& text) const {
  ZPP_ASSERT(_p_font != nullptr, "Font not set");
  uint16_t string_width = 0;
  for (const char& c : text) {
    string_width += get_char_width(_p_font, c);
  }
  return string_width;
}

void Display::draw_string_at_line(Rgb color, uint32_t line, const std::string& text, AlignMode mode) {
  uint16_t nbr_of_pixels = get_string_width(text);
  uint32_t x_pos         = 0;
  switch (mode) {
  case AlignMode::Center: {
    x_pos = (get_width() / 2) - (nbr_of_pixels / 2);
  } break;

  case AlignMode::Left: {
    x_pos = 0;
  } break;

  case AlignMode::Right: {
    x_pos = get_width() - nbr_of_pixels;
  } break;

  default:
    break;
  }
  draw_string_at(color, x_pos, compute_ypos_from_line_number(line), text);
}

// NOLINTNEXTLINE(build/include_what_you_use) - false positive (string is included in the header)
void Display::draw_string_at(Rgb color, uint32_t x_pos, uint32_t y_pos, const std::string& text) {
  ZPP_ASSERT(_p_font != nullptr, "Font not set");
  const uint32_t color_value = color.argb();

  // compute the starting column
  uint32_t refcolumn = x_pos;

  // get the number of pixels required for displaying the text
  // uint16_t nbrOfPixels = getStringWidth(text);
  // ZPP_LOG_DBG("Drawing %s at pos %d - %d (#pixels = %d, refcolumn %d)", text, x_pos,
  // y_pos, nbrOfPixels, refcolumn);

  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
  // Hardcoding values is acceptable here
  // Check that the Start column is located in the screenascii
  if ((refcolumn < 1) || (refcolumn >= 0x8000)) {
    refcolumn = 1;
  }
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)

  // Send the string character by character on display
  uint32_t total_width = refcolumn;
  for (unsigned char c : text) {
    uint16_t char_width = get_char_width(_p_font, c);
    total_width += char_width;
    if (total_width > _lcd_xsize) {
      break;
    }

    // Display one character on display
    display_char(color_value, refcolumn, y_pos, c);
    // Increment the column position by the width
    refcolumn += char_width;
  }
}

uint32_t Display::compute_ypos_from_line_number(uint32_t line) const {
  return line * get_font()->height;
}

void Display::display_char(uint32_t color_value,
                           uint32_t x_pos,  // This is a private method and we always call it correctly
                           uint32_t y_pos,  // NOLINT(bugprone-easily-swappable-parameters)
                           uint32_t unicode) {
  ZPP_ASSERT(_p_font != nullptr, "Font not set");
  uint8_t char_width    = 0;
  const uint8_t* p_data = font_get_glyph(_p_font, unicode, char_width);
  if (p_data == nullptr) {
    ZPP_LOG_ERR("Character %d not found in font", unicode);
    return;
  }
  static constexpr uint32_t kMaxCharWidth = 48;
  ZPP_ASSERT(char_width < kMaxCharWidth, "Invalid char width: %d", char_width);
  // ZPP_LOG_DBG("Displaying char %c at %d - %d", unicode, x_pos, y_pos);

  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
  // Hardcoding values is acceptable here

  // compute the bit offset in each line
  // nbrOfBytesPerLine represents the number of bytes
  // stored for each line of the character font
  uint32_t offset                = (8 * ((char_width + 7) / 8)) - char_width;
  uint32_t nbr_of_bytes_per_line = (char_width + 7) / 8;

  // draw each line of the char stored in table
  for (uint32_t i = 0; i < _p_font->height; i++) {
    // get the start address of the line
    const uint8_t* p_char = p_data + (nbr_of_bytes_per_line * i);

    // line represents the ith line of the character (as a uint64_t)
    uint64_t line = 0;
    for (uint32_t j = 0; j < nbr_of_bytes_per_line; j++) {
      line <<= 8;
      line |= p_char[j];
    }

    if (_display_capabilities.current_pixel_format == PIXEL_FORMAT_ARGB_8888 ||
        _display_capabilities.current_pixel_format == PIXEL_FORMAT_RGB_888) {
      // We need a fixed-size array for the line buffer, and char_width is asserted
      // to be < kMaxCharWidth
      // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays)
      uint32_t argb8888[kMaxCharWidth] = {0};
      for (uint32_t j = 0; j < char_width; j++) {
        // check whether the j^th bit in line is on or off
        uint64_t bit_in_pixel = static_cast<uint64_t>(1) << static_cast<uint64_t>(char_width - j + offset - 1);
        if (static_cast<bool>(line & bit_in_pixel)) {
          // char_width is asserted to be < 48, so j is always < 4
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index) -
          argb8888[j] = color_value;
        } else {
          // char_width is asserted to be < 48, so j is always < 4
          // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
          argb8888[j] = _background_color_value;
        }
      }
      fill_rgb_rect(x_pos, y_pos++, argb8888, char_width, 1);  // NOLINT
    } else {
      ZPP_LOG_ERR("Not implemented");
      ZPP_ASSERT(false, "Not implemented");
    }
  }
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
}

void Display::fill_color_argb8888(uint32_t color_value, uint8_t* p_buffer, size_t buffer_size) {
  // ZPP_LOG_INF("Filling buffer of size %d with value %d", buf_size, colorValue);
  for (size_t idx = 0; idx < buffer_size; idx += 4) {
    // static_cast<uint32_t*> is not accepted here, reinterpret_cast is not supported
    // NOLINTNEXTLINE(readability/casting, modernize-avoid-c-style-cast, cppcoreguidelines-pro-type-cstyle-cast)
    *((uint32_t*)(p_buffer + idx)) = color_value;
  }
}
void Display::fill_color_rgb888(uint32_t color_value, uint8_t* p_buffer, size_t buffer_size) {
  static constexpr size_t kBytesPerPixel = 3;
  static constexpr uint8_t kMask         = 0xFF;
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
  // Hardcoding values is acceptable here
  for (size_t idx = 0; idx < buffer_size; idx += kBytesPerPixel) {
    // Some displays expect BGR byte order for 24-bit pixels; write as B,G,R
    *(p_buffer + idx + 0) = static_cast<uint8_t>((color_value >> 0) & kMask);
    *(p_buffer + idx + 1) = static_cast<uint8_t>((color_value >> 8) & kMask);
    *(p_buffer + idx + 2) = static_cast<uint8_t>((color_value >> 16) & kMask);
  }
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
}

void Display::fill_line_argb8888(const uint32_t* p_src, size_t src_size, uint8_t* p_buffer) {
  // srcSize is number of pixels. Write each pixel as 4 consecutive bytes.
  for (size_t px = 0; px < src_size; px++) {
    // write 32-bit pixel value into buffer at byte offset px*4
    // NOLINTNEXTLINE(readability/casting, modernize-avoid-c-style-cast, cppcoreguidelines-pro-type-cstyle-cast)
    *((uint32_t*)(p_buffer + (px * 4))) = *p_src;
    p_src++;
  }
}

void Display::fill_line_rgb888(const uint32_t* p_src, size_t src_size, uint8_t* p_buffer) {
  static constexpr uint8_t kMask         = 0xFF;
  static constexpr size_t kBytesPerPixel = 3;
  // NOLINTBEGIN(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
  // Hardcoding values is acceptable here
  // NOLINTBEGIN(readability/casting, modernize-avoid-c-style-cast, cppcoreguidelines-pro-type-cstyle-cast)
  // Casting is necessary to write 24-bit pixels in BGR byte order
  for (size_t idx = 0; idx < src_size; idx++) {
    uint32_t color = *p_src;
    // Some displays expect BGR byte order for 24-bit pixels; write as B,G,R
    size_t offset            = idx * kBytesPerPixel;
    *(p_buffer + offset + 0) = static_cast<uint8_t>((color >> 0) & kMask);
    *(p_buffer + offset + 1) = static_cast<uint8_t>((color >> 8) & kMask);
    *(p_buffer + offset + 2) = static_cast<uint8_t>((color >> 16) & kMask);
    p_src++;
  }
  // NOLINTEND(readability/casting, modernize-avoid-c-style-cast, cppcoreguidelines-pro-type-cstyle-cast)
  // NOLINTEND(cppcoreguidelines-avoid-magic-numbers, readability-magic-numbers)
}

void Display::fill_rgb_rect(uint32_t x_pos,
                            uint32_t y_pos,
                            uint32_t* p_data,
                            uint32_t width,  // NOLINT(bugprone-easily-swappable-parameters) - this is a private
                                             // method and we always call it with width before height, so the
                                             // order of parameters is not error-prone in practice
                            uint32_t height) {
  struct display_buffer_descriptor buf_desc = {.buf_size = 0, .width = 0, .height = 0, .pitch = 0, .frame_incomplete = false};
  x_pos                                     = zpp_lib::min(_lcd_xsize - 1, x_pos);
  buf_desc.width                            = zpp_lib::min(_lcd_xsize - x_pos, width);
  buf_desc.pitch                            = buf_desc.width;
  buf_desc.height                           = kHStep;
  buf_desc.frame_incomplete                 = true;
  uint32_t bytes_per_pixel                  = (_display_capabilities.current_pixel_format == PIXEL_FORMAT_ARGB_8888) ? 4U : 3U;
  buf_desc.buf_size                         = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
  // draw the rect line by line
  uint16_t first_line = zpp_lib::min(y_pos, _lcd_ysize);
  uint16_t last_line  = zpp_lib::min(y_pos + height, _lcd_ysize);
  for (uint32_t line = first_line; line < last_line; line += kHStep) {
    // fill only the number of pixels we will write (buf_desc.width)
    _fill_line_function(p_data, buf_desc.width, _line_buffer);
    uint16_t line_height      = zpp_lib::min(kHStep, last_line - line);
    buf_desc.height           = line_height;
    buf_desc.buf_size         = buf_desc.pitch * buf_desc.height * bytes_per_pixel;
    buf_desc.frame_incomplete = (line + line_height < last_line);
    auto ret                  = display_write(_display_device, x_pos, line, &buf_desc, _line_buffer);
    if (ret != 0) {
      ZPP_LOG_ERR("Cannot write to display: %d", ret);
    }
    p_data += width;
  }
}

}  // namespace zpp_lib

#endif  // CONFIG_DISPLAY
