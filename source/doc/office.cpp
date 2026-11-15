// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 jw-notes <https://github.com/jw-notes>
// This file is part of Librarian. Additional terms under GPLv3 section 7
// and third-party notices: see the NOTICE file.

// DOCX and ODT to HTML sections (see office.h). Both are ZIP archives of XML:
// the text is streamed through XmlPull and written out as simple HTML, which
// html_convert turns into the reader's document model.
#include "doc/office.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <functional>
#include <memory>

#include "core/fs.h"
#include "core/log.h"
#include "core/text.h"
#include "doc/archive.h"
#include "doc/epub.h"

namespace doc {

// ---- shared pieces --------------------------------------------------------------------------

std::string html_escape(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

bool is_office_format(const std::string& format) { return format == "docx" || format == "odt" || format == "rtf"; }

void HtmlSections::boundary(int heading) {
    // A long section is cut before the next block; a chapter heading starts a
    // new section once there is a reasonable amount before it.
    if (current_.size() >= target || (heading == 1 && current_.size() >= target / 5)) {
        done_.push_back(std::move(current_));
        current_.clear();
    }
}

void HtmlSections::finish(std::vector<std::string>& out) {
    if (!current_.empty() || done_.empty()) done_.push_back(std::move(current_));
    current_.clear();
    out = std::move(done_);
    done_.clear();
}

std::string_view XmlPull::local() const {
    const size_t colon = name_.find(':');
    return colon == std::string_view::npos ? name_ : name_.substr(colon + 1);
}

namespace {

std::string decode_xml_text(std::string_view raw) {
    if (raw.find('&') == std::string_view::npos) return std::string(raw);
    return lib::decode_entities(raw);
}

bool xml_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

}  // namespace

std::string XmlPull::attr(std::string_view qname) const {
    for (const auto& [n, v] : attrs_)
        if (n == qname) return decode_xml_text(v);
    return std::string();
}

bool XmlPull::has_attr(std::string_view qname) const {
    for (const auto& a : attrs_)
        if (a.first == qname) return true;
    return false;
}

XmlPull::Event XmlPull::next() {
    if (pending_end_) {
        pending_end_ = false;
        depth_ = level_--;
        return End;
    }
    while (pos_ < s_.size()) {
        if (s_[pos_] != '<') {
            size_t lt = s_.find('<', pos_);
            if (lt == std::string_view::npos) lt = s_.size();
            text_ = decode_xml_text(s_.substr(pos_, lt - pos_));
            pos_ = lt;
            return Text;
        }
        const std::string_view rest = s_.substr(pos_);
        if (rest.substr(0, 4) == "<!--") {
            const size_t end = s_.find("-->", pos_ + 4);
            pos_ = end == std::string_view::npos ? s_.size() : end + 3;
            continue;
        }
        if (rest.substr(0, 9) == "<![CDATA[") {
            const size_t end = s_.find("]]>", pos_ + 9);
            const size_t stop = end == std::string_view::npos ? s_.size() : end;
            text_ = std::string(s_.substr(pos_ + 9, stop - pos_ - 9));
            pos_ = end == std::string_view::npos ? s_.size() : end + 3;
            return Text;
        }
        if (rest.substr(0, 2) == "<?") {
            const size_t end = s_.find("?>", pos_ + 2);
            pos_ = end == std::string_view::npos ? s_.size() : end + 2;
            continue;
        }
        if (rest.substr(0, 2) == "<!") {
            // DOCTYPE, possibly with an internal subset.
            const size_t bracket = s_.find('[', pos_), gt = s_.find('>', pos_);
            if (bracket != std::string_view::npos && bracket < gt) {
                const size_t end = s_.find("]>", bracket);
                pos_ = end == std::string_view::npos ? s_.size() : end + 2;
            } else {
                pos_ = gt == std::string_view::npos ? s_.size() : gt + 1;
            }
            continue;
        }
        if (rest.size() > 1 && rest[1] == '/') {
            const size_t gt = s_.find('>', pos_);
            size_t end = gt == std::string_view::npos ? s_.size() : gt;
            size_t a = pos_ + 2;
            size_t b = a;
            while (b < end && !xml_space(s_[b])) ++b;
            name_ = s_.substr(a, b - a);
            pos_ = gt == std::string_view::npos ? s_.size() : gt + 1;
            depth_ = level_--;
            if (level_ < 0) level_ = 0;
            return End;
        }
        // A start tag.
        attrs_.clear();
        self_closing_ = false;
        size_t p = pos_ + 1;
        const size_t name_start = p;
        while (p < s_.size() && !xml_space(s_[p]) && s_[p] != '>' && s_[p] != '/') ++p;
        name_ = s_.substr(name_start, p - name_start);
        while (p < s_.size()) {
            while (p < s_.size() && xml_space(s_[p])) ++p;
            if (p >= s_.size()) break;
            if (s_[p] == '>') {
                ++p;
                break;
            }
            if (s_[p] == '/') {
                self_closing_ = true;
                ++p;
                continue;
            }
            const size_t an = p;
            while (p < s_.size() && !xml_space(s_[p]) && s_[p] != '=' && s_[p] != '>' && s_[p] != '/') ++p;
            const std::string_view aname = s_.substr(an, p - an);
            while (p < s_.size() && xml_space(s_[p])) ++p;
            std::string_view value;
            if (p < s_.size() && s_[p] == '=') {
                ++p;
                while (p < s_.size() && xml_space(s_[p])) ++p;
                if (p < s_.size() && (s_[p] == '"' || s_[p] == '\'')) {
                    const char q = s_[p++];
                    const size_t close = s_.find(q, p);
                    const size_t stop = close == std::string_view::npos ? s_.size() : close;
                    value = s_.substr(p, stop - p);
                    p = close == std::string_view::npos ? s_.size() : close + 1;
                }
            }
            if (!aname.empty()) attrs_.push_back({aname, value});
            else ++p;  // stray character
        }
        pos_ = p;
        depth_ = ++level_;
        if (self_closing_) pending_end_ = true;
        return Start;
    }
    return Eof;
}

void XmlPull::skip() {
    const int d = depth_;
    for (Event e = next(); e != Eof; e = next())
        if (e == End && depth_ == d) return;
}

namespace {

bool picture_ext(const std::string& path) {
    const std::string ext = lib::file_extension(path);
    return ext == "jpg" || ext == "jpeg" || ext == "png" || ext == "gif" || ext == "bmp" || ext == "webp";
}

std::string img_tag(const std::string& src, int w, int h, const std::string& alt) {
    std::string s = "<img src=\"" + html_escape(src) + "\"";
    if (w > 0 && h > 0) s += " width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\"";
    if (!alt.empty()) s += " alt=\"" + html_escape(alt) + "\"";
    return s + ">";
}

// Character formatting, written as nested HTML tags around a piece of text.
struct Format {
    bool bold = false, italic = false, underline = false, strike = false, mono = false;
    int position = 0;  // 1 superscript, -1 subscript
    std::string open() const {
        std::string s;
        if (bold) s += "<b>";
        if (italic) s += "<i>";
        if (underline) s += "<u>";
        if (strike) s += "<s>";
        if (mono) s += "<code>";
        if (position > 0) s += "<sup>";
        if (position < 0) s += "<sub>";
        return s;
    }
    std::string close() const {
        std::string s;
        if (position < 0) s += "</sub>";
        if (position > 0) s += "</sup>";
        if (mono) s += "</code>";
        if (strike) s += "</s>";
        if (underline) s += "</u>";
        if (italic) s += "</i>";
        if (bold) s += "</b>";
        return s;
    }
};

// "HYPERLINK \"http://x\" \o \"tip\"" -> http://x; "HYPERLINK \l \"anchor\"" -> #anchor.
std::string hyperlink_from_field(const std::string& instr) {
    std::string s = lib::trim(instr);
    if (!lib::starts_with(lib::lower(s), "hyperlink")) return "";
    s = s.substr(9);
    bool local = false;
    std::string target;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        if (i >= s.size()) break;
        if (s[i] == '\\') {
            if (i + 1 < s.size() && s[i + 1] == 'l') local = true;
            const bool takes_arg = i + 1 < s.size() && (s[i + 1] == 'o' || s[i + 1] == 't');
            i += 2;
            if (takes_arg) {
                while (i < s.size() && s[i] == ' ') ++i;
                if (i < s.size() && s[i] == '"') {
                    const size_t close = s.find('"', i + 1);
                    i = close == std::string::npos ? s.size() : close + 1;
                }
            }
            continue;
        }
        std::string word;
        if (s[i] == '"') {
            const size_t close = s.find('"', i + 1);
            word = s.substr(i + 1, (close == std::string::npos ? s.size() : close) - i - 1);
            i = close == std::string::npos ? s.size() : close + 1;
        } else {
            const size_t sp = s.find(' ', i);
            word = s.substr(i, (sp == std::string::npos ? s.size() : sp) - i);
            i = sp == std::string::npos ? s.size() : sp;
        }
        if (target.empty()) target = word;
        if (local && !target.empty()) break;
    }
    if (target.empty()) return "";
    return local ? "#" + target : target;
}

int clamp_heading(int level) { return std::max(1, std::min(6, level)); }

// ---- DOCX -----------------------------------------------------------------------------------

struct DocxStyle {
    std::string name, based_on;
    int outline = -1;  // 0-based outline level
    int bold = -1, italic = -1;
    std::string num_id;
    int ilvl = -1;
};

struct DocxLevel {
    std::string format = "decimal";
    int start = 1;
};

class Docx {
public:
    Docx(const std::string& path, HtmlBook& book) : path_(path), book_(book) {}

    bool run(std::string& error) {
        archive_ = open_archive(path_, &error);
        if (!archive_) {
            error = "This document could not be opened: it is not a DOCX file, or it is damaged or "
                    "password-protected.";
            return false;
        }
        std::string text;
        if (archive_->read("docProps/core.xml", text, 4 * 1024 * 1024)) core_properties(text);
        main_ = "word/document.xml";
        // The main part is named in the package's relationships.
        if (archive_->read("_rels/.rels", text, 4 * 1024 * 1024)) {
            XmlPull x(text);
            for (auto e = x.next(); e != XmlPull::Eof; e = x.next())
                if (e == XmlPull::Start && x.local() == "Relationship" &&
                    lib::ends_with(x.attr("Type"), "/officeDocument"))
                    main_ = archive_resolve("", x.attr("Target"));
        }
        std::string rels_path = main_;
        const size_t slash = rels_path.rfind('/');
        rels_path = (slash == std::string::npos ? "" : rels_path.substr(0, slash + 1)) + "_rels/" +
                    lib::file_name(main_) + ".rels";
        read_rels(rels_path, rels_);
        if (archive_->read(archive_resolve(main_, "styles.xml"), text, 32 * 1024 * 1024)) styles(text);
        if (archive_->read(archive_resolve(main_, "numbering.xml"), text, 32 * 1024 * 1024)) numbering(text);

        if (archive_->size_of(main_) > 96ull * 1024 * 1024) {
            error = "This document is too large to open on the console.";
            return false;
        }
        if (!archive_->read(main_, text, 96ull * 1024 * 1024)) {
            error = "This DOCX file has no document text (word/document.xml).";
            return false;
        }
        {
            XmlPull x(text);
            for (auto e = x.next(); e != XmlPull::Eof; e = x.next())
                if (e == XmlPull::Start && x.name() == "w:body") {
                    blocks(x, x.depth());
                    break;
                }
        }
        text.clear();
        text.shrink_to_fit();
        close_lists();
        notes("footnotes.xml", "w:footnote", footnotes_, "fn");
        notes("endnotes.xml", "w:endnote", endnotes_, "en");
        out_.finish(book_.sections);
        if (archive_->has("docProps/thumbnail.jpeg")) book_.cover = archive_image_key(path_, "docProps/thumbnail.jpeg");
        else if (archive_->has("docProps/thumbnail.png")) book_.cover = archive_image_key(path_, "docProps/thumbnail.png");
        LOG("docx: %zu sections, %zu headings, %d pictures", book_.sections.size(), book_.toc.size(), pictures_);
        return true;
    }

private:
    struct Rel {
        std::string target;
        bool external = false;
    };

    void core_properties(const std::string& text) {
        XmlPull x(text);
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e != XmlPull::Start) continue;
            if (x.name() == "dc:title") book_.title = lib::collapse_space(element_text(x));
            else if (x.name() == "dc:creator") book_.author = lib::collapse_space(element_text(x));
        }
    }

