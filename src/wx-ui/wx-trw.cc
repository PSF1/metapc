#include "wx-trw.h"
#include "wx-utils.h"

#include <wx/dir.h>
#include <wx/filename.h>
#include <wx/hyperlink.h>
#include <wx/mstream.h>
#include <wx/statline.h>
#include <wx/textfile.h>
#include <wx/wfstream.h>
#include <wx/zipstrm.h>
#if wxUSE_WEBREQUEST
#include <wx/webrequest.h>
#endif

#include <algorithm>
#include <memory>
#include <regex>
#include <string>

extern "C" {
#include "ibm.h"
#include "config.h"
#include "cpu.h"
#include "device.h"
#include "mem_bios.h"
#include "model.h"
#include "paths.h"
}

/* The Retro Web's robots.txt asks for at least this long between requests. */
#define TRW_REQUEST_INTERVAL_MS 5000
/* Boards fetched within this many days are read from the local cache. */
#define TRW_CACHE_DAYS 30
#define TRW_RESULTS_PER_PAGE 48

wxString TrwBoard::Url() const { return TRW_SITE_URL "/motherboards/s/" + slug; }

/* ---------------------------------------------------------------- HTML parsing */

static std::string decode_entities(const std::string &s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
                if (s[i] != '&') {
                        out += s[i];
                        continue;
                }
                size_t end = s.find(';', i);
                if (end == std::string::npos || end - i > 10) {
                        out += s[i];
                        continue;
                }
                std::string ent = s.substr(i + 1, end - i - 1);
                unsigned long code = 0;
                if (ent == "amp")
                        code = '&';
                else if (ent == "lt")
                        code = '<';
                else if (ent == "gt")
                        code = '>';
                else if (ent == "quot")
                        code = '"';
                else if (ent == "apos")
                        code = '\'';
                else if (ent == "nbsp")
                        code = ' ';
                else if (ent.size() > 1 && ent[0] == '#')
                        code = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, NULL, 16)
                                                                : strtoul(ent.c_str() + 1, NULL, 10);
                if (!code) {
                        out += s[i];
                        continue;
                }
                wxString ch = wxString(wxUniChar((wxUint32)code));
                out += (const char *)ch.utf8_str();
                i = end;
        }
        return out;
}

/* Text of an HTML fragment: tags removed, entities decoded, white space collapsed. */
static wxString html_text(const std::string &html) {
        std::string s;
        bool in_tag = false;
        for (char c : html) {
                if (c == '<')
                        in_tag = true;
                else if (c == '>' && in_tag) {
                        in_tag = false;
                        s += ' ';
                } else if (!in_tag)
                        s += c;
        }
        s = decode_entities(s);
        std::string out;
        bool space = false;
        for (char c : s) {
                if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
                        space = !out.empty();
                        continue;
                }
                if (space)
                        out += ' ';
                space = false;
                out += c;
        }
        wxString text = wxString::FromUTF8(out.c_str());
        text.Replace(" ,", ",");
        text.Replace("( ", "(");
        text.Replace(" )", ")");
        return text;
}

static bool find_first(const std::string &html, const std::string &pattern, std::string &match, int group = 1) {
        std::smatch m;
        if (!std::regex_search(html, m, std::regex(pattern)))
                return false;
        match = m[group].str();
        return true;
}

static std::vector<wxString> all_texts(const std::string &html, const std::string &pattern) {
        std::vector<wxString> items;
        std::regex re(pattern);
        for (std::sregex_iterator it(html.begin(), html.end(), re), end; it != end; ++it) {
                wxString t = html_text((*it)[1].str());
                if (!t.empty())
                        items.push_back(t);
        }
        return items;
}

/* Entries of one of the "N x item" tables (CPU sockets, expansion slots, I/O ports). */
static std::vector<wxString> list_table(const std::string &html, const std::string &heading) {
        std::string table;
        if (!find_first(html, ">" + heading + R"(\s*</div>\s*<div class="list-table">([\s\S]*?)</div>\s*</div>)", table))
                return {};
        std::vector<wxString> items;
        /* The table was cut at its own closing tags, so the last entry has no "</div>" left. */
        std::regex re(R"(<div class="list-table-head">([\s\S]*?)</div>\s*<div>([\s\S]*?)(?=</div>|$))");
        for (std::sregex_iterator it(table.begin(), table.end(), re), end; it != end; ++it) {
                wxString count = html_text((*it)[1].str());
                wxString name = html_text((*it)[2].str());
                if (name.empty() || name == "Empty")
                        continue;
                items.push_back(count.empty() || count == "?x" ? name : count + " " + name);
        }
        return items;
}

