// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// RTF to HTML sections (see office.h). The file is read front to back through
// a small buffer, never whole. Pictures (PNG and JPEG) are not decoded here:
// their place in the file becomes an "rtf:" image source, read back when the
// reader shows them.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <vector>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/fb2.h"
#include "doc/office.h"
#include "gfx/image_cache.h"

namespace doc {

namespace {

constexpr char kSep = '\x01';

class ByteReader {
public:
    explicit ByteReader(FILE* f) : f_(f) {}
    int get() {
        if (at_ == len_ && !fill()) return EOF;
        ++offset_;
        return (unsigned char)buf_[at_++];
    }
    int peek() {
        if (at_ == len_ && !fill()) return EOF;
        return (unsigned char)buf_[at_];
    }
    uint64_t offset() const { return offset_; }  // of the next byte

private:
    bool fill() {
        len_ = fread(buf_, 1, sizeof buf_, f_);
        at_ = 0;
        return len_ > 0;
    }
    FILE* f_;
    char buf_[64 * 1024];
    size_t at_ = 0, len_ = 0;
    uint64_t offset_ = 0;
};

enum class Dest { Text, Skip, Style, Title, Author, Pict, FieldInst, Bookmark };

struct CharFormat {
    bool bold = false, italic = false, underline = false, strike = false;
    int position = 0;
    bool operator==(const CharFormat& o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike && position == o.position;
    }
};

struct Group {
    CharFormat fmt;
    int uc = 1;
    Dest dest = Dest::Text;
    bool close_link = false, note = false, pict = false;
};

struct Picture {
    std::string kind;  // "png", "jpeg", or "" (a format that cannot be drawn)
    int w = 0, h = 0, goal_w = 0, goal_h = 0, scale_x = 100, scale_y = 100;
    uint64_t start = 0, end = 0;
};

// Destinations whose text is not part of the document.
bool skipped_destination(const std::string& w) {
    static const char* const kSkip[] = {
        "fonttbl", "colortbl", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf",
        "listtable", "listoverridetable", "revtbl", "rsidtbl", "generator", "xmlnstbl", "mmathPr", "themedata",
        "colorschememapping", "latentstyles", "datastore", "filetbl", "pgdsctbl", "ftnsep", "ftnsepc", "aftnsep",
        "aftnsepc", "pnseclvl", "pn", "nonshppict", "objdata", "bkmkend", "xe", "tc", "tcn", "docvar", "template",
        "userprops", "wgrffmtfilter", "listpicture", "fchars", "lchars", "pgdsc", "annotation", "atnid", "atnauthor",
        "atrfstart", "atrfend", "comment", "operator", "company", "manager", "category", "keywords", "subject",
        "doccomm", "hlinkbase", "creatim", "revtim", "printim", "buptim", "private", "panose", "falt", "fname",
        "blipuid", "picprop", "sp", "sn", "sv", "shpinst", "oleclsid", "do", "protusertbl", "mhtmltag", "htmltag",
        "background", "factoidname", "ud", "upr", "ptabldr", "ffdeftext", "ffname", "ffhelptext", "ffstattext",
        "ffentrymcr", "ffexitmcr", "formfield", "pntxta", "pntxtb", "listname", "leveltext", "levelnumbers",
        "nesttableprops", "tblptxt"};
    for (const char* s : kSkip)
        if (w == s) return true;
    return false;
}

std::string encoding_of(int codepage) {
    if (codepage == 1251) return "windows-1251";
    if (codepage == 20866) return "koi8-r";
    return "windows-1252";
}

class Rtf {
public:
    Rtf(const std::string& path, HtmlBook& book, bool info_only) : path_(path), book_(book), info_only_(info_only) {}

    bool run(std::string& error) {
        FILE* f = lib::open_file(path_);
        if (!f) {
            error = "The file could not be read.";
            return false;
        }
        ByteReader r(f);
        char head[6] = {0};
        for (int i = 0; i < 5; ++i) {
            const int c = r.get();
            head[i] = c == EOF ? 0 : (char)c;
        }
        if (memcmp(head, "{\\rtf", 5) != 0) {
            fclose(f);
            error = "This is not an RTF document.";
            return false;
        }
        groups_.push_back(Group());  // the document group, already opened
        parse(r);
        fclose(f);
        if (info_only_) return true;
        end_paragraph();
        close_table();
        if (!notes_.empty()) {
            out_.boundary();
            out_.html() += "<hr><h2 id=\"notes\">Notes</h2>\n";
            book_.anchors["notes"] = out_.section();
            book_.toc.push_back({"Notes", "notes", out_.section(), 1});
            for (size_t i = 0; i < notes_.size(); ++i) {
                out_.boundary();
                const std::string anchor = "rtfnote-" + std::to_string(i + 1);
                book_.anchors[anchor] = out_.section();
                out_.html() += "<div id=\"" + anchor + "\"><p><b>" + std::to_string(i + 1) + ".</b></p>" + notes_[i] + "</div>\n";
            }
        }
        out_.finish(book_.sections);
        LOG("rtf: %zu sections, %zu headings, %d pictures", book_.sections.size(), book_.toc.size(), pictures_);
        return true;
    }

private:
    Group& g() { return groups_.back(); }