    static std::string element_text(XmlPull& x) {
        std::string s;
        const int d = x.depth();
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::Text) s += x.text();
            if (e == XmlPull::End && x.depth() == d) break;
        }
        return s;
    }

    void read_rels(const std::string& path, std::map<std::string, Rel>& out) {
        std::string text;
        if (!archive_->read(path, text, 8 * 1024 * 1024)) return;
        XmlPull x(text);
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e != XmlPull::Start || x.local() != "Relationship") continue;
            Rel r;
            r.external = x.attr("TargetMode") == "External";
            r.target = r.external ? x.attr("Target") : archive_resolve(main_, x.attr("Target"));
            out[x.attr("Id")] = r;
        }
    }

    void styles(const std::string& text) {
        XmlPull x(text);
        DocxStyle* cur = nullptr;
        bool in_rpr = false, in_ppr = false;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End) {
                if (x.name() == "w:style") cur = nullptr;
                if (x.name() == "w:rPr") in_rpr = false;
                if (x.name() == "w:pPr") in_ppr = false;
                continue;
            }
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:style") {
                cur = &styles_[x.attr("w:styleId")];
                continue;
            }
            if (!cur) continue;
            if (n == "w:name") cur->name = lib::lower(x.attr("w:val"));
            else if (n == "w:basedOn") cur->based_on = x.attr("w:val");
            else if (n == "w:rPr") in_rpr = true;
            else if (n == "w:pPr") in_ppr = true;
            else if (n == "w:outlineLvl" && in_ppr) cur->outline = atoi(x.attr("w:val").c_str());
            else if (n == "w:numId" && in_ppr) cur->num_id = x.attr("w:val");
            else if (n == "w:ilvl" && in_ppr) cur->ilvl = atoi(x.attr("w:val").c_str());
            else if (n == "w:b" && in_rpr) cur->bold = on_off(x);
            else if (n == "w:i" && in_rpr) cur->italic = on_off(x);
        }
    }

    void numbering(const std::string& text) {
        XmlPull x(text);
        std::string abstract_id, num_id;
        int level = -1;
        bool in_num = false;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End) {
                if (x.name() == "w:abstractNum") abstract_id.clear();
                if (x.name() == "w:num") in_num = false;
                if (x.name() == "w:lvl" || x.name() == "w:lvlOverride") level = -1;
                continue;
            }
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:abstractNum") abstract_id = x.attr("w:abstractNumId");
            else if (n == "w:lvl") level = atoi(x.attr("w:ilvl").c_str());
            else if (n == "w:numFmt" && !abstract_id.empty() && level >= 0 && level < 9)
                abstract_[abstract_id][level].format = x.attr("w:val");
            else if (n == "w:start" && !abstract_id.empty() && level >= 0 && level < 9)
                abstract_[abstract_id][level].start = atoi(x.attr("w:val").c_str());
            else if (n == "w:num") {
                num_id = x.attr("w:numId");
                in_num = true;
            } else if (n == "w:abstractNumId" && in_num) {
                num_abstract_[num_id] = x.attr("w:val");
            } else if (n == "w:lvlOverride" && in_num) {
                level = atoi(x.attr("w:ilvl").c_str());
            } else if (n == "w:startOverride" && in_num && level >= 0 && level < 9) {
                start_override_[num_id][level] = atoi(x.attr("w:val").c_str());
            }
        }
    }

    static int on_off(const XmlPull& x) {
        const std::string v = x.attr("w:val");
        return v.empty() || (v != "0" && v != "false" && v != "off" && v != "none") ? 1 : 0;
    }

    // Style properties, following the basedOn chain.
    template <typename F>
    void style_chain(const std::string& id, F visit) const {
        std::string cur = id;
        for (int guard = 0; guard < 12 && !cur.empty(); ++guard) {
            auto it = styles_.find(cur);
            if (it == styles_.end()) return;
            if (visit(it->second)) return;
            cur = it->second.based_on;
        }
    }

    int heading_level(const std::string& style) const {
        int level = 0;
        style_chain(style, [&](const DocxStyle& s) {
            if (s.name == "title") level = 1;
            else if (lib::starts_with(s.name, "heading ") && s.name.size() > 8 && isdigit((unsigned char)s.name[8]))
                level = atoi(s.name.c_str() + 8);
            else if (s.outline >= 0 && s.outline < 9) level = s.outline + 1;
            return level > 0;
        });
        return level;
    }

    bool quote_style(const std::string& style) const {
        bool quote = false;
        style_chain(style, [&](const DocxStyle& s) { return quote = s.name.find("quote") != std::string::npos; });
        return quote;
    }

    int style_flag(const std::string& style, int DocxStyle::*field) const {
        int v = -1;
        style_chain(style, [&](const DocxStyle& s) { return (v = s.*field) >= 0; });
        return v;
    }

    DocxLevel list_level(const std::string& num_id, int ilvl) const {
        DocxLevel l;
        auto a = num_abstract_.find(num_id);
        if (a != num_abstract_.end()) {
            auto levels = abstract_.find(a->second);
            if (levels != abstract_.end()) {
                auto found = levels->second.find(ilvl);
                if (found != levels->second.end()) l = found->second;
            }
        }
        auto o = start_override_.find(num_id);
        if (o != start_override_.end() && o->second.count(ilvl)) l.start = o->second.at(ilvl);
        return l;
    }

    // ---- blocks ----

    // Paragraphs and tables until the end of the element at `depth`.
    void blocks(XmlPull& x, int depth) {
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) return;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:p") paragraph(x);
            else if (n == "w:tbl") table(x);
            else if (n == "w:sectPr" || n == "w:sdtPr" || n == "w:sdtEndPr" || n == "w:del" || n == "w:moveFrom" ||
                     n == "mc:Fallback" || n == "w:tblPr")
                x.skip();
            else if (n == "w:bookmarkStart") bookmark(x);
            // Containers (w:sdt, w:sdtContent, w:customXml, w:ins ...): their children are read in turn.
        }
    }

    void bookmark(XmlPull& x) {
        const std::string name = x.attr("w:name");
        if (!name.empty() && name != "_GoBack") pending_ids_.push_back(name);
    }

    struct Para {
        std::string style, num_id, align;
        int ilvl = -1, outline = -1;
        bool has_num = false;
        std::string html, text;
        bool content = false;
    };

    void paragraph(XmlPull& x) {
        Para p;
        const int depth = x.depth();
        para_ = &p;
        inline_content(x, depth);
        para_ = nullptr;
        // Links left open by a field that ends in a later paragraph are closed here.
        for (auto& f : fields_)
            if (f.link_open) {
                p.html += "</a>";
                f.link_open = false;
            }
        emit(p);
    }

    void ppr(XmlPull& x, Para& p) {
        const int depth = x.depth();
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) return;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:rPr" || n == "w:pPrChange" || n == "w:sectPr" || n == "w:tabs") x.skip();
            else if (n == "w:pStyle") p.style = x.attr("w:val");
            else if (n == "w:numId") {
                p.num_id = x.attr("w:val");
                p.has_num = true;
            } else if (n == "w:ilvl") p.ilvl = atoi(x.attr("w:val").c_str());
            else if (n == "w:jc") p.align = x.attr("w:val");
            else if (n == "w:outlineLvl") p.outline = atoi(x.attr("w:val").c_str());
        }
    }

    // Runs, links, fields and bookmarks inside a paragraph (or a hyperlink in one).
    void inline_content(XmlPull& x, int depth) {
        Para& p = *para_;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) return;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:pPr") ppr(x, p);
            else if (n == "w:r") run(x);
            else if (n == "w:hyperlink") {
                std::string href;
                const std::string id = x.attr("r:id"), anchor = x.attr("w:anchor");
                auto rel = rels_.find(id);
                if (rel != rels_.end() && rel->second.external) href = rel->second.target;
                else if (!anchor.empty()) href = "#" + anchor;
                if (!href.empty()) p.html += "<a href=\"" + html_escape(href) + "\">";
                inline_content(x, x.depth());
                if (!href.empty()) p.html += "</a>";
            } else if (n == "w:fldSimple") {
                const std::string href = hyperlink_from_field(x.attr("w:instr"));
                if (!href.empty()) p.html += "<a href=\"" + html_escape(href) + "\">";
                inline_content(x, x.depth());
                if (!href.empty()) p.html += "</a>";
            } else if (n == "w:bookmarkStart") {
                const std::string name = x.attr("w:name");
                if (!name.empty() && name != "_GoBack") {
                    p.html += "<a id=\"" + html_escape(name) + "\"></a>";
                    para_ids_.push_back(name);
                }
            } else if (n == "m:oMath" || n == "m:oMathPara") {
                math(x);
            } else if (n == "w:del" || n == "w:moveFrom" || n == "mc:Fallback" || n == "w:sdtPr" ||
                       n == "w:proofErr" || n == "w:rPrChange") {
                x.skip();
            }
            // w:ins, w:smartTag, w:sdt, w:sdtContent, w:customXml, mc:AlternateContent ...: descend.
        }
    }

    // Office Math: its text, in italics.
    void math(XmlPull& x) {
        const int depth = x.depth();
        std::string text;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e == XmlPull::Start && x.name() == "m:t") text += element_text(x);
        }
        if (!text.empty()) {
            para_->html += "<i>" + html_escape(text) + "</i>";
            para_->text += text;
            para_->content = true;
        }
    }

    void run(XmlPull& x) {
        Para& p = *para_;
        const int depth = x.depth();
        int bold = -1, italic = -1;
        Format f;
        std::string rstyle;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) return;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:rPr") {
                const int d = x.depth();
                for (auto r = x.next(); r != XmlPull::Eof; r = x.next()) {
                    if (r == XmlPull::End && x.depth() == d) break;
                    if (r != XmlPull::Start) continue;
                    const std::string_view m = x.name();
                    if (m == "w:rPrChange") x.skip();
                    else if (m == "w:b") bold = on_off(x);
                    else if (m == "w:i") italic = on_off(x);
                    else if (m == "w:u") f.underline = on_off(x) && x.attr("w:val") != "none";
                    else if (m == "w:strike" || m == "w:dstrike") f.strike = on_off(x);
                    else if (m == "w:rStyle") rstyle = x.attr("w:val");
                    else if (m == "w:vertAlign") {
                        const std::string v = x.attr("w:val");
                        f.position = v == "superscript" ? 1 : v == "subscript" ? -1 : 0;
                    }
                }
                // Character style, then the paragraph's style (not for headings,
                // which the reader sets in bold anyway).
                if (bold < 0) bold = style_flag(rstyle, &DocxStyle::bold);
                if (italic < 0) italic = style_flag(rstyle, &DocxStyle::italic);
                if (bold < 0) bold = style_flag(p.style, &DocxStyle::bold);
                if (italic < 0) italic = style_flag(p.style, &DocxStyle::italic);
                f.bold = bold > 0;
                f.italic = italic > 0;
            } else if (n == "w:t") {
                const std::string t = element_text(x);
                if (t.empty()) continue;
                p.html += f.open() + html_escape(t) + f.close();
                p.text += t;
                p.content = true;
            } else if (n == "w:tab" || n == "w:ptab") {
                p.html += " ";
                p.text += " ";
            } else if (n == "w:br" || n == "w:cr") {
                if (x.attr("w:type") != "page" && x.attr("w:type") != "column") p.html += "<br>";
            } else if (n == "w:noBreakHyphen") {
                p.html += "-";
                p.text += "-";
            } else if (n == "w:drawing") {
                drawing(x);
            } else if (n == "w:pict" || n == "w:object") {
                vml_picture(x);
            } else if (n == "w:footnoteReference" || n == "w:endnoteReference") {
                const bool foot = n == "w:footnoteReference";
                const std::string id = x.attr("w:id");
                auto& list = foot ? footnotes_ : endnotes_;
                const int number = (int)(footnotes_.size() + endnotes_.size()) + 1;
                list.push_back({id, number});
                const std::string anchor = std::string(foot ? "fn" : "en") + "-" + id;
                p.html += "<sup><a href=\"#" + anchor + "\">" + std::to_string(number) + "</a></sup>";
            } else if (n == "w:fldChar") {
                const std::string type = x.attr("w:fldCharType");
                if (type == "begin") fields_.push_back(Field());
                else if (type == "separate" && !fields_.empty()) {
                    Field& fl = fields_.back();
                    const std::string href = hyperlink_from_field(fl.instr);
                    if (!href.empty()) {
                        p.html += "<a href=\"" + html_escape(href) + "\">";
                        fl.link_open = true;
                    }
                } else if (type == "end" && !fields_.empty()) {
                    if (fields_.back().link_open) p.html += "</a>";
                    fields_.pop_back();
                }
            } else if (n == "w:instrText") {
                const std::string t = element_text(x);
                if (!fields_.empty()) fields_.back().instr += t;
            } else if (n == "w:delText" || n == "mc:Fallback" || n == "w:rPrChange") {
                x.skip();
            }
        }
    }

    // DrawingML picture: a:blip r:embed, sized by wp:extent (EMU).
    void drawing(XmlPull& x) {
        const int depth = x.depth();
        long long cx = 0, cy = 0;
        std::string embed, alt;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "wp:extent") {
                cx = atoll(x.attr("cx").c_str());
                cy = atoll(x.attr("cy").c_str());
            } else if (n == "wp:docPr") {
                alt = x.attr("descr");
            } else if (n == "a:blip" && embed.empty()) {
                embed = x.attr("r:embed");
            } else if (n == "mc:Fallback") {
                x.skip();
            }
        }
        picture(embed, (int)(cx / 9525), (int)(cy / 9525), alt);
    }

    // Legacy VML: v:imagedata r:id, sized by the shape's style ("width:120pt;height:80pt").
    void vml_picture(XmlPull& x) {
        const int depth = x.depth();
        std::string id, style;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            if (x.name() == "v:shape" && style.empty()) style = x.attr("style");
            if (x.name() == "v:imagedata" && id.empty()) id = x.attr("r:id");
        }
        auto length = [&](const char* key) {
            const size_t at = style.find(key);
            if (at == std::string::npos) return 0;
            const double v = atof(style.c_str() + at + strlen(key));
            const size_t unit = style.find_first_not_of("0123456789.", at + strlen(key));
            const std::string u = unit == std::string::npos ? "" : style.substr(unit, 2);
            return (int)(u == "pt" ? v * 96 / 72 : u == "in" ? v * 96 : u == "cm" ? v * 96 / 2.54 : v);
        };
        picture(id, length("width:"), length("height:"), "");
    }

    void picture(const std::string& rel_id, int w, int h, const std::string& alt) {
        auto rel = rels_.find(rel_id);
        if (rel == rels_.end() || rel->second.external) return;
        // Windows metafiles and TIFF cannot be drawn: their description is kept.
        if (!picture_ext(rel->second.target)) {
            if (!alt.empty()) para_->html += "<i>[" + html_escape(alt) + "]</i>";
            return;
        }
        para_->html += img_tag(archive_image_key(path_, rel->second.target), w, h, alt);
        para_->content = true;
        ++pictures_;
    }

    // ---- writing blocks ----

    struct OpenList {
        std::string num_id;
        int level = 0;
        bool ordered = false;
    };

    void close_lists(size_t keep = 0) {
        while (lists_.size() > keep) {
            out_.html() += lists_.back().ordered ? "</li></ol>" : "</li></ul>";
            lists_.pop_back();
        }
    }

    void register_ids(std::vector<std::string>& ids) {
        for (const std::string& id : ids) book_.anchors[id] = out_.section();
        ids.clear();
    }

    void emit(Para& p) {
        std::string& html = out_.html();
        // Numbering: on the paragraph, or from its style.
        std::string num_id = p.num_id;
        int ilvl = p.ilvl;
        if (!p.has_num) {
            style_chain(p.style, [&](const DocxStyle& s) {
                if (s.num_id.empty()) return false;
                num_id = s.num_id;
                if (ilvl < 0) ilvl = s.ilvl;
                return true;
            });
        }
        if (ilvl < 0) ilvl = 0;
        ilvl = std::min(ilvl, 8);
        int heading = p.outline >= 0 && p.outline < 9 ? p.outline + 1 : heading_level(p.style);
        const DocxLevel lvl = list_level(num_id, ilvl);
        const bool listed = !num_id.empty() && num_id != "0" && lvl.format != "none" && heading == 0;

        if (table_depth_ > 0) {
            // Inside a table cell: plain paragraphs (the converter flattens cells).
            if (!p.content && p.html.empty()) return;
            html += "<p>" + std::string(listed ? "\xE2\x80\xA2 " : "") + p.html + "</p>";
            register_ids(pending_ids_);
            register_ids(para_ids_);
            return;
        }
        if (!p.content && p.html.find("<a id=") == std::string::npos) {
            // Empty paragraphs are Word's spacing; the reader has its own.
            return;
        }
        const std::string align = p.align == "center" ? " style=\"text-align:center\"" : "";
        if (heading > 0) {
            close_lists();
            heading = clamp_heading(heading);
            out_.boundary(heading);
            const std::string id = "h-" + std::to_string(++headings_);
            book_.anchors[id] = out_.section();
            register_ids(pending_ids_);
            register_ids(para_ids_);
            const std::string title = lib::collapse_space(p.text);
            if (!title.empty()) book_.toc.push_back({title, id, out_.section(), heading});
            out_.html() += "<h" + std::to_string(heading) + " id=\"" + id + "\"" + align + ">" + p.html + "</h" +
                           std::to_string(heading) + ">\n";
            return;
        }
        if (listed) {
            list_item(num_id, ilvl, lvl, p.html);
            register_ids(pending_ids_);
            register_ids(para_ids_);
            return;
        }
        close_lists();
        out_.boundary();
        register_ids(pending_ids_);
        register_ids(para_ids_);
        if (quote_style(p.style)) out_.html() += "<blockquote><p>" + p.html + "</p></blockquote>\n";
        else out_.html() += "<p" + align + ">" + p.html + "</p>\n";
    }

    void list_item(const std::string& num_id, int ilvl, const DocxLevel& lvl, const std::string& content) {
        if (lists_.empty()) out_.boundary();
        // Another list at the same depth ends the current one.
        while (!lists_.empty() && (lists_.size() > (size_t)ilvl + 1 ||
                                   (lists_.size() == (size_t)ilvl + 1 && lists_.back().num_id != num_id)))
            close_lists(lists_.size() - 1);
        auto& counters = counters_[num_id];
        if (counters.size() < 9) counters.assign(9, 0);
        // Deeper levels restart after an item at this level.
        for (int k = ilvl + 1; k < 9; ++k) counters[k] = 0;
        const int number = counters[ilvl] ? counters[ilvl] + 1 : lvl.start;
        counters[ilvl] = number;
        std::string& html = out_.html();
        if (lists_.size() == (size_t)ilvl + 1) {
            html += "</li><li>" + content;
            return;
        }
        while (lists_.size() < (size_t)ilvl + 1) {
            const int at = (int)lists_.size();
            const DocxLevel l = at == ilvl ? lvl : list_level(num_id, at);
            const bool ordered = l.format != "bullet";
            std::string type;
            if (l.format == "lowerLetter") type = "a";
            else if (l.format == "upperLetter") type = "A";
            else if (l.format == "lowerRoman") type = "i";
            else if (l.format == "upperRoman") type = "I";
            if (ordered) {
                html += "<ol" + (type.empty() ? std::string() : " type=\"" + type + "\"") + " start=\"" +
                        std::to_string(at == ilvl ? number : std::max(1, counters[at])) + "\"><li>";
            } else {
                html += "<ul><li>";
            }
            lists_.push_back({num_id, at, ordered});
        }
        html += content;
    }

    void table(XmlPull& x) {
        if (table_depth_ == 0) {
            close_lists();
            out_.boundary();
        }
        ++table_depth_;
        const int depth = x.depth();
        out_.html() += "<table>";
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:tblPr" || n == "w:tblGrid" || n == "w:trPr" || n == "w:tblPrEx" || n == "w:del") x.skip();
            else if (n == "w:tr") {
                out_.html() += "<tr>";
                const int rd = x.depth();
                for (auto r = x.next(); r != XmlPull::Eof; r = x.next()) {
                    if (r == XmlPull::End && x.depth() == rd) break;
                    if (r != XmlPull::Start) continue;
                    if (x.name() == "w:tc") cell(x);
                    else if (x.name() == "w:trPr" || x.name() == "w:tblPrEx") x.skip();
                }
                out_.html() += "</tr>";
            }
        }
        out_.html() += "</table>\n";
        --table_depth_;
    }

    void cell(XmlPull& x) {
        const int depth = x.depth();
        int span = 1;
        bool merged = false;
        std::string& html = out_.html();
        const size_t at = html.size();
        html += "<td>";
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "w:tcPr") {
                const int d = x.depth();
                for (auto c = x.next(); c != XmlPull::Eof; c = x.next()) {
                    if (c == XmlPull::End && x.depth() == d) break;
                    if (c != XmlPull::Start) continue;
                    if (x.name() == "w:gridSpan") span = std::max(1, atoi(x.attr("w:val").c_str()));
                    if (x.name() == "w:vMerge") merged = x.attr("w:val") != "restart";
                }
            } else if (n == "w:p") {
                paragraph(x);
            } else if (n == "w:tbl") {
                table(x);
            } else if (n == "w:sdtPr") {
                x.skip();
            }
        }
        std::string& h = out_.html();
        if (merged) h.resize(at + 4);  // the cell above continues: an empty cell keeps the columns
        if (span > 1) h.replace(at, 4, "<td colspan=\"" + std::to_string(span) + "\">");
        h += "</td>";
    }

    // Footnotes and endnotes, after the text, in the order they were referred to.
    void notes(const std::string& part, const char* element, const std::vector<std::pair<std::string, int>>& refs,
               const char* prefix) {
        if (refs.empty()) return;
        std::string text;
        if (!archive_->read(archive_resolve(main_, part), text, 32 * 1024 * 1024)) return;
        std::map<std::string, int> wanted;
        for (const auto& [id, number] : refs) wanted[id] = number;
        // Pictures in notes have their own relationships.
        std::map<std::string, Rel> saved = rels_;
        read_rels(archive_resolve(main_, "_rels/" + part + ".rels"), rels_);
        close_lists();
        out_.boundary();
        if (!notes_heading_) {
            out_.html() += "<hr><h2 id=\"notes\">Notes</h2>\n";
            book_.anchors["notes"] = out_.section();
            book_.toc.push_back({"Notes", "notes", out_.section(), 1});
            notes_heading_ = true;
        }
        XmlPull x(text);
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e != XmlPull::Start || x.name() != element) continue;
            auto found = wanted.find(x.attr("w:id"));
            if (found == wanted.end()) {
                x.skip();
                continue;
            }
            const std::string anchor = std::string(prefix) + "-" + found->first;
            out_.html() += "<p id=\"" + anchor + "\"><b>" + std::to_string(found->second) + ".</b></p>";
            book_.anchors[anchor] = out_.section();
            blocks(x, x.depth());
            close_lists();
            out_.boundary();
        }
        rels_ = saved;
    }

    struct Field {
        std::string instr;
        bool link_open = false;
    };

    std::string path_, main_;
    HtmlBook& book_;
    std::shared_ptr<Archive> archive_;
    std::map<std::string, Rel> rels_;
    std::map<std::string, DocxStyle> styles_;
    std::map<std::string, std::map<int, DocxLevel>> abstract_;
    std::map<std::string, std::string> num_abstract_;
    std::map<std::string, std::map<int, int>> start_override_;
    std::map<std::string, std::vector<int>> counters_;
    std::vector<OpenList> lists_;
    std::vector<Field> fields_;
    std::vector<std::string> pending_ids_, para_ids_;
    std::vector<std::pair<std::string, int>> footnotes_, endnotes_;
    HtmlSections out_;
    Para* para_ = nullptr;
    int table_depth_ = 0, headings_ = 0, pictures_ = 0;
    bool notes_heading_ = false;
};