bool trw_parse_search(const wxString &html, std::vector<TrwSearchResult> &results) {
        std::string s = (const char *)html.utf8_str();
        results.clear();
        std::regex re(R"(href="/motherboards/s/([^"?]+)[^"]*">[\s\S]*?<div class="perk-name">([\s\S]*?)</div>)");
        for (std::sregex_iterator it(s.begin(), s.end(), re), end; it != end; ++it) {
                TrwSearchResult r;
                r.slug = wxString::FromUTF8((*it)[1].str().c_str());
                r.title = html_text((*it)[2].str());
                results.push_back(r);
        }
        return s.find("search-form-grid") != std::string::npos;
}

static void parse_bios(const std::string &html, TrwBoard &board) {
        size_t start = html.find("class=\"show-downloads\" id=\"sh-bios\"");
        if (start == std::string::npos)
                return;
        size_t stop = html.find("id=\"sh-driver\"", start);
        std::string section = html.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
        size_t generic_at = section.find("generic BIOS file");

        std::vector<std::pair<size_t, wxString>> vendors;
        std::regex vendor_re(R"(<p class="bios-vendor-header">([\s\S]*?)</p>)");
        for (std::sregex_iterator it(section.begin(), section.end(), vendor_re), end; it != end; ++it) {
                wxString v = html_text((*it)[1].str());
                int dash = v.rfind(" - ") == wxString::npos ? wxNOT_FOUND : (int)v.rfind(" - ");
                vendors.push_back({(size_t)it->position(), dash == wxNOT_FOUND ? v : v.Left(dash)});
        }

        const std::string marker = "<div class=\"list-entry centered";
        size_t pos = section.find(marker);
        while (pos != std::string::npos) {
                size_t next = section.find(marker, pos + marker.size());
                std::string entry = section.substr(pos, next == std::string::npos ? std::string::npos : next - pos);
                size_t entry_pos = pos;
                pos = next;
                if (entry.compare(marker.size(), 6, " head\"") == 0)
                        continue;

                TrwBios bios;
                std::string m;
                if (!find_first(entry, R"(href="(/[^"]+)\")", m))
                        continue; /* nothing to download */
                bios.href = wxString::FromUTF8(m.c_str());
                if (find_first(entry, R"(download="([^"]*)\")", m))
                        bios.filename = html_text(m);
                if (find_first(entry, R"(<div class="monospace">([\s\S]*?)</div>)", m))
                        bios.post_string = html_text(m);
                if (find_first(entry, R"(Core version</div>\s*<div>([\s\S]*?)</div>)", m))
                        bios.core_version = html_text(m);
                if (find_first(entry, R"(<a [^>]*download[^>]*>[\s\S]*?<span>([\s\S]*?)</span>)", m))
                        bios.version = html_text(m);
                for (auto &v : vendors)
                        if (v.first < entry_pos)
                                bios.vendor = v.second;
                bios.generic = generic_at != std::string::npos && entry_pos > generic_at;
                board.bios.push_back(bios);
        }
}

bool trw_parse_board(const wxString &html, const wxString &slug, TrwBoard &board) {
        std::string s = (const char *)html.utf8_str();
        std::string m;

        board = TrwBoard();
        board.slug = slug;
        if (!find_first(s, R"(<title>([\s\S]*?)</title>)", m))
                return false;
        board.title = html_text(m);
        if (board.title.EndsWith(" - The Retro Web"))
                board.title.RemoveLast(strlen(" - The Retro Web"));
        if (s.find("quick-spec-table") == std::string::npos)
                return false;

        if (find_first(s, R"(>Chipset\s*</div>\s*<div>([\s\S]*?)</div>)", m)) {
                board.chipset = html_text(m);
                if (board.chipset == "Empty" || board.chipset == "Unknown")
                        board.chipset.clear();
        }
        if (find_first(s, R"(FSB speeds?\s*</div>\s*<div class="tag-container">([\s\S]*?)</div>)", m))
                board.fsb = all_texts(m, R"(<span class="text-block">([\s\S]*?)</span>)");
        if (find_first(s, R"(RAM types?</div>\s*<ul[^>]*>([\s\S]*?)</ul>)", m))
                board.ram_types = all_texts(m, R"(<li>([\s\S]*?)</li>)");
        if (find_first(s, R"(Max RAM size\s*</div>\s*<div class="tag-container">([\s\S]*?)</div>)", m))
                board.max_ram = all_texts(m, R"(<span class="text-block">([\s\S]*?)</span>)");
        board.sockets = list_table(s, "CPU sockets?");
        board.slots = list_table(s, "Expansion slots");
        board.io_ports = list_table(s, "I/O ports");

        size_t chips_at = s.find("<div class=\"chip-list\">");
        if (chips_at != std::string::npos) {
                size_t chips_end = s.find("<!-- CPU family", chips_at);
                std::string chips = s.substr(chips_at, chips_end == std::string::npos ? 20000 : chips_end - chips_at);
                std::regex re(R"(<span>-(?:>|&gt;)\s*([^<]*)</span>\s*<div class="tag-container">([\s\S]*?)</div>)");
                for (std::sregex_iterator it(chips.begin(), chips.end(), re), end; it != end; ++it) {
                        TrwChipGroup group;
                        group.type = html_text((*it)[1].str());
                        group.chips = all_texts((*it)[2].str(), R"(<span class="text-block">([\s\S]*?)</span>)");
                        if (!group.chips.empty())
                                board.chips.push_back(group);
                }
        }

        parse_bios(s, board);
        return true;
}

/* ---------------------------------------------------------------- compatibility */

/* The MetaPC models that can stand in for real boards. boards are the normalized titles of the
   boards each model is (see normalize_title); chipsets are keys found in a normalized chipset
   name. The first model whose chipset matches, preferring one with the same Super I/O, provides
   the hardware for another board with that chipset. */
struct TrwBaseModel {
        const char *model;
        const char *boards[3];
        const char *chipsets[4];
        const char *superio;
        long bios_size;
        const char *note;
};

static const char *intel_note = "This MetaPC model emulates Intel's own board logic, so BIOSes made for other "
                                "boards may not start on it.";

static const TrwBaseModel base_models[] = {
        {"p55t2p4", {"asuspip55t2p4"}, {"430hx"}, "w83877f", 0x20000, NULL},
        {"p55tvp4", {"asuspip55tvp4"}, {"430vx"}, "w83877f", 0x20000, NULL},
        {"p55va", {"epoxp55va"}, {"430vx"}, "fdc37c932fr", 0x20000, NULL},
        {"430vx", {"shuttlehot557"}, {"430vx"}, "um8669f", 0x20000, NULL},
        {"endeavor", {"inteladvancedev"}, {"430fx"}, "pc87306", 0x20000, intel_note},
        {"zappa", {"inteladvancedzp"}, {"430fx"}, "pc87306", 0x20000, intel_note},
        {"revenge", {"intelpremierepci"}, {"430lx"}, "fdc37c665", 0x20000, intel_note},
        {"fic_va503p", {"ficva503"}, {"mvp3"}, "w83877tf", 0x20000, NULL},
        {"vs440fx", {"intelvs440fx"}, {"440fx"}, "pc87307", 0x40000, intel_note},
        {"ga686bx", {"gigabytega686bx"}, {"440bx"}, "w83977tf", 0x40000, NULL},
        {"sis496", {}, {"sis496", "85c496"}, NULL, 0x20000, NULL},
        {"ami486", {}, {"m1429", "ali1429"}, NULL, 0x10000, NULL},
        {"ami386dx", {}, {"82c495", "opti495"}, NULL, 0x10000, NULL},
        {"dtk386", {}, {"neatsx", "cs8281"}, NULL, 0x10000, NULL},
        {"ami286", {}, {"neat", "cs8221"}, NULL, 0x10000, NULL},
        {"kmxc02", {}, {"scatsx", "82c836"}, NULL, 0x10000, NULL},
        {"award286", {}, {"scat", "82c235"}, NULL, 0x10000, NULL},
        {"ami386", {}, {"ht18", "ht21"}, NULL, 0x10000, NULL},
        {"tg286m", {}, {"ht12"}, NULL, 0x10000, NULL},
};

static wxString normalize(const wxString &s, bool drop_parentheses) {
        wxString out;
        int depth = 0;
        for (wxUniChar c : s.Lower()) {
                if (c == '(')
                        depth++;
                else if (c == ')' && depth > 0)
                        depth--;
                else if ((!drop_parentheses || !depth) && ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
                        out += c;
        }
        return out;
}

/* A board title without its revision, e.g. "ASUS P/I-P55T2P4 (REV 2.x)" -> "asuspip55t2p4". */
static wxString normalize_title(const wxString &title) {
        wxString t = normalize(title, true);
        int rev = t.Find("rev");
        if (rev > 0)
                t = t.Left(rev);
        return t;
}

static int find_model(const char *internal_name) {
        for (int c = 0; models[c] != NULL; c++)
                if (!strcmp(models[c]->internal_name, internal_name))
                        return c;
        return -1;
}

static wxString join(const std::vector<wxString> &items, const wxString &sep = ", ") {
        wxString s;
        for (const wxString &i : items)
                s += (s.empty() ? "" : sep) + i;
        return s;
}

static std::vector<wxString> board_superio(const TrwBoard &board) {
        std::vector<wxString> chips;
        for (const TrwChipGroup &g : board.chips)
                if (g.type.Lower().Contains("super i/o"))
                        chips.insert(chips.end(), g.chips.begin(), g.chips.end());
        return chips;
}

static bool chips_contain(const std::vector<wxString> &chips, const char *part) {
        for (const wxString &c : chips)
                if (normalize(c, false).Contains(part))
                        return true;
        return false;
}

TrwCheck trw_check_board(const TrwBoard &board) {
        TrwCheck check;
        check.verdict = TRW_UNSUPPORTED;
        check.model = -1;
        check.bios_size = 0;
        check.own_rom = false;

        wxString title = normalize_title(board.title);
        wxString chipset = normalize(board.chipset, false);
        std::vector<wxString> superio = board_superio(board);
        const TrwBaseModel *base = NULL;

        for (const TrwBaseModel &b : base_models) {
                for (const char *key : b.boards)
                        if (key && title == key && find_model(b.model) >= 0) {
                                base = &b;
                                check.verdict = TRW_EXACT;
                        }
                if (base)
                        break;
        }
        if (!base && !chipset.empty()) {
                for (const TrwBaseModel &b : base_models) {
                        bool match = false;
                        for (const char *key : b.chipsets)
                                if (key && chipset.Contains(key))
                                        match = true;
                        if (!match || find_model(b.model) < 0)
                                continue;
                        if (!base || (b.superio && chips_contain(superio, b.superio) &&
                                      !(base->superio && chips_contain(superio, base->superio))))
                                base = &b;
                }
                if (base)
                        check.verdict = TRW_COMPATIBLE;
        }

        if (!base) {
                check.summary = board.chipset.empty()
                                        ? "The Retro Web does not list this board's chipset, so MetaPC cannot tell how to emulate it."
                                        : "MetaPC does not emulate the " + board.chipset + " chipset yet.";
                check.problems.push_back(check.summary);
                return check;
        }

        check.model = find_model(base->model);
        check.bios_size = base->bios_size;
        wxString model_name = models[check.model]->name;
        check.own_rom = romspresent[models[check.model]->id] != 0;

        if (check.verdict == TRW_EXACT) {
                check.summary = "MetaPC emulates this board as \"" + model_name + "\".";
                check.good.push_back(check.summary);
                if (check.own_rom)
                        check.good.push_back("Its ROM set is installed. You can use it or one of the board's BIOS files.");
                else
                        check.warnings.push_back("Its ROM set is not installed, so choose one of the board's BIOS files.");
        } else {
                check.summary = "The " + board.chipset + " chipset is emulated: MetaPC will run this board's BIOS on the "
                                "hardware of \"" + model_name + "\" (experimental).";
                check.good.push_back(check.summary);
                if (base->superio) {
                        if (chips_contain(superio, base->superio))
                                check.good.push_back("Its Super I/O (" + join(superio) + ") is emulated too.");
                        else
                                check.warnings.push_back(
                                        superio.empty()
                                                ? wxString("The Retro Web does not list its Super I/O chip. The BIOS may not "
                                                           "find the floppy, serial or parallel ports.")
                                                : "Its Super I/O (" + join(superio) +
                                                          ") differs from the emulated board's, so the BIOS may not find "
                                                          "the floppy, serial or parallel ports.");
                }
                if (base->note)
                        check.warnings.push_back(base->note);
        }

        static const char *onboard[] = {"audio", "sound", "video", "graphic", "vga", "scsi", "network", "ethernet", "lan", NULL};
        for (const TrwChipGroup &g : board.chips) {
                wxString type = g.type.Lower();
                for (int i = 0; onboard[i]; i++)
                        if (type.Contains(onboard[i])) {
                                check.warnings.push_back("Onboard " + g.type + " (" + join(g.chips) +
                                                         ") is not emulated; add an expansion card instead.");
                                break;
                        }
        }

        if (!check.own_rom || check.verdict != TRW_EXACT) {
                if (board.bios.empty()) {
                        check.problems.push_back("The Retro Web has no BIOS file for this board.");
                        check.verdict = TRW_UNSUPPORTED;
                } else
                        check.good.push_back(wxString::Format("MetaPC needs a %ld KB BIOS image; it is checked after "
                                                              "downloading.",
                                                              check.bios_size / 1024));
        }
        return check;
}

/* ---------------------------------------------------------------- local files */

static wxString trw_dir() {
        wxFileName dir = wxFileName::DirName(wxString(pcem_path));
        dir.AppendDir("trw");
        return dir.GetPath(wxPATH_GET_VOLUME | wxPATH_GET_SEPARATOR);
}

static wxString board_dir(const wxString &slug) {
        wxFileName dir = wxFileName::DirName(trw_dir());
        dir.AppendDir(slug);
        return dir.GetPath(wxPATH_GET_VOLUME | wxPATH_GET_SEPARATOR);
}

static bool safe_slug(const wxString &slug) {
        if (slug.empty() || slug.length() > 200 || slug.StartsWith("."))
                return false;
        for (wxUniChar c : slug)
                if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                      c == '_'))
                        return false;
        return true;
}

static wxString safe_filename(const wxString &name) {
        wxString base = wxFileName(name).GetFullName();
        wxString out;
        for (wxUniChar c : base)
                out += ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                        c == '_')
                               ? c
                               : wxUniChar('_');
        if (out.empty() || out.StartsWith("."))
                out = "bios" + out;
        return out;
}

static const char *cache_header = "# MetaPC cache of a motherboard page on The Retro Web (" TRW_SITE_URL ").";

/* The parsed board is cached as tab-separated lines, with the source and licence on top. */
static void save_board_cache(const TrwBoard &b) {
        if (!safe_slug(b.slug) || !wxFileName::Mkdir(board_dir(b.slug), wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL))
                return;
        wxTextFile f(board_dir(b.slug) + "board.txt");
        if (f.Exists())
                f.Open();
        else
                f.Create();
        f.Clear();
        f.AddLine(cache_header);
        f.AddLine("# Data from The Retro Web, " + b.Url() + ", licensed " TRW_LICENSE_NAME " (" TRW_LICENSE_URL ").");
        f.AddLine("# Adapted by MetaPC into a machine configuration.");
        auto line = [&f](const wxString &key, const std::vector<wxString> &values) {
                wxString s = key;
                for (const wxString &v : values)
                        s += "\t" + v;
                f.AddLine(s);
        };
        line("title", {b.title});
        line("chipset", {b.chipset});
        for (const TrwChipGroup &g : b.chips) {
                std::vector<wxString> v = {g.type};
                v.insert(v.end(), g.chips.begin(), g.chips.end());
                line("chips", v);
        }
        line("sockets", b.sockets);
        line("fsb", b.fsb);
        line("ram_types", b.ram_types);
        line("max_ram", b.max_ram);
        line("slots", b.slots);
        line("io_ports", b.io_ports);
        for (const TrwBios &bios : b.bios)
                line("bios", {bios.vendor, bios.post_string, bios.core_version, bios.version, bios.href, bios.filename,
                              bios.generic ? "1" : "0"});
        f.Write(wxTextFileType_None, wxConvUTF8);
}

static bool load_board_cache(const wxString &slug, TrwBoard &b) {
        if (!safe_slug(slug))
                return false;
        wxFileName fn(board_dir(slug) + "board.txt");
        if (!fn.FileExists() || (wxDateTime::Now() - fn.GetModificationTime()).GetDays() > TRW_CACHE_DAYS)
                return false;
        wxTextFile f(fn.GetFullPath());
        if (!f.Open(wxConvUTF8) || f.GetLineCount() == 0 || f.GetLine(0) != cache_header)
                return false;
        b = TrwBoard();
        b.slug = slug;
        for (size_t i = 0; i < f.GetLineCount(); i++) {
                wxString l = f.GetLine(i);
                if (l.StartsWith("#"))
                        continue;
                wxArrayString p = wxSplit(l, '\t', 0);
                if (p.empty())
                        continue;
                std::vector<wxString> v(p.begin() + 1, p.end());
                wxString key = p[0];
                if (key == "title" && v.size() > 0)
                        b.title = v[0];
                else if (key == "chipset" && v.size() > 0)
                        b.chipset = v[0];
                else if (key == "chips" && v.size() > 1)
                        b.chips.push_back({v[0], std::vector<wxString>(v.begin() + 1, v.end())});
                else if (key == "sockets")
                        b.sockets = v;
                else if (key == "fsb")
                        b.fsb = v;
                else if (key == "ram_types")
                        b.ram_types = v;
                else if (key == "max_ram")
                        b.max_ram = v;
                else if (key == "slots")
                        b.slots = v;
                else if (key == "io_ports")
                        b.io_ports = v;
                else if (key == "bios" && v.size() >= 7)
                        b.bios.push_back({v[0], v[1], v[2], v[3], v[4], v[5], v[6] == "1"});
        }
        return !b.title.empty();
}

struct RomFile {
        wxString name;
        std::vector<unsigned char> data;
};

/* Which half of an even/odd BIOS pair a file name says it is: 0 even (low), 1 odd (high), -1 unknown. */
static int half_label(const wxString &name, const wxString &other) {
        wxString a = wxFileName(name).GetName().Lower(), b = wxFileName(other).GetName().Lower();
        if (a.Contains("even") || a.Contains("low"))
                return 0;
        if (a.Contains("odd") || a.Contains("high"))
                return 1;
        if (a.EndsWith("lo") || a.EndsWith("-l") || a.EndsWith("_l"))
                return 0;
        if (a.EndsWith("hi") || a.EndsWith("-h") || a.EndsWith("_h"))
                return 1;
        /* Names that differ only in their last letter, such as BIOSL/BIOSH or 386E/386O. */
        if (a.length() == b.length() && a.length() > 1 && a.Left(a.length() - 1) == b.Left(b.length() - 1)) {
                wxUniChar c = a.Last();
                if (c == 'l' || c == 'e')
                        return 0;
                if (c == 'h' || c == 'o')
                        return 1;
        }
        return -1;
}

/* An x86 BIOS starts at its reset vector, 16 bytes from the end, with a jump. */
static bool valid_reset_vector(const std::vector<unsigned char> &image) {
        if (image.size() < 16)
                return false;
        unsigned char op = image[image.size() - 16];
        return op == 0xea || op == 0xe9 || op == 0xeb;
}

static size_t common_prefix(const wxString &a, const wxString &b) {
        size_t n = 0;
        while (n < a.length() && n < b.length() && wxTolower(a[n]) == wxTolower(b[n]))
                n++;
        return n;
}

/* Finds the pair of half-size files that interleave into a valid BIOS image. Pairs whose names
   say which half they are, and whose names are most alike, are preferred. */
static bool interleave_halves(const std::vector<RomFile> &halves, long size, std::vector<unsigned char> &image,
                              wxString &name) {
        int best_score = -1;
        for (size_t i = 0; i < halves.size(); i++)
                for (size_t j = 0; j < halves.size(); j++) {
                        if (i == j)
                                continue;
                        const RomFile &even = halves[i], &odd = halves[j];
                        int even_label = half_label(even.name, odd.name), odd_label = half_label(odd.name, even.name);
                        if (even_label == 1 || odd_label == 0)
                                continue; /* the names say the other way round */
                        std::vector<unsigned char> out(size);
                        for (long k = 0; k < size / 2; k++) {
                                out[2 * k] = even.data[k];
                                out[2 * k + 1] = odd.data[k];
                        }
                        if (!valid_reset_vector(out))
                                continue;
                        int score = (even_label == 0 ? 100 : 0) + (odd_label == 1 ? 100 : 0) +
                                    (int)common_prefix(even.name, odd.name);
                        if (score > best_score) {
                                best_score = score;
                                image = out;
                                name = wxFileName(even.name).GetName() + "+" + wxFileName(odd.name).GetName() + ".bin";
                        }
                }
        return best_score >= 0;
}

/* Picks the BIOS image out of a download (a zip, or the image itself) and saves it in the
   board's folder. Returns the saved path, or an empty string and an error. */
static wxString store_bios(const wxMemoryBuffer &data, const TrwBoard &board, const TrwBios &bios, long wanted_size,
                           wxString &error) {
        const unsigned char *bytes = (const unsigned char *)data.GetData();
        size_t len = data.GetDataLen();
        wxString dir = board_dir(board.slug);
        if (!safe_slug(board.slug) || !wxFileName::Mkdir(dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL)) {
                error = "Could not create the folder " + dir;
                return "";
        }

        auto save = [&](const void *p, size_t n, const wxString &name) -> wxString {
                wxString path = dir + safe_filename(name);
                wxFile out;
                if (!out.Create(path, true) || out.Write(p, n) != n) {
                        error = "Could not write " + path;
                        return "";
                }
                return path;
        };

        if (len >= 4 && bytes[0] == 'P' && bytes[1] == 'K') {
                wxMemoryInputStream mem(bytes, len);
                wxZipInputStream zip(mem);
                std::unique_ptr<wxZipEntry> entry;
                std::vector<wxString> sizes;
                std::vector<RomFile> halves;
                while (entry.reset(zip.GetNextEntry()), entry) {
                        if (entry->IsDir())
                                continue;
                        wxFileOffset size = entry->GetSize();
                        sizes.push_back(wxString::Format("%s (%lld KB)", entry->GetName(), (long long)size / 1024));
                        if (size != wanted_size && size != wanted_size / 2)
                                continue;
                        RomFile f;
                        f.name = wxFileName(entry->GetName()).GetFullName();
                        f.data.resize(size);
                        zip.Read(f.data.data(), size);
                        if ((wxFileOffset)zip.LastRead() != size) {
                                error = "Could not read " + entry->GetName() + " from the downloaded file.";
                                return "";
                        }
                        if (size == wanted_size)
                                return save(f.data.data(), size, f.name);
                        halves.push_back(f);
                }

                /* BIOSes for 16 and 32-bit boards often come as two chips, one holding the even
                   bytes and one the odd bytes; they are interleaved back into one image. */
                if (halves.size() >= 2) {
                        std::vector<unsigned char> image;
                        wxString name;
                        if (interleave_halves(halves, wanted_size, image, name))
                                return save(image.data(), image.size(), name);
                        error = wxString::Format("The download has two %ld KB halves of a BIOS, but MetaPC could not tell "
                                                 "which holds the even bytes: no order gives a valid start-up jump. "
                                                 "It contains: %s.",
                                                 wanted_size / 2048, join(sizes));
                        return "";
                }
                error = wxString::Format("The download has no %ld KB BIOS image (or two %ld KB halves of one), which the "
                                         "emulated hardware needs. It contains: %s.",
                                         wanted_size / 1024, wanted_size / 2048,
                                         sizes.empty() ? wxString("nothing") : join(sizes));
                return "";
        }
        if ((long)len != wanted_size) {
                error = wxString::Format("The BIOS file is %ld KB, but the emulated hardware needs %ld KB.", (long)len / 1024,
                                         wanted_size / 1024);
                return "";
        }
        return save(bytes, len, bios.filename.empty() ? wxString("bios.bin") : bios.filename);
}

/* ---------------------------------------------------------------- attribution */

/* "256MB", "1GB", "512KB (with ...)" -> KB, or 0. */
static long parse_size_kb(const wxString &s) {
        double value;
        wxString t = s.Strip(wxString::both);
        size_t i = 0;
        while (i < t.length() && (wxIsdigit(t[i]) || t[i] == '.'))
                i++;
        if (!i || !t.Left(i).ToCDouble(&value))
                return 0;
        wxString unit = t.Mid(i).Strip(wxString::leading).Upper();
        if (unit.StartsWith("GB"))
                return (long)(value * 1024 * 1024);
        if (unit.StartsWith("MB"))
                return (long)(value * 1024);
        if (unit.StartsWith("KB"))
                return (long)value;
        return 0;
}

/* "66MHz", "33.3MHz" -> MHz, or 0. */
static double parse_mhz(const wxString &s) {
        double value;
        wxString t = s.Strip(wxString::both);
        size_t i = 0;
        while (i < t.length() && (wxIsdigit(t[i]) || t[i] == '.'))
                i++;
        if (!i || !t.Left(i).ToCDouble(&value) || !t.Mid(i).Strip(wxString::leading).Upper().StartsWith("MHZ"))
                return 0;
        return value;
}

static void cfg_set(const char *name, const wxString &value) {
        config_set_string(CFG_MACHINE, (char *)TRW_CONFIG_SECTION, (char *)name, (char *)(const char *)value.utf8_str());
}

static wxString cfg_get(const char *name) {
        char *v = config_get_string(CFG_MACHINE, (char *)TRW_CONFIG_SECTION, (char *)name, NULL);
        return v ? wxString::FromUTF8(v) : wxString();
}

void trw_save_attribution(const TrwBoard &board, const char *model_internal_name) {
        trw_clear_attribution();
        cfg_set("board", board.title);
        cfg_set("url", board.Url());
        cfg_set("license", "Board data from The Retro Web (" TRW_SITE_URL "), " TRW_LICENSE_NAME " (" TRW_LICENSE_URL
                           "), adapted by MetaPC into this machine configuration");
        cfg_set("model", model_internal_name);

        wxString fsb;
        for (const wxString &f : board.fsb) {
                double mhz = parse_mhz(f);
                if (mhz > 0)
                        fsb += (fsb.empty() ? "" : ",") + wxString::FromCDouble(mhz);
        }
        cfg_set("fsb", fsb);

        long max_kb = 0;
        for (const wxString &m : board.max_ram)
                max_kb = std::max(max_kb, parse_size_kb(m));
        cfg_set("max_ram_kb", max_kb ? wxString::Format("%ld", max_kb) : wxString());

        /* Slot types as MetaPC's cards know them; empty when The Retro Web lists none. */
        wxString slots;
        auto add = [&slots](const char *type) {
                if (wxSplit(slots, ',', 0).Index(type) == wxNOT_FOUND)
                        slots += (slots.empty() ? "" : ",") + wxString(type);
        };
        for (const wxString &slot : board.slots) {
                wxString t = slot.Upper();
                if (t.Contains("AGP"))
                        add("agp");
                if (t.Contains("PCI") && !t.Contains("PCI-E") && !t.Contains("PCIE") && !t.Contains("PCI EXPRESS"))
                        add("pci");
                if (t.Contains("VLB") || t.Contains("VESA"))
                        add("vlb");
                if (t.Contains("EISA") || t.Contains("16-BIT ISA"))
                        add("isa16");
                else if (t.Contains("8-BIT ISA"))
                        add("isa8");
        }
        cfg_set("slots", slots);
}

TrwLimits trw_load_limits() {
        TrwLimits l;
        l.active = false;
        l.model = -1;
        l.max_ram_kb = 0;
        l.slots_known = l.isa8 = l.isa16 = l.vlb = l.pci = l.agp = false;

        l.board = cfg_get("board");
        wxString model_name = cfg_get("model");
        if (l.board.empty() || model_name.empty())
                return l;
        l.model = find_model(model_name.utf8_str());
        l.active = l.model >= 0;

        for (const wxString &f : wxSplit(cfg_get("fsb"), ',', 0)) {
                double mhz;
                if (f.ToCDouble(&mhz) && mhz > 0)
                        l.fsb.push_back(mhz);
        }
        if (!cfg_get("max_ram_kb").ToLong(&l.max_ram_kb))
                l.max_ram_kb = 0;
        wxString slots = cfg_get("slots");
        l.slots_known = !slots.empty();
        for (const wxString &s : wxSplit(slots, ',', 0)) {
                l.isa8 |= s == "isa8";
                l.isa16 |= s == "isa16";
                l.vlb |= s == "vlb";
                l.pci |= s == "pci";
                l.agp |= s == "agp";
        }
        return l;
}

bool trw_cpu_fits(const TrwLimits &limits, const CPU *cpu) {
        if (!limits.active || limits.fsb.empty())
                return true;
        /* The bus clock is the core clock over the multiplier. MetaPC rounds some multipliers
           (1.5 is stored as 2), so on PCI boards it is also checked as 2 or 3 times the PCI clock. */
        std::vector<double> candidates;
        if (cpu->multi > 0)
                candidates.push_back(cpu->rspeed / 1e6 / cpu->multi);
        if (cpu->pci_speed > 0) {
                candidates.push_back(cpu->pci_speed * 2 / 1e6);
                candidates.push_back(cpu->pci_speed * 3 / 1e6);
        }
        if (candidates.empty())
                candidates.push_back(cpu->rspeed / 1e6);
        for (double bus : candidates)
                for (double f : limits.fsb)
                        if (bus > f * 0.95 && bus < f * 1.05)
                                return true;
        return false;
}

/* The buses each card was really made for, by its device name. */
enum { BUS_ISA8 = 1, BUS_ISA16 = 2, BUS_VLB = 4, BUS_PCI = 8 };

static const struct {
        const char *device;
        int buses;
} card_buses[] = {
        /* Video */
        {"ATI Mach64GX", BUS_ISA16 | BUS_VLB | BUS_PCI},
        {"ATI Mach64VT2", BUS_PCI},
        {"Cirrus Logic GD5428", BUS_ISA16 | BUS_VLB},
        {"Cirrus Logic GD5429", BUS_ISA16 | BUS_VLB},
        {"Cirrus Logic GD5430", BUS_VLB | BUS_PCI},
        {"Cirrus Logic GD5434", BUS_VLB | BUS_PCI},
        {"Diamond Stealth 3D 2000 (S3 ViRGE)", BUS_PCI},
        {"S3 ViRGE/DX", BUS_PCI},
        {"Matrox Millennium", BUS_PCI},
        {"Matrox Mystique", BUS_PCI},
        {"Number 9 9FX (S3 Trio64)", BUS_VLB | BUS_PCI},
        {"Paradise Bahamas 64 (S3 Vision864)", BUS_VLB | BUS_PCI},
        {"Phoenix S3 Trio32", BUS_VLB | BUS_PCI},
        {"Phoenix S3 Trio64", BUS_VLB | BUS_PCI},
        {"Trident TGUI 9400CXi", BUS_ISA16 | BUS_VLB},
        {"Trident TGUI 9440", BUS_VLB | BUS_PCI},
        {"Tseng Labs ET4000/w32p", BUS_VLB | BUS_PCI},
        {"Tseng Labs ET4000AX", BUS_ISA16},
        {"Trident TVGA 8900D", BUS_ISA16},
        {"Trident TVGA 9000B", BUS_ISA16},
        {"ATI-28800", BUS_ISA16},
        {"Oak OTI-067", BUS_ISA16},
        {"VGA", BUS_ISA16},
        {"MDA", BUS_ISA8},
        {"CGA", BUS_ISA8},
        {"EGA", BUS_ISA8},
        {"Hercules", BUS_ISA8},
        {"Hercules InColor", BUS_ISA8},
        {"Colorplus", BUS_ISA8},
        {"Sigma Color 400", BUS_ISA8},
        {"Genius VHR", BUS_ISA8},
        {"Wyse 700", BUS_ISA8},
        {"Image Manager 1024", BUS_ISA8},
        {"PGC", BUS_ISA8},
        {"ATI EGA Wonder 800+", BUS_ISA8},
        {"ATI-18800", BUS_ISA16}, /* VGA Edge-16 */
        {"Oak OTI-037", BUS_ISA8},
        /* Sound */
        {"Ensoniq AudioPCI (ES1371)", BUS_PCI},
        {"Sound Blaster 16", BUS_ISA16},
        {"Sound Blaster AWE32", BUS_ISA16},
        {"Windows Sound System", BUS_ISA16},
        {"AdLib Gold", BUS_ISA16},
        {"Aztech Sound Galaxy Pro 16 AB (Washington)", BUS_ISA16},
        {"Aztech Sound Galaxy Nova 16 Extra (Clinton)", BUS_ISA16},
        {"Pro Audio Spectrum 16", BUS_ISA16},
        {"AdLib", BUS_ISA8},
        {"Sound Blaster v1.0", BUS_ISA8},
        {"Sound Blaster v1.5", BUS_ISA8},
        {"Sound Blaster v2.0", BUS_ISA8},
        {"Sound Blaster Pro v1", BUS_ISA8},
        {"Sound Blaster Pro v2", BUS_ISA8},
        {"Creative Music System / Game Blaster", BUS_ISA8},
        {"Innovation SSI-2001", BUS_ISA8},
        {"Mindscape Music Board", BUS_ISA8},
        /* Network */
        {"Novell NE2000", BUS_ISA16},
        {"Realtek RTL8029AS", BUS_PCI},
};

/* Whether a card made for these buses fits one of the board's slots. An ISA card fits any ISA
   slot at least as wide, and a VLB slot (an ISA slot with an extra connector). */
static bool buses_fit(const TrwLimits &l, int buses) {
        return ((buses & BUS_PCI) && l.pci) || ((buses & BUS_VLB) && l.vlb) ||
               ((buses & BUS_ISA16) && (l.isa16 || l.vlb)) || ((buses & BUS_ISA8) && (l.isa8 || l.isa16 || l.vlb));
}

bool trw_device_fits(const TrwLimits &limits, const device_t *device) {
        if (!limits.active || !limits.slots_known || !device)
                return true;
        for (const auto &c : card_buses)
                if (!strcmp(c.device, device->name))
                        return buses_fit(limits, c.buses);
        return trw_bus_fits(limits, device->flags);
}

bool trw_bus_fits(const TrwLimits &limits, int device_flags) {
        if (!limits.active || !limits.slots_known)
                return true;
        if (device_flags & DEVICE_MCA)
                return true;
        if (device_flags & DEVICE_PCI)
                return buses_fit(limits, BUS_PCI);
        return buses_fit(limits, (device_flags & DEVICE_AT) ? BUS_ISA16 : BUS_ISA8);
}

void trw_clear_attribution() { config_free_section(CFG_MACHINE, (char *)TRW_CONFIG_SECTION); }

/* ---------------------------------------------------------------- search dialog */

#if wxUSE_WEBREQUEST

static wxLongLong last_request_ms = 0;

class MotherboardSearchDialog : public wxDialog {
    public:
        MotherboardSearchDialog(wxWindow *parent, TrwSelection &selection);
        ~MotherboardSearchDialog();

    private:
        enum Kind { KIND_SEARCH, KIND_BOARD, KIND_BIOS };

        TrwSelection &selection;
        wxTextCtrl *search;
        wxButton *search_button;
        wxListBox *results_list;
        wxButton *prev_button, *next_button;
        wxStaticText *page_label;
        wxScrolledWindow *details;
        wxStaticText *status;
        wxButton *page_button, *use_button;
        wxChoice *bios_choice;

        wxTimer wait_timer;
        wxWebRequest request;
        Kind pending_kind;
        wxString pending_url;

        wxString query;
        int page;
        std::vector<TrwSearchResult> results;
        bool have_board;
        TrwBoard board;
        TrwCheck check;
        std::vector<int> bios_index; /* choice item -> board.bios index, -1 for the model's own ROM set */
        int pending_bios;            /* board.bios index being downloaded */

        void Fetch(Kind kind, const wxString &url);
        void StartPending();
        void OnWaitTimer(wxTimerEvent &);
        void OnWebRequest(wxWebRequestEvent &evt);
        void SetBusy(const wxString &message);
        void SetIdle(const wxString &message);

        void StartSearch(int new_page);
        void SearchDone(const wxString &html);
        void SelectResult();
        void BoardDone(const wxString &html);
        void ShowBoard();
        void UseBoard();
        void BiosDone(const wxMemoryBuffer &data);
};

MotherboardSearchDialog::MotherboardSearchDialog(wxWindow *parent, TrwSelection &selection)
        : wxDialog(parent, wxID_ANY, "New machine by motherboard", wxDefaultPosition, wxDefaultSize,
                   wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER),
          selection(selection), bios_choice(NULL), wait_timer(this), page(1), have_board(false), pending_bios(-1) {
        wxBoxSizer *main = new wxBoxSizer(wxVERTICAL);

        wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(new wxStaticText(this, wxID_ANY, "Model or part number:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
        search = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER);
        search->SetHint("e.g. P55T2P4, GA-586HX, 486VL");
        row->Add(search, 1, wxALIGN_CENTER_VERTICAL);
        search_button = new wxButton(this, wxID_ANY, "Search");
        row->Add(search_button, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
        main->Add(row, 0, wxEXPAND | wxALL, FromDIP(10));

        wxStaticText *hint = new wxStaticText(
                this, wxID_ANY,
                "Searches the motherboards on The Retro Web that have BIOS files. Type the model without the manufacturer. "
                "Pick a board to see whether MetaPC can emulate it.");
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(FromDIP(900));
        main->Add(hint, 0, wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

        wxBoxSizer *body = new wxBoxSizer(wxHORIZONTAL);
        wxBoxSizer *left = new wxBoxSizer(wxVERTICAL);
        results_list = new wxListBox(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(320, 380)));
        left->Add(results_list, 1, wxEXPAND);
        wxBoxSizer *pager = new wxBoxSizer(wxHORIZONTAL);
        prev_button = new wxButton(this, wxID_ANY, "< Previous");
        next_button = new wxButton(this, wxID_ANY, "Next >");
        page_label = new wxStaticText(this, wxID_ANY, "");
        pager->Add(prev_button, 0);
        pager->Add(page_label, 1, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(6));
        pager->Add(next_button, 0);
        left->Add(pager, 0, wxEXPAND | wxTOP, FromDIP(6));
        body->Add(left, 0, wxEXPAND | wxRIGHT, FromDIP(10));

        details = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, FromDIP(wxSize(560, 380)), wxVSCROLL | wxBORDER_THEME);
        details->SetScrollRate(0, FromDIP(10));
        body->Add(details, 1, wxEXPAND);
        main->Add(body, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

        status = new wxStaticText(this, wxID_ANY, "", wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
        main->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        main->Add(new wxStaticLine(this), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        wxBoxSizer *credit = new wxBoxSizer(wxHORIZONTAL);
        credit->Add(new wxStaticText(this, wxID_ANY, "Motherboard data from"), 0, wxALIGN_CENTER_VERTICAL);
        credit->Add(new wxHyperlinkCtrl(this, wxID_ANY, "The Retro Web", TRW_SITE_URL), 0,
                    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
        credit->Add(new wxStaticText(this, wxID_ANY, "under"), 0, wxALIGN_CENTER_VERTICAL);
        credit->Add(new wxHyperlinkCtrl(this, wxID_ANY, TRW_LICENSE_NAME, TRW_LICENSE_URL), 0,
                    wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
        credit->Add(new wxStaticText(this, wxID_ANY, "- adapted by MetaPC, which does not include or redistribute it."), 0,
                    wxALIGN_CENTER_VERTICAL);
        main->Add(credit, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
        page_button = new wxButton(this, wxID_ANY, "Open board page");
        buttons->Add(page_button, 0);
        buttons->AddStretchSpacer();
        use_button = new wxButton(this, wxID_OK, "Use this board");
        buttons->Add(use_button, 0, wxRIGHT, FromDIP(6));
        buttons->Add(new wxButton(this, wxID_CANCEL, "Cancel"), 0);
        main->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));

        SetSizerAndFit(main);
        CentreOnParent();

        search_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StartSearch(1); });
        search->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { StartSearch(1); });
        prev_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StartSearch(page - 1); });
        next_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { StartSearch(page + 1); });
        results_list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) { SelectResult(); });
        page_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
                if (have_board)
                        wxLaunchDefaultBrowser(board.Url());
        });
        use_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { UseBoard(); });
        Bind(wxEVT_TIMER, &MotherboardSearchDialog::OnWaitTimer, this, wait_timer.GetId());
        Bind(wxEVT_WEBREQUEST_STATE, &MotherboardSearchDialog::OnWebRequest, this);

        ShowBoard();
        SetIdle("");
        search->SetFocus();
}

