// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Распознавание выделения: текст через tesseract, QR и прочие штрихкоды через zxing-cpp.
#include <cstdint>
#include <string>
#include <vector>

// rgba — строки сверху вниз; lang в формате tesseract ("rus+eng"); пустая строка — ничего не найдено
std::string ocr_text(const std::vector<uint8_t>& rgba, int w, int h, const std::string& lang, bool& ok);
struct OcrWord {
    int         x = 0, y = 0, w = 0, h = 0; // в пикселях переданного изображения
    long        line = 0, par = 0;          // ключи строки и абзаца: слова одной строки — одно значение
    std::string text;
};
// слова в порядке чтения — для выделения куска текста мышью
std::vector<OcrWord> ocr_words(const std::vector<uint8_t>& rgba, int w, int h, const std::string& lang, bool& ok);
// склеить слова [from, to] обратно в текст: пробелы внутри строки, переводы строк между строками
std::string join_words(const std::vector<OcrWord>& words, size_t from, size_t to);

std::vector<std::string> read_codes(const std::vector<uint8_t>& rgba, int w, int h);