// ---- ODT ------------------------------------------------------------------------------------

struct OdtStyle {
    std::string parent, display, list_style;
    int outline = 0;
    int bold = -1, italic = -1, underline = -1, strike = -1, position = 0;
    bool mono = false;
};

struct OdtListLevel {
    bool numbered = false;
    std::string format = "1";
    int start = 1;
};

// "2.5cm", "1in", "72pt", "300px" -> pixels at 96 dpi.
int odf_length(const std::string& s) {
    const double v = atof(s.c_str());
    if (lib::ends_with(s, "cm")) return (int)lround(v * 96 / 2.54);
    if (lib::ends_with(s, "mm")) return (int)lround(v * 96 / 25.4);
    if (lib::ends_with(s, "in")) return (int)lround(v * 96);
    if (lib::ends_with(s, "pt")) return (int)lround(v * 96 / 72);
    if (lib::ends_with(s, "pc")) return (int)lround(v * 16);
    return (int)lround(v);
}

class Odt {
public:
    Odt(const std::string& path, HtmlBook& book) : path_(path), book_(book) {}

    bool run(std::string& error) {
        archive_ = open_archive(path_, &error);
        std::string text;
        if (!archive_ || !archive_->read("content.xml", text, 96ull * 1024 * 1024)) {
            error = "This document could not be opened: it is not an ODT file, or it is damaged or encrypted.";
            return false;
        }
        std::string styles_xml, meta;
        if (archive_->read("styles.xml", styles_xml, 32 * 1024 * 1024)) styles(styles_xml);
        if (archive_->read("meta.xml", meta, 4 * 1024 * 1024)) metadata(meta, book_.title, book_.author);
        styles(text);  // automatic styles
        {
            XmlPull x(text);
            for (auto e = x.next(); e != XmlPull::Eof; e = x.next())
                if (e == XmlPull::Start && x.name() == "office:text") {
                    blocks(x, x.depth());
                    break;
                }
        }
        if (!notes_.empty()) {
            out_.boundary();
            out_.html() += "<hr><h2 id=\"notes\">Notes</h2>\n";
            book_.anchors["notes"] = out_.section();
            book_.toc.push_back({"Notes", "notes", out_.section(), 1});
            for (auto& [anchor, html] : notes_) {
                out_.boundary();
                book_.anchors[anchor] = out_.section();
                out_.html() += "<div id=\"" + anchor + "\">" + html + "</div>\n";
            }
        }
        out_.finish(book_.sections);
        if (archive_->has("Thumbnails/thumbnail.png")) book_.cover = archive_image_key(path_, "Thumbnails/thumbnail.png");
        LOG("odt: %zu sections, %zu headings, %d pictures", book_.sections.size(), book_.toc.size(), pictures_);
        return true;
    }

