#ifndef _WX_TRW_H_
#define _WX_TRW_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <vector>

/* Motherboard lookups on The Retro Web (https://theretroweb.com), used to create a machine
   from a real board. MetaPC ships none of its data: everything is fetched when the user asks
   for it, cached on the user's computer and credited to The Retro Web under CC BY-SA 4.0. */

#define TRW_SITE_URL "https://theretroweb.com"
#define TRW_LICENSE_NAME "CC BY-SA 4.0"
#define TRW_LICENSE_URL "https://creativecommons.org/licenses/by-sa/4.0/"
/* Section of a machine's configuration that records the board it was created from. */
#define TRW_CONFIG_SECTION "The Retro Web"

struct TrwSearchResult {
        wxString slug;
        wxString title;
};

struct TrwBios {
        wxString vendor;
        wxString post_string;
        wxString core_version;
        wxString version;
        wxString href;     /* download path on the site */
        wxString filename; /* name the site gives the download */
        bool generic;      /* a generic BIOS for the board's chipset, not one for this board */
};

struct TrwChipGroup {
        wxString type;
        std::vector<wxString> chips;
};

struct TrwBoard {
        wxString slug;
        wxString title;
        wxString chipset;
        std::vector<TrwChipGroup> chips;
        std::vector<wxString> sockets;
        std::vector<wxString> fsb;
        std::vector<wxString> ram_types;
        std::vector<wxString> max_ram;
        std::vector<wxString> slots;
        std::vector<wxString> io_ports;
        std::vector<TrwBios> bios;

        wxString Url() const;
};

bool trw_parse_search(const wxString &html, std::vector<TrwSearchResult> &results);
bool trw_parse_board(const wxString &html, const wxString &slug, TrwBoard &board);

enum TrwVerdict {
        TRW_EXACT,      /* MetaPC emulates this very board */
        TRW_COMPATIBLE, /* MetaPC emulates its chipset, and runs its BIOS on a board with that chipset */
        TRW_UNSUPPORTED
};

struct TrwCheck {
        TrwVerdict verdict;
        int model;           /* index in models[] of the hardware MetaPC uses, or -1 */
        long bios_size;      /* BIOS image size that hardware takes */
        bool own_rom;        /* the model's own ROM set is installed (TRW_EXACT only) */
        wxString summary;
        std::vector<wxString> good;
        std::vector<wxString> warnings;
        std::vector<wxString> problems;

        bool Usable() const { return verdict != TRW_UNSUPPORTED; }
};

TrwCheck trw_check_board(const TrwBoard &board);

/* What the user picked in the motherboard search. */
struct TrwSelection {
        TrwBoard board;
        int model;          /* index in models[] */
        wxString bios_path; /* BIOS image to use, or empty for the model's own ROM set */
};

/* Shows the motherboard search. Returns true and fills selection when the user picks a board
   MetaPC can use. */
bool trw_choose_motherboard(wxWindow *parent, TrwSelection &selection);

/* Records the board in the loaded machine configuration, with its source and licence, and the
   limits it sets on the machine: the emulated model, bus speeds, maximum RAM and slot types. */
void trw_save_attribution(const TrwBoard &board, const char *model_internal_name);
void trw_clear_attribution();

/* The limits a board from The Retro Web sets on its machine, read from the loaded configuration. */
struct TrwLimits {
        bool active;             /* the machine was created from a board */
        wxString board;          /* the board's name */
        int model;               /* index in models[] the board is emulated with, or -1 */
        std::vector<double> fsb; /* bus speeds in MHz; empty if unknown */
        long max_ram_kb;         /* 0 if unknown */
        bool slots_known;
        bool isa8, isa16, vlb, pci, agp;
};

TrwLimits trw_load_limits();
/* Whether the CPU runs at one of the board's bus speeds. */
bool trw_cpu_fits(const TrwLimits &limits, const struct CPU *cpu);
/* Whether a card with these device flags (DEVICE_AT, DEVICE_PCI...) fits one of the board's slots. */
bool trw_bus_fits(const TrwLimits &limits, int device_flags);
/* Whether a card fits one of the board's slots, by the buses the real card was made for when
   MetaPC knows them (its device flags alone do not say, e.g., that a card was PCI or VLB only). */
bool trw_device_fits(const TrwLimits &limits, const struct device_t *device);

#endif /* _WX_TRW_H_ */
