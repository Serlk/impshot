// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// PNG без альфы, быстрое сжатие; отдача в wl-copy и запись в файл.
#include <cstdint>
#include <string>
#include <vector>

bool encode_png(const std::vector<uint8_t>& rgba, int w, int h, std::vector<uint8_t>& out);
// primary — в primary selection (вставка средней кнопкой) вместо обычного буфера
bool copy_to_clipboard(const std::vector<uint8_t>& data, const char* mime = "image/png", bool primary = false);
// запустить программу, отдать ей input в stdin и вернуть stdout; false — не запустилась или упала
bool run_filter(const std::vector<std::string>& args, const std::vector<uint8_t>& input, std::string& out);
// путь по шаблону каталога; пустая строка — не удалось
std::string save_png(const std::vector<uint8_t>& png, const std::string& dir);
void        notify(const std::string& summary, const std::string& body, const std::string& icon_path);