    void parse(ByteReader& r) {
        bool group_start = false;  // just after '{': the next word may name a destination
        bool ignorable = false;    // "{\*"
        int c;
        while ((c = r.get()) != EOF) {
            if (info_only_ && done_info_) return;
            if (c == '{') {
                Group copy = groups_.empty() ? Group() : g();
                copy.close_link = copy.note = copy.pict = false;
                groups_.push_back(copy);
                group_start = true;
                ignorable = false;
                continue;
            }
            if (c == '}') {
                pop_group(r);
                group_start = ignorable = false;
                if (groups_.empty()) return;
                continue;
            }
            if (c == '\\') {
                const int n = r.peek();
                if (n == EOF) return;
                if (!isalpha(n)) {
                    r.get();
                    control_symbol((char)n, r, ignorable, group_start);
                    continue;
                }
                std::string word;
                while (isalpha(r.peek()) && word.size() < 32) word += (char)r.get();
                bool has_param = false, negative = false;
                long param = 0;
                if (r.peek() == '-') {
                    negative = true;
                    r.get();
                }
                while (isdigit(r.peek())) {
                    has_param = true;
                    param = param * 10 + (r.get() - '0');
                    if (param > 100000000) param = 100000000;
                }
                if (negative) param = -param;
                if (r.peek() == ' ') r.get();
                const bool first = group_start;
                group_start = false;
                if (first) {
                    if (destination(word, ignorable)) {
                        ignorable = false;
                        continue;
                    }
                }
                ignorable = false;
                control_word(word, has_param, param, r);
                continue;
            }
            group_start = false;
            if (c == '\r' || c == '\n') continue;
            character((char)c, r.offset() - 1);
        }
    }

    // A destination at the start of a group. True when the word was one.
    bool destination(const std::string& w, bool ignorable) {
        Group& s = g();
        if (s.dest == Dest::Skip) return true;
        if (w == "stylesheet") {
            s.dest = Dest::Style;
            return true;
        }
        if (w == "info") return true;  // its title and author groups follow
        if (w == "title") {
            s.dest = Dest::Title;
            capture_.clear();
            return true;
        }
        if (w == "author") {
            s.dest = Dest::Author;
            capture_.clear();
            return true;
        }
        if (w == "pict") {
            s.dest = Dest::Pict;
            s.pict = true;
            picture_ = Picture();
            return true;
        }
        if (w == "fldinst") {
            s.dest = Dest::FieldInst;
            capture_.clear();
            return true;
        }
        if (w == "fldrslt") {
            const std::string href = field_link(field_instr_);
            field_instr_.clear();
            if (!href.empty()) {
                flush_run();
                para_ += "<a href=\"" + html_escape(href) + "\">";
                s.close_link = true;
            }
            return true;
        }
        if (w == "bkmkstart") {
            s.dest = Dest::Bookmark;
            capture_.clear();
            return true;
        }
        if (w == "footnote") {
            flush_run();
            const int n = (int)notes_.size() + 1;
            para_ += "<sup><a href=\"#rtfnote-" + std::to_string(n) + "\">" + std::to_string(n) + "</a></sup>";
            notes_.push_back("");
            // The note's own paragraphs are collected apart from the text around it.
            saved_.push_back({para_, para_text_, run_, run_fmt_, center_, style_, outline_, intbl_});
            para_.clear();
            para_text_.clear();
            run_.clear();
            s.note = true;
            ++in_note_;
            return true;
        }
        if (w == "shppict" || w == "result" || w == "field") return true;
        if (skipped_destination(w) || ignorable) {
            s.dest = Dest::Skip;
            return true;
        }
        return false;
    }