MotherboardSearchDialog::~MotherboardSearchDialog() {
        wait_timer.Stop();
        if (request.IsOk() && request.GetState() == wxWebRequest::State_Active)
                request.Cancel();
}

void MotherboardSearchDialog::SetBusy(const wxString &message) {
        status->SetLabel(message);
        search_button->Disable();
        prev_button->Disable();
        next_button->Disable();
        use_button->Disable();
}

void MotherboardSearchDialog::SetIdle(const wxString &message) {
        status->SetLabel(message);
        search_button->Enable();
        prev_button->Enable(!query.empty() && page > 1);
        next_button->Enable(!query.empty() && (int)results.size() >= TRW_RESULTS_PER_PAGE);
        page_label->SetLabel(query.empty() ? wxString() : wxString::Format("Page %d", page));
        use_button->Enable(have_board && check.Usable());
        page_button->Enable(have_board);
}

/* Starts a request once The Retro Web's crawl delay has passed since the previous one. */
void MotherboardSearchDialog::Fetch(Kind kind, const wxString &url) {
        wait_timer.Stop();
        if (request.IsOk() && request.GetState() == wxWebRequest::State_Active)
                request.Cancel();
        pending_kind = kind;
        pending_url = url;
        wxLongLong wait = last_request_ms + TRW_REQUEST_INTERVAL_MS - wxGetLocalTimeMillis();
        if (wait > 0) {
                SetBusy(wxString::Format("Waiting %d s before asking The Retro Web again, as the site asks...",
                                         (int)((wait.GetValue() + 999) / 1000)));
                wait_timer.StartOnce((int)wait.GetValue());
        } else
                StartPending();
}

