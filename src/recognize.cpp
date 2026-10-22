// SPDX-License-Identifier: GPL-3.0-or-later
#include "recognize.hpp"

#include "export.hpp"

#include <ZXing/ReadBarcode.h>
#include <algorithm>
#include <cstdio>
#include <thread>

namespace {

std::vector<uint8_t> pgm(const std::vector<uint8_t>& gray, int w, int y0, int y1) {
    std::string          head = "P5\n" + std::to_string(w) + " " + std::to_string(y1 - y0) + "\n255\n";
    std::vector<uint8_t> out(head.begin(), head.end());
    out.insert(out.end(), gray.begin() + static_cast<ptrdiff_t>(y0) * w, gray.begin() + static_cast<ptrdiff_t>(y1) * w);
    return out;
}

void trim_tail(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\f'))
        s.pop_back();
}

} // namespace

namespace {

// серое изображение (тёмная тема переворачивается) и полосы для параллельного распознавания
struct Prepared {
    std::vector<uint8_t>             gray;
    std::vector<std::pair<int, int>> bands;
};

Prepared prepare(const std::vector<uint8_t>& rgba, int w, int h) {
    Prepared p;
    size_t   n = static_cast<size_t>(w) * h;
    p.gray.resize(n);
    uint64_t sum = 0;
    for (size_t i = 0; i < n; i++) {
        const uint8_t* px = &rgba[i * 4];
        p.gray[i]         = static_cast<uint8_t>((px[0] * 77 + px[1] * 150 + px[2] * 29) >> 8);
        sum += p.gray[i];
    }
    // светлый текст на тёмном (терминалы, тёмные темы) tesseract читает хуже — переворачиваем
    if (n && sum / n < 110)
        for (auto& g : p.gray)
            g = static_cast<uint8_t>(255 - g);

    // tesseract распознаёт строки по очереди на одном ядре. Режем картинку на полосы и гоним их
    // параллельно отдельными процессами. Резать надо между строками текста: берём ряды с наименьшей
    // «энергией» (сумма перепадов яркости) — пустые промежутки, а на фото и картинках самые ровные ряды
    std::vector<uint32_t> energy(h);
    for (int y = 0; y < h; y++) {
        const uint8_t* row = &p.gray[static_cast<size_t>(y) * w];
        uint32_t       e   = 0;
        for (int x = 1; x < w; x++)
            e += static_cast<uint32_t>(std::abs(row[x] - row[x - 1]));
        energy[y] = e;
    }
    unsigned cores = std::max(1u, std::min(12u, std::thread::hardware_concurrency() * 3 / 4));
    int      nb    = std::clamp(h / 90, 1, static_cast<int>(cores)); // полоса не ниже ~3 строк
    int      start = 0;
    for (int k = 1; k < nb; k++) {
        int target = h * k / nb, win = h / nb / 3;
        int best   = -1;
        for (int y = std::max(start + 30, target - win); y <= std::min(h - 30, target + win); y++)
            if (best < 0 || energy[y] < energy[best])
                best = y;
        if (best > start) {
            p.bands.push_back({start, best});
            start = best;
        }
    }
    p.bands.push_back({start, h});
    return p;
}

// каждая полоса — свой процесс tesseract; outs[i] — его stdout
bool run_bands(const Prepared& p, int w, const std::string& lang, bool tsv, std::vector<std::string>& outs) {
    outs.assign(p.bands.size(), {});
    std::vector<char>        good(p.bands.size(), 0);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < p.bands.size(); i++)
        workers.emplace_back([&, i] {
            std::vector<std::string> args{"tesseract", "stdin", "stdout", "-l", lang};
            if (tsv)
                args.push_back("tsv");
            good[i] = run_filter(args, pgm(p.gray, w, p.bands[i].first, p.bands[i].second), outs[i]);
        });
    for (auto& t : workers)
        t.join();
    return std::ranges::all_of(good, [](char g) { return g != 0; });
}

} // namespace

std::string ocr_text(const std::vector<uint8_t>& rgba, int w, int h, const std::string& lang, bool& ok) {
    Prepared                 p = prepare(rgba, w, h);
    std::vector<std::string> parts;
    ok = run_bands(p, w, lang, false, parts);
    std::string out;
    for (auto& part : parts) {
        trim_tail(part);
        if (part.empty())
            continue;
        if (!out.empty())
            out += '\n';
        out += part;
    }
    return out;
}

std::vector<OcrWord> ocr_words(const std::vector<uint8_t>& rgba, int w, int h, const std::string& lang, bool& ok) {
    Prepared                 p = prepare(rgba, w, h);
    std::vector<std::string> outs;
    ok = run_bands(p, w, lang, true, outs);
    std::vector<OcrWord> words;
    for (size_t b = 0; b < outs.size(); b++) {
        // level page block par line word left top width height conf text
        size_t pos = 0;
        while (pos < outs[b].size()) {
            size_t      eol  = outs[b].find('\n', pos);
            std::string line = outs[b].substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
            pos              = eol == std::string::npos ? outs[b].size() : eol + 1;
            std::vector<std::string> f;
            size_t                   s = 0;
            for (int k = 0; k < 11; k++) {
                size_t t = line.find('\t', s);
                if (t == std::string::npos)
                    break;
                f.push_back(line.substr(s, t - s));
                s = t + 1;
            }
            if (f.size() != 11 || f[0] != "5")
                continue;
            std::string text = line.substr(s);
            while (!text.empty() && (text.back() == '\r' || text.back() == ' '))
                text.pop_back();
            if (text.empty())
                continue;
            try {
                OcrWord wd;
                long blk = std::stol(f[2]), par = std::stol(f[3]), ln = std::stol(f[4]);
                wd.par   = (static_cast<long>(b) << 32) | (blk << 16) | par;
                wd.line  = (wd.par << 12) | ln;
                wd.x     = std::stoi(f[6]);
                wd.y     = std::stoi(f[7]) + p.bands[b].first;
                wd.w     = std::stoi(f[8]);
                wd.h     = std::stoi(f[9]);
                wd.text  = std::move(text);
                words.push_back(std::move(wd));
            } catch (const std::exception&) {
            }
        }
    }
    return words;
}

std::string join_words(const std::vector<OcrWord>& words, size_t from, size_t to) {
    std::string out;
    for (size_t i = from; i <= to && i < words.size(); i++) {
        if (i > from)
            out += words[i].line == words[i - 1].line ? " " : words[i].par == words[i - 1].par ? "\n" : "\n\n";
        out += words[i].text;
    }
    return out;
}

std::vector<std::string> read_codes(const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<std::string> res;
    try {
        ZXing::ImageView     img(rgba.data(), w, h, ZXing::ImageFormat::RGBA);
        ZXing::ReaderOptions opts;
        opts.setTryHarder(true);
        opts.setTryInvert(true);
        opts.setTryRotate(true);
        for (auto& b : ZXing::ReadBarcodes(img, opts))
            if (b.isValid())
                res.push_back(b.text());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "impshot: zxing: %s\n", e.what());
    }
    return res;
}