    std::string field_link(const std::string& instr) {
        std::string s = lib::trim(instr);
        if (!lib::starts_with(lib::lower(s), "hyperlink")) return "";
        s = lib::trim(s.substr(9));
        bool local = false;
        if (lib::starts_with(s, "\\l")) {
            local = true;
            s = lib::trim(s.substr(2));
        }
        std::string target;
        if (!s.empty() && s[0] == '"') {
            const size_t close = s.find('"', 1);
            target = s.substr(1, (close == std::string::npos ? s.size() : close) - 1);
        } else {
            target = s.substr(0, s.find(' '));
        }
        if (target.empty()) return "";
        return local ? "#" + target : target;
    }

    void pop_group(ByteReader& r) {
        if (groups_.empty()) return;
        Group done = groups_.back();
        // Text captured for this group's destination.
        if (done.dest == Dest::Title && !(groups_.size() >= 2 && groups_[groups_.size() - 2].dest == Dest::Title)) {
            book_.title = lib::collapse_space(decode(capture_));
            capture_.clear();
        } else if (done.dest == Dest::Author && !(groups_.size() >= 2 && groups_[groups_.size() - 2].dest == Dest::Author)) {
            book_.author = lib::collapse_space(decode(capture_));
            capture_.clear();
            done_info_ = info_only_ && !book_.title.empty();
        } else if (done.dest == Dest::FieldInst && !(groups_.size() >= 2 && groups_[groups_.size() - 2].dest == Dest::FieldInst)) {
            field_instr_ = decode(capture_);
            capture_.clear();
        } else if (done.dest == Dest::Bookmark && !(groups_.size() >= 2 && groups_[groups_.size() - 2].dest == Dest::Bookmark)) {
            const std::string name = lib::trim(decode(capture_));
            capture_.clear();
            if (!name.empty() && !info_only_) {
                flush_run();
                para_ += "<a id=\"" + html_escape(name) + "\"></a>";
                para_ids_.push_back(name);
            }
        }
        if (done.dest == Dest::Style && style_depth_ == groups_.size()) {
            finish_style();
        }
        if (done.pict) picture_done();
        if (done.close_link) {
            flush_run();
            para_ += "</a>";
        }
        if (done.note) {
            end_paragraph();
            --in_note_;
            const Saved s = saved_.back();
            saved_.pop_back();
            para_ = s.para;
            para_text_ = s.text;
            run_ = s.run;
            run_fmt_ = s.fmt;
            center_ = s.center;
            style_ = s.style;
            outline_ = s.outline;
            intbl_ = s.intbl;
        }
        groups_.pop_back();
        (void)r;
    }

    void finish_style() {
        if (style_def_ >= 0) {
            std::string name = lib::lower(lib::trim(decode(capture_)));
            if (!name.empty() && name.back() == ';') name.pop_back();
            int level = style_outline_ >= 0 ? style_outline_ + 1 : 0;
            if (lib::starts_with(name, "heading ") && name.size() > 8) level = atoi(name.c_str() + 8);
            if (name == "title") level = 1;
            heading_styles_[style_def_] = level;
        }
        style_def_ = -1;
        style_outline_ = -1;
        style_depth_ = 0;
        capture_.clear();
    }

    void control_symbol(char c, ByteReader& r, bool& ignorable, bool& group_start) {
        if (c == '*') {
            ignorable = true;
            return;
        }
        group_start = false;
        if (c == '\'') {
            int v = 0;
            for (int i = 0; i < 2; ++i) {
                const int h = r.get();
                v = v * 16 + (isdigit(h) ? h - '0' : (h | 32) >= 'a' && (h | 32) <= 'f' ? (h | 32) - 'a' + 10 : 0);
            }
            if (skip_ > 0) {
                --skip_;
                return;
            }
            byte((char)v);
            return;
        }
        if (c == '\\' || c == '{' || c == '}') {
            character(c, r.offset() - 1);
            return;
        }
        if (c == '~') text("\xC2\xA0");
        else if (c == '_') text("-");
        else if (c == '\r' || c == '\n') control_word("par", false, 0, r);
        // "\-" (optional hyphen), "\|", "\:": nothing to show.
    }