void MotherboardSearchDialog::OnWaitTimer(wxTimerEvent &) { StartPending(); }

void MotherboardSearchDialog::StartPending() {
        last_request_ms = wxGetLocalTimeMillis();
        request = wxWebSession::GetDefault().CreateRequest(this, pending_url);
        if (!request.IsOk()) {
                SetIdle("Could not connect to The Retro Web.");
                return;
        }
        request.SetHeader("User-Agent", "MetaPC/" PCEM_VERSION_STRING " (+https://github.com/PSF1/metapc)");
        if (pending_kind == KIND_BIOS)
                request.SetHeader("Accept", "*/*");
        SetBusy(pending_kind == KIND_SEARCH ? "Searching The Retro Web..."
                : pending_kind == KIND_BOARD ? "Reading the board's page on The Retro Web..."
                                             : "Downloading the BIOS from The Retro Web...");
        request.Start();
}

void MotherboardSearchDialog::OnWebRequest(wxWebRequestEvent &evt) {
        switch (evt.GetState()) {
        case wxWebRequest::State_Completed: {
                wxWebResponse response = evt.GetResponse();
                if (!response.IsOk() || response.GetStatus() != 200) {
                        SetIdle(wxString::Format("The Retro Web answered with an error (HTTP %d).",
                                                 response.IsOk() ? response.GetStatus() : 0));
                        return;
                }
                if (pending_kind == KIND_BIOS) {
                        wxMemoryBuffer data;
                        wxInputStream *in = response.GetStream();
                        char buf[16384];
                        while (in && in->CanRead()) {
                                in->Read(buf, sizeof(buf));
                                size_t n = in->LastRead();
                                if (!n)
                                        break;
                                data.AppendData(buf, n);
                        }
                        BiosDone(data);
                } else if (pending_kind == KIND_SEARCH)
                        SearchDone(response.AsString());
                else
                        BoardDone(response.AsString());
                break;
        }
        case wxWebRequest::State_Failed:
                SetIdle("Could not reach The Retro Web: " + evt.GetErrorDescription());
                break;
        case wxWebRequest::State_Unauthorized:
                SetIdle("The Retro Web refused the request.");
                break;
        case wxWebRequest::State_Cancelled:
        default:
                break;
        }
}