    static void metadata(const std::string& meta, std::string& title, std::string& author) {
        XmlPull x(meta);
        std::string initial;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e != XmlPull::Start) continue;
            if (x.name() == "dc:title") title = text_of(x);
            else if (x.name() == "meta:initial-creator") initial = text_of(x);
            else if (x.name() == "dc:creator") author = text_of(x);
        }
        if (!initial.empty()) author = initial;
    }

private:
    static std::string text_of(XmlPull& x) {
        std::string s;
        const int d = x.depth();
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::Text) s += x.text();
            if (e == XmlPull::End && x.depth() == d) break;
        }
        return lib::collapse_space(s);
    }

    void styles(const std::string& text) {
        XmlPull x(text);
        OdtStyle* cur = nullptr;
        std::string list_name;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End) {
                if (x.name() == "style:style") cur = nullptr;
                if (x.name() == "text:list-style") list_name.clear();
                continue;
            }
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "style:style") {
                cur = &styles_[x.attr("style:name")];
                cur->parent = x.attr("style:parent-style-name");
                cur->display = lib::lower(x.attr("style:display-name"));
                if (cur->display.empty()) cur->display = lib::lower(lib::replace_all(x.attr("style:name"), "_20_", " "));
                cur->list_style = x.attr("style:list-style-name");
                cur->outline = atoi(x.attr("style:default-outline-level").c_str());
            } else if (n == "style:text-properties" && cur) {
                const std::string weight = x.attr("fo:font-weight");
                if (!weight.empty()) cur->bold = weight == "bold" || atoi(weight.c_str()) >= 600;
                const std::string style = x.attr("fo:font-style");
                if (!style.empty()) cur->italic = style == "italic" || style == "oblique";
                const std::string under = x.attr("style:text-underline-style");
                if (!under.empty()) cur->underline = under != "none";
                const std::string strike = x.attr("style:text-line-through-style");
                if (!strike.empty()) cur->strike = strike != "none";
                const std::string pos = x.attr("style:text-position");
                if (lib::starts_with(pos, "super")) cur->position = 1;
                else if (lib::starts_with(pos, "sub")) cur->position = -1;
                else if (!pos.empty() && atof(pos.c_str()) > 0) cur->position = 1;
                else if (!pos.empty() && atof(pos.c_str()) < 0) cur->position = -1;
                const std::string font = lib::lower(x.attr("style:font-name"));
                if (font.find("mono") != std::string::npos || font.find("courier") != std::string::npos) cur->mono = true;
            } else if (n == "text:list-style") {
                list_name = x.attr("style:name");
            } else if ((n == "text:list-level-style-number" || n == "text:list-level-style-bullet" ||
                        n == "text:list-level-style-image") && !list_name.empty()) {
                const int level = std::max(1, std::min(10, atoi(x.attr("text:level").c_str())));
                OdtListLevel& l = lists_styles_[list_name][level];
                l.numbered = n == "text:list-level-style-number" && !x.attr("style:num-format").empty();
                l.format = x.attr("style:num-format");
                const std::string start = x.attr("text:start-value");
                l.start = start.empty() ? 1 : std::max(0, atoi(start.c_str()));
            }
        }
    }

    template <typename F>
    void chain(const std::string& name, F visit) const {
        std::string cur = name;
        for (int guard = 0; guard < 12 && !cur.empty(); ++guard) {
            auto it = styles_.find(cur);
            if (it == styles_.end()) return;
            if (visit(it->second)) return;
            cur = it->second.parent;
        }
    }

    Format format_of(const std::string& style) const {
        Format f;
        int bold = -1, italic = -1, underline = -1, strike = -1, position = 0;
        bool mono = false;
        chain(style, [&](const OdtStyle& s) {
            if (bold < 0) bold = s.bold;
            if (italic < 0) italic = s.italic;
            if (underline < 0) underline = s.underline;
            if (strike < 0) strike = s.strike;
            if (!position) position = s.position;
            mono = mono || s.mono;
            return false;
        });
        f.bold = bold > 0;
        f.italic = italic > 0;
        f.underline = underline > 0;
        f.strike = strike > 0;
        f.position = position;
        f.mono = mono;
        return f;
    }

    bool style_named(const std::string& style, const char* what) const {
        bool found = false;
        chain(style, [&](const OdtStyle& s) { return found = s.display.find(what) != std::string::npos; });
        return found;
    }

    // ---- blocks ----

    void blocks(XmlPull& x, int depth) {
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) return;
            if (e != XmlPull::Start) continue;
            block(x);
        }
    }

    // One block element (just started).
    void block(XmlPull& x) {
        const std::string_view n = x.name();
        if (n == "text:p" || n == "text:h") {
            paragraph(x);
        } else if (n == "text:list") {
            if (lists_open_ == 0) out_.boundary();
            list(x);
        } else if (n == "table:table") {
            if (table_depth_ == 0) out_.boundary();
            table(x);
        } else if (n == "draw:frame" || n == "draw:a") {
            // A picture anchored to the page.
            std::string html;
            frame(x, html);
            if (!html.empty()) {
                out_.boundary();
                out_.html() += "<p>" + html + "</p>\n";
            }
        } else if (n == "text:sequence-decls" || n == "office:forms" || n == "text:tracked-changes" ||
                   n == "text:table-of-content-source" || n == "text:alphabetical-index-source" ||
                   n == "office:annotation" || n == "text:variable-decls" || n == "text:user-field-decls" ||
                   n == "table:table-columns" || n == "table:table-column" || n == "text:soft-page-break") {
            x.skip();
        } else {
            // text:section, text:table-of-content, text:index-body ...: their blocks in turn.
            blocks(x, x.depth());
        }
    }

    void paragraph(XmlPull& x) {
        const bool is_h = x.name() == "text:h";
        const std::string style = x.attr("text:style-name");
        int level = 0;
        if (is_h) {
            level = atoi(x.attr("text:outline-level").c_str());
            if (level <= 0) chain(style, [&](const OdtStyle& s) { return (level = s.outline) > 0; });
            if (level <= 0) level = 1;
        } else if (style_named(style, "title") && !style_named(style, "subtitle")) {
            level = 1;
        }
        std::string text;
        std::vector<std::string> ids;
        std::string html = inline_content(x, x.depth(), text, ids);
        const bool empty = lib::trim(text).empty() && html.find("<img") == std::string::npos;
        if (lists_open_ > 0 && table_depth_ == 0 && level > 0 && !empty) {
            // A numbered heading (a heading inside a list): still a heading, in the list item.
            level = clamp_heading(level);
            const std::string id = "h-" + std::to_string(++headings_);
            book_.anchors[id] = out_.section();
            const std::string title = lib::collapse_space(text);
            if (!title.empty()) book_.toc.push_back({title, id, out_.section(), level});
            item_html_ += "<h" + std::to_string(level) + " id=\"" + id + "\">" + html + "</h" + std::to_string(level) + ">";
            for (auto& i : ids) pending_ids_.push_back(i);
            return;
        }
        if (lists_open_ > 0 || table_depth_ > 0) {
            // Inside a list item or a table cell: the enclosing element is written by the caller.
            if (!empty || !ids.empty()) {
                if (!item_html_.empty()) item_html_ += "<br>";
                item_html_ += html;
            }
            for (auto& id : ids) pending_ids_.push_back(id);
            return;
        }
        if (empty && ids.empty()) return;
        if (level > 0) {
            level = clamp_heading(level);
            out_.boundary(level);
            const std::string id = "h-" + std::to_string(++headings_);
            book_.anchors[id] = out_.section();
            for (auto& i : ids) book_.anchors[i] = out_.section();
            const std::string title = lib::collapse_space(text);
            if (!title.empty()) book_.toc.push_back({title, id, out_.section(), level});
            out_.html() += "<h" + std::to_string(level) + " id=\"" + id + "\">" + html + "</h" + std::to_string(level) + ">\n";
            return;
        }
        out_.boundary();
        for (auto& i : ids) book_.anchors[i] = out_.section();
        if (style_named(style, "preformatted")) out_.html() += "<pre>" + html + "</pre>\n";
        else if (style_named(style, "quot")) out_.html() += "<blockquote><p>" + html + "</p></blockquote>\n";
        else out_.html() += "<p>" + html + "</p>\n";
    }

    // Text, spans, links, notes, pictures until the end of the element at `depth`.
    std::string inline_content(XmlPull& x, int depth, std::string& text, std::vector<std::string>& ids) {
        std::string html;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e == XmlPull::Text) {
                html += html_escape(x.text());
                text += x.text();
                continue;
            }
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "text:span") {
                const Format f = format_of(x.attr("text:style-name"));
                html += f.open() + inline_content(x, x.depth(), text, ids) + f.close();
            } else if (n == "text:a") {
                std::string href = x.attr("xlink:href");
                // Internal targets: "#name" or "#name|outline".
                if (lib::starts_with(href, "#")) href = href.substr(0, href.find('|'));
                html += "<a href=\"" + html_escape(href) + "\">" + inline_content(x, x.depth(), text, ids) + "</a>";
            } else if (n == "text:s") {
                html += " ";
                text += " ";
                x.skip();
            } else if (n == "text:tab") {
                html += " ";
                text += " ";
                x.skip();
            } else if (n == "text:line-break") {
                html += "<br>";
                x.skip();
            } else if (n == "text:bookmark" || n == "text:bookmark-start" || n == "text:reference-mark" ||
                       n == "text:reference-mark-start") {
                const std::string name = x.attr("text:name");
                if (!name.empty()) {
                    html += "<a id=\"" + html_escape(name) + "\"></a>";
                    ids.push_back(name);
                }
                x.skip();
            } else if (n == "text:note") {
                html += note(x);
            } else if (n == "draw:frame" || n == "draw:a") {
                frame(x, html);
            } else if (n == "office:annotation" || n == "text:soft-page-break" || n == "text:bookmark-end" ||
                       n == "text:reference-mark-end" || n == "text:change" || n == "text:change-start" ||
                       n == "text:change-end") {
                x.skip();
            } else {
                // Fields and anything else: their text.
                html += inline_content(x, x.depth(), text, ids);
            }
        }
        return html;
    }

    std::string note(XmlPull& x) {
        const int depth = x.depth();
        std::string citation, body;
        const std::string anchor = "note-" + std::to_string(notes_.size() + 1);
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            if (x.name() == "text:note-citation") {
                std::string t;
                std::vector<std::string> ids;
                inline_content(x, x.depth(), t, ids);
                citation = lib::trim(t);
            } else if (x.name() == "text:note-body") {
                const int d = x.depth();
                for (auto b = x.next(); b != XmlPull::Eof; b = x.next()) {
                    if (b == XmlPull::End && x.depth() == d) break;
                    if (b != XmlPull::Start) continue;
                    if (x.name() == "text:p" || x.name() == "text:h") {
                        std::string t;
                        std::vector<std::string> ids;
                        const std::string p = inline_content(x, x.depth(), t, ids);
                        body += "<p>" + (body.empty() ? "<b>" + html_escape(citation) + ".</b> " : std::string()) + p + "</p>";
                    } else {
                        x.skip();
                    }
                }
            }
        }
        if (citation.empty()) citation = std::to_string(notes_.size() + 1);
        notes_.push_back({anchor, body});
        return "<sup><a href=\"#" + anchor + "\">" + html_escape(citation) + "</a></sup>";
    }

    // draw:frame with draw:image (a picture) or draw:text-box (its paragraphs, inline).
    void frame(XmlPull& x, std::string& html) {
        const int depth = x.depth();
        int w = odf_length(x.attr("svg:width")), h = odf_length(x.attr("svg:height"));
        std::string alt = x.attr("draw:name");
        bool done = false;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "draw:frame") {
                w = odf_length(x.attr("svg:width"));
                h = odf_length(x.attr("svg:height"));
            } else if (n == "draw:image" && !done) {
                const std::string href = x.attr("xlink:href");
                // Embedded objects and their replacement pictures (charts, formulas) are left out.
                if (!href.empty() && !lib::starts_with(href, "http") && picture_ext(href)) {
                    html += img_tag(archive_image_key(path_, archive_resolve("", href)), w, h, "");
                    ++pictures_;
                    done = true;
                }
                x.skip();
            } else if (n == "svg:desc" || n == "svg:title") {
                x.skip();
            } else if (n == "text:p" || n == "text:h") {
                std::string t;
                std::vector<std::string> ids;
                const std::string p = inline_content(x, x.depth(), t, ids);
                if (!lib::trim(t).empty()) html += (html.empty() ? "" : "<br>") + p;
            }
        }
        (void)alt;
    }

    void list(XmlPull& x) {
        const int depth = x.depth();
        std::string style = x.attr("text:style-name");
        if (style.empty() && !list_style_stack_.empty()) style = list_style_stack_.back();
        list_style_stack_.push_back(style);
        const int level = lists_open_ + 1;
        ++lists_open_;
        OdtListLevel l;
        auto ls = lists_styles_.find(style);
        if (ls != lists_styles_.end() && ls->second.count(level)) l = ls->second.at(level);
        const bool cont = x.attr("text:continue-numbering") == "true" || x.has_attr("text:continue-list");
        int number = l.start;
        const std::string key = style + "#" + std::to_string(level);
        if (cont && continued_.count(key)) number = continued_[key];
        std::string& html = out_.html();
        if (l.numbered) {
            std::string type;
            if (l.format == "a" || l.format == "A" || l.format == "i" || l.format == "I") type = l.format;
            html += "<ol" + (type.empty() ? std::string() : " type=\"" + type + "\"") + " start=\"" + std::to_string(number) + "\">";
        } else {
            html += "<ul>";
        }
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            if (x.name() == "text:list-item" || x.name() == "text:list-header") {
                const bool header = x.name() == "text:list-header";
                const int d = x.depth();
                if (!header) ++number;
                out_.html() += header ? "<li>" : "<li>";
                item_html_.clear();
                for (auto c = x.next(); c != XmlPull::Eof; c = x.next()) {
                    if (c == XmlPull::End && x.depth() == d) break;
                    if (c != XmlPull::Start) continue;
                    if (x.name() == "text:list") {
                        out_.html() += item_html_;
                        item_html_.clear();
                        list(x);
                    } else if (x.name() == "table:table") {
                        out_.html() += item_html_;
                        item_html_.clear();
                        table(x);
                    } else if (x.name() == "text:p" || x.name() == "text:h") {
                        paragraph(x);
                    } else {
                        x.skip();
                    }
                }
                out_.html() += item_html_ + "</li>";
                item_html_.clear();
                for (auto& id : pending_ids_) book_.anchors[id] = out_.section();
                pending_ids_.clear();
            }
        }
        continued_[key] = number;
        out_.html() += l.numbered ? "</ol>" : "</ul>";
        if (level == 1) out_.html() += "\n";
        --lists_open_;
        list_style_stack_.pop_back();
    }

    void table(XmlPull& x) {
        ++table_depth_;
        const int depth = x.depth();
        // Lists inside a table are written flat into its cells.
        const int saved_lists = lists_open_;
        lists_open_ = 0;
        out_.html() += "<table>";
        int skip_covered = 0;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e == XmlPull::End && x.name() == "table:table-row") out_.html() += "</tr>";
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "table:table-row") {
                out_.html() += "<tr>";
                skip_covered = 0;
            } else if (n == "table:table-cell") {
                const int span = std::max(1, atoi(x.attr("table:number-columns-spanned").c_str()));
                skip_covered = span - 1;
                cell(x, span);
            } else if (n == "table:covered-table-cell") {
                // Covered by a cell to the left (its colspan) or above (kept as an empty cell).
                if (skip_covered > 0) --skip_covered;
                else out_.html() += "<td></td>";
                x.skip();
            } else if (n == "table:table-column" || n == "table:table-columns" || n == "table:table-column-group" ||
                       n == "office:annotation") {
                x.skip();
            }
            // table:table-header-rows, table:table-rows: their rows in turn.
            if (e == XmlPull::Start && n == "table:table-row") {
                // Rows end with their own End event; closed below.
            }
        }
        out_.html() += "</table>\n";
        lists_open_ = saved_lists;
        --table_depth_;
    }

    void cell(XmlPull& x, int span) {
        const int depth = x.depth();
        std::string saved_item;
        saved_item.swap(item_html_);
        std::string content;
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e == XmlPull::End && x.depth() == depth) break;
            if (e != XmlPull::Start) continue;
            const std::string_view n = x.name();
            if (n == "text:p" || n == "text:h") {
                paragraph(x);
            } else if (n == "table:table") {
                content += item_html_;
                item_html_.clear();
                // A nested table is written in place.
                std::string& html = out_.html();
                const size_t before = html.size();
                table(x);
                content += html.substr(before);
                html.resize(before);
            } else if (n == "text:list") {
                // Items, flattened with bullets.
                const int d = x.depth();
                for (auto c = x.next(); c != XmlPull::Eof; c = x.next()) {
                    if (c == XmlPull::End && x.depth() == d) break;
                    if (c == XmlPull::Start && (x.name() == "text:p" || x.name() == "text:h")) {
                        std::string t;
                        std::vector<std::string> ids;
                        const std::string p = inline_content(x, x.depth(), t, ids);
                        item_html_ += (item_html_.empty() ? "" : "<br>") + std::string("\xE2\x80\xA2 ") + p;
                    }
                }
            } else {
                x.skip();
            }
        }
        content += item_html_;
        item_html_.swap(saved_item);
        out_.html() += (span > 1 ? "<td colspan=\"" + std::to_string(span) + "\">" : std::string("<td>")) + content + "</td>";
    }

    std::string path_;
    HtmlBook& book_;
    std::shared_ptr<Archive> archive_;
    std::map<std::string, OdtStyle> styles_;
    std::map<std::string, std::map<int, OdtListLevel>> lists_styles_;
    std::map<std::string, int> continued_;
    std::vector<std::string> list_style_stack_, pending_ids_;
    std::vector<std::pair<std::string, std::string>> notes_;
    std::string item_html_;
    HtmlSections out_;
    int lists_open_ = 0, table_depth_ = 0, headings_ = 0, pictures_ = 0;
};

}  // namespace