    void control_word(const std::string& w, bool has_param, long param, ByteReader& r) {
        Group& s = g();
        if (w == "bin") {
            for (long i = 0; i < param; ++i) r.get();
            return;
        }
        if (s.dest == Dest::Skip) return;
        if (w == "u") {
            long v = param;
            if (v < 0) v += 65536;
            if (skip_) skip_ = 0;
            uint32_t cp = (uint32_t)v;
            // A surrogate pair arrives as two \u words.
            if (cp >= 0xD800 && cp < 0xDC00) {
                high_surrogate_ = cp;
            } else {
                if (cp >= 0xDC00 && cp < 0xE000 && high_surrogate_) {
                    cp = 0x10000 + ((high_surrogate_ - 0xD800) << 10) + (cp - 0xDC00);
                    high_surrogate_ = 0;
                }
                std::string u;
                lib::utf8_append(u, cp);
                text(u);
            }
            skip_ = s.uc;
            return;
        }
        if (w == "uc") {
            s.uc = (int)std::max(0L, param);
            return;
        }
        if (w == "ansicpg") {
            codepage_ = (int)param;
            return;
        }
        if (s.dest == Dest::Style) {
            if (w == "s" || w == "cs" || w == "ds" || w == "ts") {
                style_def_ = w == "s" ? (int)param : -1;
                style_depth_ = groups_.size();
                capture_.clear();
            } else if (w == "outlinelevel") {
                style_outline_ = (int)param;
            }
            return;
        }
        if (s.dest == Dest::Pict) {
            if (w == "pngblip") picture_.kind = "png";
            else if (w == "jpegblip") picture_.kind = "jpeg";
            else if (w == "emfblip" || w == "wmetafile" || w == "macpict" || w == "dibitmap" || w == "wbitmap" ||
                     w == "pmmetafile")
                picture_.kind = "";
            else if (w == "picw") picture_.w = (int)param;
            else if (w == "pich") picture_.h = (int)param;
            else if (w == "picwgoal") picture_.goal_w = (int)param;
            else if (w == "pichgoal") picture_.goal_h = (int)param;
            else if (w == "picscalex") picture_.scale_x = (int)param;
            else if (w == "picscaley") picture_.scale_y = (int)param;
            return;
        }
        if (s.dest != Dest::Text) {
            if (w == "tab" || w == "par" || w == "line") capture_ += ' ';
            return;
        }
        if (info_only_) {
            // The document's text has begun: \info, if any, is behind.
            if (w == "pard" || w == "par" || w == "sectd" || w == "plain") done_info_ = true;
            return;
        }
        CharFormat& f = s.fmt;
        const bool on = !has_param || param != 0;
        if (w == "par" || w == "sect" || w == "page") end_paragraph();
        else if (w == "line") {
            flush_run();
            para_ += "<br>";
        } else if (w == "tab") text(" ");
        else if (w == "pard") {
            center_ = false;
            style_ = 0;
            outline_ = -1;
            intbl_ = false;
        } else if (w == "plain") f = CharFormat();
        else if (w == "b") f.bold = on;
        else if (w == "i") f.italic = on;
        else if (w == "ul" || w == "uld" || w == "uldb" || w == "ulw" || w == "uldash" || w == "ulth") f.underline = on;
        else if (w == "ulnone") f.underline = false;
        else if (w == "strike" || w == "striked") f.strike = on;
        else if (w == "super") f.position = 1;
        else if (w == "sub") f.position = -1;
        else if (w == "nosupersub") f.position = 0;
        else if (w == "qc") center_ = true;
        else if (w == "ql" || w == "qr" || w == "qj" || w == "qd") center_ = false;
        else if (w == "s") style_ = (int)param;
        else if (w == "outlinelevel") outline_ = (int)param;
        else if (w == "intbl") intbl_ = true;
        else if (w == "cell" || w == "nestcell") end_cell();
        else if (w == "row" || w == "nestrow") end_row();
        else if (w == "emdash") text("\xE2\x80\x94");
        else if (w == "endash") text("\xE2\x80\x93");
        else if (w == "lquote") text("\xE2\x80\x98");
        else if (w == "rquote") text("\xE2\x80\x99");
        else if (w == "ldblquote") text("\xE2\x80\x9C");
        else if (w == "rdblquote") text("\xE2\x80\x9D");
        else if (w == "bullet") text("\xE2\x80\xA2");
        else if (w == "emspace" || w == "enspace" || w == "qmspace") text(" ");
    }

    void character(char c, uint64_t offset) {
        Group& s = g();
        if (s.dest == Dest::Skip) return;
        if (s.dest == Dest::Pict) {
            if (isxdigit((unsigned char)c)) {
                if (!picture_.start) picture_.start = offset;
                picture_.end = offset + 1;
            }
            return;
        }
        if (skip_ > 0) {
            --skip_;
            return;
        }
        if (s.dest == Dest::Style) {
            if (c == ';') {
                if (style_def_ >= 0) finish_style();
            } else {
                capture_ += c;
            }
            return;
        }
        if (s.dest != Dest::Text) {
            capture_ += c;
            return;
        }
        if (info_only_) return;
        byte(c);
    }