void MotherboardSearchDialog::StartSearch(int new_page) {
        wxString q = search->GetValue().Strip(wxString::both);
        if (q.empty()) {
                SetIdle("Type a model or part number to search.");
                return;
        }
        if (new_page < 1)
                new_page = 1;
        query = q;
        page = new_page;
        wxString encoded;
        wxScopedCharBuffer utf8 = q.utf8_str();
        for (const char *p = utf8.data(); *p; p++) {
                unsigned char c = (unsigned char)*p;
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                    c == '_' || c == '~')
                        encoded += (char)c;
                else
                        encoded += wxString::Format("%%%02X", c);
        }
        wxString url = wxString::Format(TRW_SITE_URL "/motherboards?name=%s&hasBios=1&itemsPerPage=%d&page=%d", encoded,
                               TRW_RESULTS_PER_PAGE, page);
        Fetch(KIND_SEARCH, url);
}

void MotherboardSearchDialog::SearchDone(const wxString &html) {
        std::vector<TrwSearchResult> found;
        if (!trw_parse_search(html, found)) {
                results.clear();
                results_list->Clear();
                SetIdle("The Retro Web's search page could not be read; the site may have changed.");
                return;
        }
        results = found;
        results_list->Clear();
        for (const TrwSearchResult &r : results)
                results_list->Append(r.title);
        SetIdle(results.empty() ? wxString("No boards with BIOS files match. Type the model without the manufacturer.")
                                : wxString::Format("%d boards on this page. Pick one to check it.", (int)results.size()));
}