bool open_docx(const std::string& path, HtmlBook& out, std::string& error) {
    out = HtmlBook();
    Docx d(path, out);
    return d.run(error);
}

bool open_odt(const std::string& path, HtmlBook& out, std::string& error) {
    out = HtmlBook();
    Odt d(path, out);
    return d.run(error);
}

bool open_office(const std::string& path, HtmlBook& out, std::string& error) {
    const std::string ext = lib::file_extension(path);
    bool ok = false;
    if (ext == "docx") ok = open_docx(path, out, error);
    else if (ext == "odt") ok = open_odt(path, out, error);
    else if (ext == "rtf") ok = open_rtf(path, out, error);
    else error = "Not a word-processor document.";
    if (ok) {
        out.title = lib::trim(out.title);
        out.author = lib::trim(out.author);
    }
    return ok;
}

bool office_metadata(const std::string& path, std::string& title, std::string& author) {
    const std::string ext = lib::file_extension(path);
    if (ext == "rtf") {
        // RTF keeps its \info group near the start: only the head is read.
        return rtf_info(path, title, author);
    }
    auto archive = open_archive(path);
    if (!archive) return false;
    std::string text;
    if (ext == "odt") {
        if (!archive->read("meta.xml", text, 4 * 1024 * 1024)) return false;
        Odt::metadata(text, title, author);
        return true;
    }
    if (ext == "docx") {
        if (!archive->read("docProps/core.xml", text, 4 * 1024 * 1024)) return false;
        XmlPull x(text);
        for (auto e = x.next(); e != XmlPull::Eof; e = x.next()) {
            if (e != XmlPull::Start) continue;
            if (x.name() == "dc:title" || x.name() == "dc:creator") {
                std::string& target = x.name() == "dc:title" ? title : author;
                const int d = x.depth();
                std::string s;
                for (auto t = x.next(); t != XmlPull::Eof; t = x.next()) {
                    if (t == XmlPull::Text) s += x.text();
                    if (t == XmlPull::End && x.depth() == d) break;
                }
                target = lib::collapse_space(s);
            }
        }
        return true;
    }
    return false;
}

bool office_cover(const std::string& path, std::string& bytes) {
    const std::string ext = lib::file_extension(path);
    if (ext != "docx" && ext != "odt") return false;
    auto archive = open_archive(path);
    if (!archive) return false;
    for (const char* name : {"Thumbnails/thumbnail.png", "docProps/thumbnail.jpeg", "docProps/thumbnail.png"})
        if (archive->has(name) && archive->read(name, bytes, 8 * 1024 * 1024)) return true;
    return false;
}

}  // namespace doc