    // A byte of text in the document's code page.
    void byte(char c) {
        Group& s = g();
        if (s.dest == Dest::Skip || s.dest == Dest::Pict) return;
        if (s.dest != Dest::Text) {
            capture_ += c;
            return;
        }
        if (info_only_) return;
        if (!(s.fmt == run_fmt_)) flush_run();
        run_bytes_ += c;
    }

    void text(const std::string& utf8) {
        Group& s = g();
        if (s.dest != Dest::Text) {
            if (s.dest != Dest::Skip && s.dest != Dest::Pict) capture_ += utf8;
            return;
        }
        if (info_only_) return;
        if (!(s.fmt == run_fmt_)) flush_run();
        decode_bytes();
        run_ += utf8;
    }

    std::string decode(const std::string& bytes) const {
        if (lib::is_valid_utf8(bytes) && codepage_ == 65001) return bytes;
        return decode_legacy(bytes, encoding_of(codepage_));
    }

    void decode_bytes() {
        if (run_bytes_.empty()) return;
        run_ += decode(run_bytes_);
        run_bytes_.clear();
    }

    void flush_run() {
        decode_bytes();
        if (!run_.empty()) {
            std::string open, close;
            if (run_fmt_.bold) { open += "<b>"; close = "</b>" + close; }
            if (run_fmt_.italic) { open += "<i>"; close = "</i>" + close; }
            if (run_fmt_.underline) { open += "<u>"; close = "</u>" + close; }
            if (run_fmt_.strike) { open += "<s>"; close = "</s>" + close; }
            if (run_fmt_.position > 0) { open += "<sup>"; close = "</sup>" + close; }
            if (run_fmt_.position < 0) { open += "<sub>"; close = "</sub>" + close; }
            para_ += open + html_escape(run_) + close;
            para_text_ += run_;
            run_.clear();
        }
        run_fmt_ = groups_.empty() ? CharFormat() : g().fmt;
    }