void MotherboardSearchDialog::SelectResult() {
        int sel = results_list->GetSelection();
        if (sel < 0 || sel >= (int)results.size())
                return;
        const TrwSearchResult &r = results[sel];
        if (!safe_slug(r.slug)) {
                SetIdle("This board's address on The Retro Web is not one MetaPC can use.");
                return;
        }
        TrwBoard cached;
        if (load_board_cache(r.slug, cached)) {
                board = cached;
                have_board = true;
                ShowBoard();
                SetIdle("Board read from the local copy of its page on The Retro Web.");
                return;
        }
        have_board = false;
        ShowBoard();
        board.slug = r.slug;
        Fetch(KIND_BOARD, TRW_SITE_URL "/motherboards/s/" + r.slug);
}

void MotherboardSearchDialog::BoardDone(const wxString &html) {
        TrwBoard parsed;
        wxString slug = pending_url.AfterLast('/');
        if (!trw_parse_board(html, slug, parsed)) {
                SetIdle("The board's page could not be read; the site may have changed.");
                return;
        }
        board = parsed;
        have_board = true;
        save_board_cache(board);
        ShowBoard();
        SetIdle("");
}

static void add_field(wxWindow *parent, wxFlexGridSizer *grid, const wxString &label, const wxString &value) {
        if (value.empty())
                return;
        wxStaticText *l = new wxStaticText(parent, wxID_ANY, label);
        l->SetFont(l->GetFont().Bold());
        grid->Add(l, 0, wxALIGN_TOP);
        wxStaticText *v = new wxStaticText(parent, wxID_ANY, value);
        v->Wrap(parent->FromDIP(400));
        grid->Add(v, 1, wxEXPAND);
}

void MotherboardSearchDialog::ShowBoard() {
        details->Freeze();
        details->DestroyChildren();
        bios_choice = NULL;
        bios_index.clear();
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        int pad = FromDIP(10);

        if (!have_board) {
                wxStaticText *t = new wxStaticText(details, wxID_ANY, "Search for a board and pick it from the list.");
                t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
                sizer->Add(t, 0, wxALL, pad);
                details->SetSizer(sizer, true);
                details->Thaw();
                use_button->Disable();
                return;
        }

        check = trw_check_board(board);

        wxStaticText *title = new wxStaticText(details, wxID_ANY, board.title);
        title->SetFont(title->GetFont().Bold().Scaled(1.3f));
        sizer->Add(title, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        sizer->Add(new wxHyperlinkCtrl(details, wxID_ANY, board.Url(), board.Url()), 0, wxLEFT | wxRIGHT, pad);

        /* The verdict comes first, so the answer is visible without scrolling. */
        wxColour colour = check.verdict == TRW_EXACT        ? wxColour(0, 120, 0)
                          : check.verdict == TRW_COMPATIBLE ? wxColour(170, 100, 0)
                                                            : wxColour(180, 0, 0);
        wxString heading = check.verdict == TRW_EXACT        ? "Can be emulated"
                           : check.verdict == TRW_COMPATIBLE ? "Can be emulated (experimental)"
                                                             : "Cannot be emulated";
        wxStaticText *verdict = new wxStaticText(details, wxID_ANY, heading);
        verdict->SetFont(verdict->GetFont().Bold().Scaled(1.15f));
        verdict->SetForegroundColour(colour);
        sizer->Add(verdict, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        auto bullets = [&](const std::vector<wxString> &items, const wxString &mark, const wxColour &c) {
                for (const wxString &i : items) {
                        wxStaticText *t = new wxStaticText(details, wxID_ANY, mark + " " + i);
                        if (c.IsOk())
                                t->SetForegroundColour(c);
                        t->Wrap(FromDIP(520));
                        sizer->Add(t, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(4) + pad / 2);
                }
        };
        bullets(check.good, wxString::FromUTF8("\xE2\x9C\x94"), wxNullColour);
        bullets(check.warnings, wxString::FromUTF8("\xE2\x9A\xA0"), wxColour(170, 100, 0));
        bullets(check.problems, wxString::FromUTF8("\xE2\x9C\x96"), wxColour(180, 0, 0));

        if (check.Usable()) {
                wxStaticText *l = new wxStaticText(details, wxID_ANY, "BIOS");
                l->SetFont(l->GetFont().Bold());
                sizer->Add(l, 0, wxLEFT | wxRIGHT | wxTOP, pad);
                bios_choice = new wxChoice(details, wxID_ANY);
                if (check.verdict == TRW_EXACT && check.own_rom) {
                        bios_choice->Append("MetaPC's ROM set for " + wxString(models[check.model]->name));
                        bios_index.push_back(-1);
                }
                for (int pass = 0; pass < 2; pass++)
                        for (size_t i = 0; i < board.bios.size(); i++) {
                                const TrwBios &b = board.bios[i];
                                if (b.generic != (pass == 1))
                                        continue;
                                wxString label = b.vendor;
                                if (!b.core_version.empty())
                                        label += " " + b.core_version;
                                if (!b.version.empty() && b.version != "[unknown]")
                                        label += " - " + b.version;
                                if (!b.post_string.empty())
                                        label += " (" + b.post_string + ")";
                                if (b.generic)
                                        label += " [generic for the chipset]";
                                bios_choice->Append(label.Strip(wxString::both));
                                bios_index.push_back((int)i);
                        }
                if (bios_choice->GetCount())
                        bios_choice->SetSelection(0);
                sizer->Add(bios_choice, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, pad / 2 + FromDIP(2));
                wxStaticText *note = new wxStaticText(
                        details, wxID_ANY,
                        "Board BIOS files are downloaded from The Retro Web when you use the board and kept in MetaPC's "
                        "trw folder. BIOS images belong to their makers.");
                note->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
                note->Wrap(FromDIP(520));
                sizer->Add(note, 0, wxLEFT | wxRIGHT | wxTOP, pad / 2);
        }

        wxStaticText *spec = new wxStaticText(details, wxID_ANY, "Specifications (from The Retro Web)");
        spec->SetFont(spec->GetFont().Bold());
        sizer->Add(spec, 0, wxLEFT | wxRIGHT | wxTOP, pad);
        wxFlexGridSizer *grid = new wxFlexGridSizer(2, FromDIP(4), FromDIP(10));
        grid->AddGrowableCol(1, 1);
        add_field(details, grid, "Chipset", board.chipset.empty() ? wxString("Not listed") : board.chipset);
        for (const TrwChipGroup &g : board.chips)
                add_field(details, grid, g.type, join(g.chips));
        add_field(details, grid, "CPU socket", join(board.sockets));
        add_field(details, grid, "FSB", join(board.fsb));
        add_field(details, grid, "RAM", join(board.ram_types));
        add_field(details, grid, "Max RAM", join(board.max_ram));
        add_field(details, grid, "Expansion slots", join(board.slots));
        add_field(details, grid, "I/O ports", join(board.io_ports));
        sizer->Add(grid, 0, wxEXPAND | wxALL, pad);

        details->SetSizer(sizer, true);
        details->FitInside();
        details->Scroll(0, 0);
        details->Thaw();
        details->Layout();
}

void MotherboardSearchDialog::UseBoard() {
        if (!have_board || !check.Usable())
                return;
        selection.board = board;
        selection.model = check.model;
        selection.bios_path.clear();

        int sel = bios_choice ? bios_choice->GetSelection() : wxNOT_FOUND;
        if (sel == wxNOT_FOUND || sel >= (int)bios_index.size()) {
                wx_messagebox(this, "Choose a BIOS for the board.", "MetaPC", wxOK | wxICON_INFORMATION);
                return;
        }
        if (bios_index[sel] < 0) {
                EndModal(wxID_OK);
                return;
        }
        const TrwBios &bios = board.bios[bios_index[sel]];
        /* Only follow links on The Retro Web itself. */
        if (!bios.href.StartsWith("/") || bios.href.StartsWith("//")) {
                SetIdle("This BIOS link does not point to The Retro Web.");
                return;
        }
        pending_bios = bios_index[sel];
        Fetch(KIND_BIOS, TRW_SITE_URL + bios.href);
}

void MotherboardSearchDialog::BiosDone(const wxMemoryBuffer &data) {
        if (pending_bios < 0 || pending_bios >= (int)board.bios.size()) {
                SetIdle("");
                return;
        }
        wxString error;
        wxString path = store_bios(data, board, board.bios[pending_bios], check.bios_size, error);
        if (path.empty()) {
                SetIdle("This BIOS cannot be used.");
                wx_messagebox(this, error + "\n\nChoose another BIOS for the board.", "MetaPC", wxOK | wxICON_WARNING);
                return;
        }
        selection.bios_path = path;
        SetIdle("");
        EndModal(wxID_OK);
}

bool trw_choose_motherboard(wxWindow *parent, TrwSelection &selection) {
        MotherboardSearchDialog dlg(parent, selection);
        return dlg.ShowModal() == wxID_OK;
}

#else

bool trw_choose_motherboard(wxWindow *parent, TrwSelection &selection) {
        wx_messagebox(parent, "This build of MetaPC cannot connect to The Retro Web (wxWidgets was built without web requests).",
                      "MetaPC", wxOK | wxICON_WARNING);
        return false;
}

#endif