    void picture_done() {
        const Picture p = picture_;
        picture_ = Picture();
        if (p.kind.empty() || !p.start || p.end <= p.start || info_only_) return;
        // Shown size: the goal size (twips) scaled, else the picture's own.
        int w = p.goal_w > 0 ? p.goal_w / 15 : p.w;
        int h = p.goal_h > 0 ? p.goal_h / 15 : p.h;
        w = w * std::max(1, p.scale_x) / 100;
        h = h * std::max(1, p.scale_y) / 100;
        flush_run();
        const std::string key = "rtf:" + path_ + kSep + std::to_string(p.start) + ":" + std::to_string(p.end);
        para_ += "<img src=\"" + html_escape(key) + "\"" +
                 (w > 0 && h > 0 ? " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"" : "") + ">";
        has_picture_ = true;
        ++pictures_;
    }

    void register_ids() {
        for (const auto& id : para_ids_) book_.anchors[id] = out_.section();
        para_ids_.clear();
    }

    void end_paragraph() {
        flush_run();
        const bool content = !lib::trim(para_text_).empty() || has_picture_ || !para_ids_.empty();
        if (in_note_ > 0) {
            if (content) notes_.back() += "<p>" + para_ + "</p>";
            para_.clear();
            para_text_.clear();
            has_picture_ = false;
            return;
        }
        if (intbl_) {
            // Text in a table cell: kept for the cell.
            if (content) cell_ += (cell_.empty() ? "" : "<br>") + para_;
            para_.clear();
            para_text_.clear();
            has_picture_ = false;
            return;
        }
        close_table();
        if (content) {
            int level = outline_ >= 0 ? outline_ + 1 : 0;
            auto hs = heading_styles_.find(style_);
            if (!level && hs != heading_styles_.end()) level = hs->second;
            if (level > 0) {
                level = std::max(1, std::min(6, level));
                out_.boundary(level);
                const std::string id = "h-" + std::to_string(++headings_);
                book_.anchors[id] = out_.section();
                register_ids();
                const std::string title = lib::collapse_space(para_text_);
                if (!title.empty()) book_.toc.push_back({title, id, out_.section(), level});
                out_.html() += "<h" + std::to_string(level) + " id=\"" + id + "\">" + para_ + "</h" + std::to_string(level) + ">\n";
            } else {
                out_.boundary();
                register_ids();
                out_.html() += std::string(center_ ? "<p style=\"text-align:center\">" : "<p>") + para_ + "</p>\n";
            }
        }
        para_.clear();
        para_text_.clear();
        has_picture_ = false;
    }

    void end_cell() {
        flush_run();
        if (!lib::trim(para_text_).empty() || has_picture_) cell_ += (cell_.empty() ? "" : "<br>") + para_;
        para_.clear();
        para_text_.clear();
        has_picture_ = false;
        cells_.push_back(cell_);
        cell_.clear();
    }

    void end_row() {
        if (cell_.size() || !lib::trim(para_text_).empty()) end_cell();
        if (in_note_ > 0) {
            for (const auto& c : cells_) notes_.back() += "<p>" + c + "</p>";
            cells_.clear();
            return;
        }
        if (!table_open_) {
            out_.boundary();
            out_.html() += "<table>";
            table_open_ = true;
        }
        out_.html() += "<tr>";
        for (const auto& c : cells_) out_.html() += "<td>" + c + "</td>";
        out_.html() += "</tr>";
        cells_.clear();
        register_ids();
    }

    void close_table() {
        if (!cells_.empty()) end_row();
        if (table_open_) {
            out_.html() += "</table>\n";
            table_open_ = false;
        }
    }

    struct Saved {
        std::string para, text, run;
        CharFormat fmt;
        bool center;
        int style, outline;
        bool intbl;
    };

    std::string path_;
    HtmlBook& book_;
    bool info_only_ = false, done_info_ = false;
    std::vector<Group> groups_;
    std::vector<Saved> saved_;
    HtmlSections out_;
    std::string para_, para_text_, run_, run_bytes_, capture_, field_instr_, cell_;
    std::vector<std::string> cells_, notes_, para_ids_;
    CharFormat run_fmt_;
    Picture picture_;
    std::map<int, int> heading_styles_;
    int codepage_ = 1252, skip_ = 0, style_ = 0, outline_ = -1, headings_ = 0, pictures_ = 0, in_note_ = 0;
    int style_def_ = -1, style_outline_ = -1;
    size_t style_depth_ = 0;
    uint32_t high_surrogate_ = 0;
    bool center_ = false, intbl_ = false, table_open_ = false, has_picture_ = false;
};

}  // namespace

std::string rtf_hex_decode(std::string_view hex) {
    std::string out;
    out.reserve(hex.size() / 2);
    int nibble = -1;
    for (char c : hex) {
        int v;
        if (c >= '0' && c <= '9') v = c - '0';
        else if ((c | 32) >= 'a' && (c | 32) <= 'f') v = (c | 32) - 'a' + 10;
        else continue;
        if (nibble < 0) {
            nibble = v;
        } else {
            out += (char)(nibble << 4 | v);
            nibble = -1;
        }
    }
    return out;
}

bool open_rtf(const std::string& path, HtmlBook& out, std::string& error) {
    out = HtmlBook();
    Rtf r(path, out, false);
    return r.run(error);
}

bool rtf_info(const std::string& path, std::string& title, std::string& author) {
    HtmlBook book;
    Rtf r(path, book, true);
    std::string error;
    if (!r.run(error)) return false;
    title = book.title;
    author = book.author;
    return true;
}

void register_rtf_images() {
    gfx::image_register_prefix_loader("rtf:", [](const std::string& source, std::string& bytes) {
        const size_t sep = source.find(kSep);
        if (sep == std::string::npos) return false;
        const std::string path = source.substr(4, sep - 4);
        const std::string range = source.substr(sep + 1);
        const uint64_t start = strtoull(range.c_str(), nullptr, 10);
        const size_t colon = range.find(':');
        const uint64_t end = colon == std::string::npos ? 0 : strtoull(range.c_str() + colon + 1, nullptr, 10);
        // Hex doubles the size: a 32 MB picture is 64 MB of digits.
        if (end <= start || end - start > 64ull * 1024 * 1024) return false;
        FILE* f = lib::open_file(path);
        if (!f) return false;
        std::string hex((size_t)(end - start), '\0');
        const bool ok = fseeko(f, (off_t)start, SEEK_SET) == 0 && fread(&hex[0], 1, hex.size(), f) == hex.size();
        fclose(f);
        if (!ok) return false;
        bytes = rtf_hex_decode(hex);
        return !bytes.empty();
    });
}

}  // namespace doc
