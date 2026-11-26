#include "wx-panels.h"
#include "wx-common.h"
#include "wx-frontend.h"
#include "wx-machineinfo.h"
#include "wx-utils.h"

#include <wx/clipbrd.h>
#include <wx/dnd.h>
#include <wx/filename.h>
#include <wx/statline.h>
#include <wx/tglbtn.h>
#include <wx/xrc/xmlres.h>

#include <algorithm>

extern "C" {
#include "ibm.h"
#include "config.h"
#include "fdd.h"
#include "hdd.h"
#include "hdd_hostdisk.h"
#include "ide.h"
#include "scsi_zip.h"
#include "sdl2-emu.h"
drive_info_t *get_machine_info(char *, int *);
}

#define RECENT_MAX 10
#define ID_RECENT_BASE (wxID_HIGHEST + 5000)

static wxBitmap panel_bitmap(const char *name) {
        wxBitmap bitmap = wxXmlResource::Get()->LoadBitmap(name);
        return bitmap.IsOk() ? bitmap : wxNullBitmap;
}

static int xrcid_index(const char *name, int index) { return XRCID(wxString::Format("%s[%d]", name, index).utf8_str()); }

const drive_info_t *MachineStatus::Find(drive_type_t type, int drive) const {
        for (int i = 0; i < num_drives; i++)
                if (drives[i].type == type && drives[i].drive == drive)
                        return &drives[i];
        return NULL;
}

/* ---------------------------------------------------------------- recent images */

wxArrayString wx_recent_get(const char *kind) {
        wxArrayString list;
        for (int i = 0; i < RECENT_MAX; i++) {
                const char *s = wx_config_get_string(CFG_GLOBAL, "Recent images", wxString::Format("%s%d", kind, i).mb_str(), "");
                if (s && s[0])
                        list.Add(wxString(s));
        }
        return list;
}

void wx_recent_add(const char *kind, const wxString &path) {
        if (path.empty())
                return;
        wxArrayString list = wx_recent_get(kind);
        for (size_t i = 0; i < list.size();) {
                if (wxFileName(list[i]).SameAs(wxFileName(path)))
                        list.RemoveAt(i);
                else
                        i++;
        }
        list.Insert(path, 0);
        for (int i = 0; i < RECENT_MAX; i++) {
                wxString key = wxString::Format("%s%d", kind, i);
                wx_config_set_string(CFG_GLOBAL, "Recent images", key.mb_str(),
                                     i < (int)list.size() ? (const char *)list[i].mb_str() : "");
        }
}

/* Appends the recent images of a kind to a menu (ids ID_RECENT_BASE + index). */
static void append_recent(wxMenu *menu, const wxArrayString &recent) {
        if (recent.empty()) {
                menu->Append(wxID_ANY, "(no recent images)")->Enable(false);
                return;
        }
        for (size_t i = 0; i < recent.size(); i++) {
                wxString label = wxFileName(recent[i]).GetFullName();
                label.Replace("&", "&&");
                wxMenuItem *item = menu->Append(ID_RECENT_BASE + i, wxString::Format("&%d %s", (int)(i + 1) % 10, label), recent[i]);
                item->Enable(wxFileExists(recent[i]));
        }
}

/* ---------------------------------------------------------------- common widgets */

ActivityLed::ActivityLed(wxWindow *parent) : wxWindow(parent, wxID_ANY), present(false), active(false) {
        SetInitialSize(FromDIP(wxSize(10, 10)));
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        Bind(wxEVT_PAINT, &ActivityLed::OnPaint, this);
}

void ActivityLed::SetState(bool p, bool a) {
        if (p != present || a != active) {
                present = p;
                active = a;
                Refresh();
        }
}

void ActivityLed::OnPaint(wxPaintEvent &event) {
        wxPaintDC dc(this);
        dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
        dc.Clear();
        wxSize size = GetClientSize();
        int r = std::min(size.x, size.y) / 2 - 1;
        wxColour colour = active ? wxColour(40, 230, 40) : (present ? wxColour(20, 90, 20) : wxColour(110, 110, 110));
        dc.SetPen(wxPen(colour.ChangeLightness(60)));
        dc.SetBrush(wxBrush(colour));
        dc.DrawCircle(size.x / 2, size.y / 2, r);
}

MachinePanel::MachinePanel(wxWindow *parent, PanelHost *host, const wxString &name, const wxString &caption)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTAB_TRAVERSAL | wxBORDER_NONE), host(host), name(name),
          caption(caption) {}

void MachinePanel::Command(int id) {
        host->RunCommand(id);
        host->FocusDisplay();
}

wxButton *MachinePanel::MakeButton(const wxString &label, const char *bitmap, const wxString &tooltip,
                                   std::function<void()> action) {
        wxBitmap bmp = bitmap ? panel_bitmap(bitmap) : wxNullBitmap;
        /* An icon-only button falls back to text if its icon is missing. */
        wxString text = (label.empty() && !bmp.IsOk()) ? tooltip : label;
        wxButton *b = new wxButton(this, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
        if (bmp.IsOk())
                b->SetBitmap(bmp);
        if (!tooltip.empty())
                b->SetToolTip(tooltip);
        b->Bind(wxEVT_BUTTON, [this, action](wxCommandEvent &) {
                action();
                host->FocusDisplay();
        });
        return b;
}

static wxStaticText *value_text(wxWindow *parent, const wxString &text, long style = 0) {
        wxStaticText *t = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, style);
        if (style & (wxST_ELLIPSIZE_MIDDLE | wxST_ELLIPSIZE_END))
                t->SetMinSize(wxSize(parent->FromDIP(60), -1));
        return t;
}

static wxStaticText *label_text(wxWindow *parent, const wxString &text) {
        wxStaticText *t = new wxStaticText(parent, wxID_ANY, text);
        t->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        return t;
}

/* ---------------------------------------------------------------- speed graph */

class SpeedGraph : public wxWindow {
    public:
        SpeedGraph(wxWindow *parent) : wxWindow(parent, wxID_ANY) {
                SetInitialSize(wxSize(-1, FromDIP(44)));
                SetMinSize(wxSize(FromDIP(80), FromDIP(44)));
                SetBackgroundStyle(wxBG_STYLE_PAINT);
                Bind(wxEVT_PAINT, &SpeedGraph::OnPaint, this);
        }

        void AddSample(int speed) {
                samples.push_back(speed);
                if (samples.size() > 120)
                        samples.erase(samples.begin());
                Refresh();
        }

    private:
        std::vector<int> samples;

        void OnPaint(wxPaintEvent &event) {
                wxPaintDC dc(this);
                wxSize size = GetClientSize();
                wxColour bg = wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW);
                wxColour grid = wxSystemSettings::GetColour(wxSYS_COLOUR_3DLIGHT);
                dc.SetBackground(wxBrush(bg));
                dc.Clear();
                dc.SetPen(wxPen(wxSystemSettings::GetColour(wxSYS_COLOUR_3DSHADOW)));
                dc.SetBrush(*wxTRANSPARENT_BRUSH);
                dc.DrawRectangle(0, 0, size.x, size.y);

                int max_speed = 150;
                for (int s : samples)
                        max_speed = std::max(max_speed, s + 10);
                auto y_of = [&](int s) { return size.y - 2 - (int)((long long)s * (size.y - 4) / max_speed); };

                dc.SetPen(wxPen(grid, 1, wxPENSTYLE_DOT));
                dc.DrawLine(1, y_of(100), size.x - 1, y_of(100));

                if (samples.size() < 2)
                        return;
                int step = std::max(1, FromDIP(2));
                std::vector<wxPoint> points;
                int x = size.x - 2;
                for (int i = (int)samples.size() - 1; i >= 0 && x >= 1; i--, x -= step)
                        points.push_back(wxPoint(x, y_of(samples[i])));
                dc.SetPen(wxPen(wxColour(30, 120, 220), std::max(1, FromDIP(1))));
                dc.DrawLines(points.size(), points.data());
        }
};

/* ---------------------------------------------------------------- machine information */

class MachineInfoPanel : public MachinePanel {
    public:
        MachineInfoPanel(wxWindow *parent, PanelHost *host) : MachinePanel(parent, host, "info", "Machine") {
                wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

                wxStaticText *title = new wxStaticText(this, wxID_ANY, wxString(config_name));
                title->SetFont(GetFont().Bold().Scaled(1.2f));
                sizer->Add(title, 0, wxALL, FromDIP(6));

                wxFlexGridSizer *grid = new wxFlexGridSizer(2, FromDIP(2), FromDIP(8));
                grid->AddGrowableCol(1);
                auto add = [&](const wxString &label, const wxString &value) {
                        grid->Add(label_text(this, label));
                        wxStaticText *v = value_text(this, value, wxST_ELLIPSIZE_END);
                        v->SetToolTip(value);
                        grid->Add(v, 1, wxEXPAND);
                };
                add("Model:", wx_machine_model_name());
                add("CPU:", wx_machine_cpu_name());
                add("Memory:", wx_machine_memory());
                add("Video:", wx_machine_video_name());
                add("Sound:", wx_machine_sound_name());
                sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));

                sizer->Add(new wxStaticLine(this), 0, wxEXPAND | wxALL, FromDIP(6));

                wxFlexGridSizer *status = new wxFlexGridSizer(2, FromDIP(2), FromDIP(8));
                status->AddGrowableCol(1);
                status->Add(label_text(this, "State:"));
                state = value_text(this, "-");
                state->SetFont(GetFont().Bold());
                status->Add(state);
                status->Add(label_text(this, "Speed:"));
                speed = value_text(this, "-");
                status->Add(speed);
                status->Add(label_text(this, "Video:"));
                fps = value_text(this, "-");
                status->Add(fps);
                status->Add(label_text(this, "Rate:"));
                rate = value_text(this, "-");
                status->Add(rate);
                sizer->Add(status, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(6));

                graph = new SpeedGraph(this);
                graph->SetToolTip("Emulation speed over the last two minutes (dotted line: 100%)");
                sizer->Add(graph, 0, wxEXPAND | wxALL, FromDIP(6));

                SetSizerAndFit(sizer);
                last_sample = 0;
        }

        bool DefaultRight() const override { return true; }

        void UpdateStatus(const MachineStatus &st) override {
                wxString s;
                wxColour colour;
                if (st.state == EMULATION_RUNNING) {
                        s = "Running";
                        colour = wxColour(0, 140, 0);
                } else if (st.state == EMULATION_PAUSED) {
                        s = "Paused";
                        colour = wxColour(200, 120, 0);
                } else {
                        s = "Stopped";
                        colour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
                }
                SetText(state, s);
                if (state->GetForegroundColour() != colour) {
                        state->SetForegroundColour(colour);
                        state->Refresh();
                }
                SetText(speed, st.state == EMULATION_RUNNING ? wxString::Format("%d%%", st.speed) : wxString("-"));
                SetText(fps, st.state == EMULATION_RUNNING ? wxString::Format("%d fps", st.fps) : wxString("-"));

                wxString r;
                if (emu_turbo)
                        r = "Turbo (unlimited)";
                else if (emu_speed == EMU_SPEED_UNLIMITED)
                        r = "Unlimited";
                else
                        r = wxString::Format("%dx", emu_speed);
                SetText(rate, r);

                wxLongLong now = wxGetLocalTimeMillis();
                if (now - last_sample >= 1000) {
                        last_sample = now;
                        if (st.state == EMULATION_RUNNING)
                                graph->AddSample(st.speed);
                }
        }

    private:
        wxStaticText *state, *speed, *fps, *rate;
        SpeedGraph *graph;
        wxLongLong last_sample;

        static void SetText(wxStaticText *t, const wxString &s) {
                if (t->GetLabel() != s)
                        t->SetLabel(s);
        }
};

/* ---------------------------------------------------------------- controls */

static const int panel_speeds[] = {1, 2, 4, 8, EMU_SPEED_UNLIMITED};
static const char *panel_speed_names[] = {"1x (real time)", "2x", "4x", "8x", "Unlimited"};

class ControlsPanel : public MachinePanel {
    public:
        ControlsPanel(wxWindow *parent, PanelHost *host) : MachinePanel(parent, host, "controls", "Controls") {
                wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
                wxGridSizer *grid = new wxGridSizer(2, FromDIP(4), FromDIP(4));

                pause = MakeButton("Pause", "BITMAP_PAUSE24", "Pause or resume the emulation", [this]() {
                        if (emulation_state == EMULATION_PAUSED)
                                resume_emulation();
                        else
                                pause_emulation();
                });
                grid->Add(pause, 0, wxEXPAND);
                grid->Add(MakeButton("Reset", "BITMAP_RESET24", "Hard reset the machine", [this]() { Reset(); }), 0, wxEXPAND);
                grid->Add(MakeButton("Ctrl+Alt+Del", NULL, "Send Ctrl+Alt+Del to the machine",
                                     [this]() { Command(XRCID("IDM_FILE_RESET_CAD")); }),
                          0, wxEXPAND);
                grid->Add(MakeButton("Screenshot", NULL, "Save a screenshot (Ctrl+Alt+PageUp)",
                                     [this]() { Command(XRCID("IDM_SCREENSHOT")); }),
                          0, wxEXPAND);
                grid->Add(MakeButton("Fullscreen", NULL, "Toggle fullscreen (Ctrl+Alt+PageDown)",
                                     [this]() { Command(XRCID("IDM_VID_FULLSCREEN_TOGGLE")); }),
                          0, wxEXPAND);
                grid->Add(MakeButton("Shut down", "BITMAP_STOP24", "Stop this machine and close its window",
                                     [this]() { Command(XRCID("IDM_FILE_EXIT")); }),
                          0, wxEXPAND);
                sizer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(6));

                wxBoxSizer *row = new wxBoxSizer(wxHORIZONTAL);
                row->Add(label_text(this, "Speed:"), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
                speed = new wxChoice(this, wxID_ANY);
                for (const char *s : panel_speed_names)
                        speed->Append(s);
                speed->Bind(wxEVT_CHOICE, [this](wxCommandEvent &event) {
                        Command(xrcid_index("IDM_EMU_SPEED", event.GetSelection()));
                });
                row->Add(speed, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
                turbo = new wxToggleButton(this, wxID_ANY, "Turbo");
                turbo->SetToolTip("Run as fast as possible (Ctrl+Alt+Home)");
                turbo->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &) { Command(XRCID("IDM_EMU_TURBO")); });
                row->Add(turbo, 0, wxALIGN_CENTER_VERTICAL);
                sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(6));

                SetSizerAndFit(sizer);
        }

        bool DefaultRight() const override { return true; }

        void UpdateStatus(const MachineStatus &st) override {
                bool paused = st.state == EMULATION_PAUSED;
                wxString label = paused ? "Resume" : "Pause";
                if (pause->GetLabel() != label) {
                        pause->SetLabel(label);
                        pause->SetBitmap(panel_bitmap(paused ? "BITMAP_PLAY24" : "BITMAP_PAUSE24"));
                }
                int index = 0;
                for (int i = 0; i < (int)(sizeof(panel_speeds) / sizeof(panel_speeds[0])); i++)
                        if (panel_speeds[i] == emu_speed)
                                index = i;
                if (speed->GetSelection() != index)
                        speed->SetSelection(index);
                if (turbo->GetValue() != (emu_turbo != 0))
                        turbo->SetValue(emu_turbo != 0);
        }

    private:
        wxButton *pause;
        wxChoice *speed;
        wxToggleButton *turbo;

        void Reset() {
                int ret = wxID_OK;
                if (confirm_on_reset_machine) {
                        wxDialog dlg;
                        wxXmlResource::Get()->LoadDialog(&dlg, host->HostWindow(), "ConfirmRememberDlg");
                        dlg.FindWindow("IDC_CONFIRM_LABEL")->SetLabel("Reset machine?");
                        dlg.FindWindow("IDC_CONFIRM_REMEMBER")->SetLabel("Do not ask again");
                        dlg.Fit();
                        ret = dlg.ShowModal();
                        if (ret == wxID_OK)
                                confirm_on_reset_machine = !((wxCheckBox *)dlg.FindWindow("IDC_CONFIRM_REMEMBER"))->IsChecked();
                }
                if (ret == wxID_OK)
                        Command(XRCID("IDM_FILE_HRESET"));
        }
};

/* ---------------------------------------------------------------- removable media drives */

/* Accepts image files dropped on a drive panel. */
class MediaDropTarget : public wxFileDropTarget {
    public:
        MediaDropTarget(std::function<void(const wxString &)> load) : load(load) {}
        bool OnDropFiles(wxCoord x, wxCoord y, const wxArrayString &files) override {
                if (files.empty())
                        return false;
                load(files[0]);
                return true;
        }

    private:
        std::function<void(const wxString &)> load;
};

/* Common layout: [icon] title ........ [led]
                  media name
                  [buttons]                    */
class DrivePanel : public MachinePanel {
    public:
        DrivePanel(wxWindow *parent, PanelHost *host, const wxString &name, const wxString &caption, const char *icon)
                : MachinePanel(parent, host, name, caption) {
                sizer = new wxBoxSizer(wxVERTICAL);
                wxBoxSizer *top = new wxBoxSizer(wxHORIZONTAL);
                wxBitmap bmp = panel_bitmap(icon);
                if (bmp.IsOk())
                        top->Add(new wxStaticBitmap(this, wxID_ANY, bmp), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(6));
                title = new wxStaticText(this, wxID_ANY, caption);
                title->SetFont(GetFont().Bold());
                top->Add(title, 1, wxALIGN_CENTER_VERTICAL);
                led = new ActivityLed(this);
                top->Add(led, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
                sizer->Add(top, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

                media = value_text(this, "Empty", wxST_ELLIPSIZE_MIDDLE);
                sizer->Add(media, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(6));

                buttons = new wxBoxSizer(wxHORIZONTAL);
                sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(6));
                SetSizer(sizer);
        }

    protected:
        wxBoxSizer *sizer;
        wxBoxSizer *buttons;
        wxStaticText *title;
        wxStaticText *media;
        ActivityLed *led;
        wxString media_path;

        void SetMedia(const wxString &path, const wxString &empty_text = "Empty") {
                if (path == media_path && !media->GetLabel().empty())
                        return;
                media_path = path;
                media->SetLabel(path.empty() ? empty_text : wxFileName(path).GetFullName());
                media->SetToolTip(path.empty() ? empty_text : path);
                media->SetForegroundColour(path.empty() ? wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT)
                                                        : wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOWTEXT));
                media->Refresh();
        }

        void FinishButtons() {
                buttons->AddStretchSpacer();
                Fit();
                SetMinSize(wxSize(FromDIP(150), GetBestSize().y));
        }

        /* Shows a menu of recent images (plus optional extra items) and returns the chosen path,
           or the chosen extra item's id through *extra_id. */
        wxString ChooseRecent(const char *kind, wxMenu *extra, int *extra_id, wxWindow *anchor) {
                wxMenu menu;
                wxArrayString recent = wx_recent_get(kind);
                append_recent(&menu, recent);
                if (extra) {
                        menu.AppendSeparator();
                        wxMenuItemList &items = extra->GetMenuItems();
                        for (wxMenuItem *item : items) {
                                if (item->IsSeparator())
                                        menu.AppendSeparator();
                                else if (!item->IsSubMenu()) {
                                        wxMenuItem *copy = menu.Append(item->GetId(), item->GetItemLabel(), wxEmptyString,
                                                                       item->GetKind());
                                        if (item->IsCheckable())
                                                copy->Check(item->IsChecked());
                                        copy->Enable(item->IsEnabled());
                                }
                        }
                }
                wxPoint pos = anchor ? anchor->GetPosition() + wxPoint(0, anchor->GetSize().y) : wxDefaultPosition;
                int id = GetPopupMenuSelectionFromUser(menu, pos);
                if (id >= ID_RECENT_BASE && id < ID_RECENT_BASE + (int)recent.size())
                        return recent[id - ID_RECENT_BASE];
                if (extra_id)
                        *extra_id = id;
                return wxEmptyString;
        }
};

class FloppyPanel : public DrivePanel {
    public:
        FloppyPanel(wxWindow *parent, PanelHost *host, int drive)
                : DrivePanel(parent, host, wxString::Format("fdd%d", drive), wxString::Format("Floppy %c:", 'A' + drive),
                             "BITMAP_FLOPPY"),
                  drive(drive) {
                title->SetLabel(wxString::Format("%c:  %s", 'A' + drive, wx_floppy_type_name(fdd_get_type(drive))));

                buttons->Add(MakeButton("", "BITMAP_FOLDER", "Insert a disc image...",
                                        [this]() { Command(XRCID(this->drive ? "IDM_DISC_B" : "IDM_DISC_A")); }),
                             0, wxRIGHT, FromDIP(4));
                eject = MakeButton("", "BITMAP_EJECT", "Eject the disc",
                                   [this]() { Command(XRCID(this->drive ? "IDM_EJECT_B" : "IDM_EJECT_A")); });
                buttons->Add(eject, 0, wxRIGHT, FromDIP(4));
                wxButton *recent = new wxButton(this, wxID_ANY, wxString::FromUTF8("Recent \xe2\x96\xbe"), wxDefaultPosition,
                                                wxDefaultSize, wxBU_EXACTFIT);
                recent->SetToolTip("Insert a recently used disc image");
                recent->Bind(wxEVT_BUTTON, [this, recent](wxCommandEvent &) {
                        wxString path = ChooseRecent("floppy", NULL, NULL, recent);
                        if (!path.empty())
                                wx_load_disc_file(this->drive, path);
                        this->host->FocusDisplay();
                });
                buttons->Add(recent, 0);
                FinishButtons();

                SetDropTarget(new MediaDropTarget([this](const wxString &file) {
                        wx_load_disc_file(this->drive, file);
                        this->host->FocusDisplay();
                }));
                SetToolTip("Drop a disc image here to insert it");
        }

        void UpdateStatus(const MachineStatus &st) override {
                const drive_info_t *info = NULL;
                for (int i = 0; i < st.num_drives; i++)
                        if (st.drives[i].type == DRIVE_TYPE_FDD && st.drives[i].drive_letter == 'A' + drive)
                                info = &st.drives[i];
                bool inserted = info && info->enabled;
                SetMedia(inserted ? wxString(info->fn) : wxString());
                led->SetState(inserted, info && info->readflash);
                if (eject->IsEnabled() != inserted)
                        eject->Enable(inserted);
        }

    private:
        int drive;
        wxButton *eject;
};

class CdromPanel : public DrivePanel {
    public:
        CdromPanel(wxWindow *parent, PanelHost *host) : DrivePanel(parent, host, "cdrom", "CD-ROM", "BITMAP_CDROM") {
                title->SetLabel(wxString::Format("%c:  CD-ROM", 'C' + cdrom_channel));

                buttons->Add(MakeButton("", "BITMAP_FOLDER", "Insert a CD-ROM image (ISO/CUE)",
                                        [this]() { Command(XRCID("IDM_CDROM_IMAGE_LOAD")); }),
                             0, wxRIGHT, FromDIP(4));
                eject = MakeButton("", "BITMAP_EJECT", "Eject the CD", [this]() { Command(XRCID("IDM_CDROM_EMPTY")); });
                buttons->Add(eject, 0, wxRIGHT, FromDIP(4));
                wxButton *more = new wxButton(this, wxID_ANY, wxString::FromUTF8("More \xe2\x96\xbe"), wxDefaultPosition,
                                              wxDefaultSize, wxBU_EXACTFIT);
                more->SetToolTip("Recent images and host CD/DVD drives");
                more->Bind(wxEVT_BUTTON, [this, more](wxCommandEvent &) {
                        wx_refresh_host_cdrom_menu();
                        wxFrame *frame = wxDynamicCast(this->host->HostWindow(), wxFrame);
                        wxMenu *cdrom = (frame && frame->GetMenuBar()) ? wx_getsubmenu(frame->GetMenuBar(), XRCID("IDM_CDROM"))
                                                                      : NULL;
                        int id = wxID_NONE;
                        wxString path = ChooseRecent("cdrom", cdrom, &id, more);
                        if (!path.empty())
                                wx_load_cdrom_image(path);
                        else if (id != wxID_NONE)
                                this->host->RunCommand(id);
                        this->host->FocusDisplay();
                });
                buttons->Add(more, 0);
                FinishButtons();

                SetDropTarget(new MediaDropTarget([this](const wxString &file) {
                        wx_load_cdrom_image(file);
                        this->host->FocusDisplay();
                }));
                SetToolTip("Drop an ISO/CUE image here to insert it");
        }

        void UpdateStatus(const MachineStatus &st) override {
                const drive_info_t *info = st.Find(DRIVE_TYPE_CDROM, cdrom_channel);
                bool inserted = info && info->enabled;
                SetMedia(inserted ? wxString(info->fn) : wxString());
                led->SetState(inserted, info && info->readflash);
                if (eject->IsEnabled() != (cdrom_drive != 0))
                        eject->Enable(cdrom_drive != 0);
        }

    private:
        wxButton *eject;
};

class ZipPanel : public DrivePanel {
    public:
        ZipPanel(wxWindow *parent, PanelHost *host) : DrivePanel(parent, host, "zip", "ZIP drive", "BITMAP_FLOPPY") {
                title->SetLabel(wxString::Format("%c:  ZIP drive", 'C' + zip_channel));
                buttons->Add(MakeButton("", "BITMAP_FOLDER", "Insert a ZIP disc image",
                                        [this]() { Command(XRCID("IDM_DISC_ZIP")); }),
                             0, wxRIGHT, FromDIP(4));
                eject = MakeButton("", "BITMAP_EJECT", "Eject the ZIP disc", [this]() { Command(XRCID("IDM_EJECT_ZIP")); });
                buttons->Add(eject, 0, wxRIGHT, FromDIP(4));
                wxButton *recent = new wxButton(this, wxID_ANY, wxString::FromUTF8("Recent \xe2\x96\xbe"), wxDefaultPosition,
                                                wxDefaultSize, wxBU_EXACTFIT);
                recent->Bind(wxEVT_BUTTON, [this, recent](wxCommandEvent &) {
                        wxString path = ChooseRecent("zip", NULL, NULL, recent);
                        if (!path.empty())
                                wx_load_zip_file(path);
                        this->host->FocusDisplay();
                });
                buttons->Add(recent, 0);
                FinishButtons();

                SetDropTarget(new MediaDropTarget([this](const wxString &file) {
                        wx_load_zip_file(file);
                        this->host->FocusDisplay();
                }));
        }

        void UpdateStatus(const MachineStatus &st) override {
                const drive_info_t *info = NULL;
                for (int i = 0; i < st.num_drives; i++)
                        if (st.drives[i].type == DRIVE_TYPE_FDD && st.drives[i].drive_letter == 'C' + zip_channel)
                                info = &st.drives[i];
                bool inserted = zip_loaded() != 0;
                wxArrayString recent = wx_recent_get("zip");
                SetMedia(inserted ? (recent.empty() ? wxString("Disc inserted") : recent[0]) : wxString());
                led->SetState(inserted, info && info->readflash);
                if (eject->IsEnabled() != inserted)
                        eject->Enable(inserted);
        }

    private:
        wxButton *eject;
};

/* ---------------------------------------------------------------- hard disks */

class StoragePanel : public MachinePanel {
    public:
        StoragePanel(wxWindow *parent, PanelHost *host) : MachinePanel(parent, host, "storage", "Hard disks") {
                wxFlexGridSizer *grid = new wxFlexGridSizer(3, FromDIP(4), FromDIP(6));
                grid->AddGrowableCol(1);
                wxBitmap bmp = panel_bitmap("BITMAP_HARDDISK");
                wxBitmap cf_bmp = panel_bitmap("BITMAP_COMPACTFLASH");
                int num = hdd_controller_current_is_mfm() ? 2 : (hdd_controller_current_is_scsi() ? 7 : 4);
                for (int i = 0; i < num; i++) {
                        if (!ide_fn[i][0] || (!hdd_controller_current_is_mfm() && (i == cdrom_channel || i == zip_channel)))
                                continue;
                        bool host_disk = hostdisk_is_device_path(ide_fn[i]) != 0;
                        const wxBitmap &icon = (host_disk && cf_bmp.IsOk()) ? cf_bmp : bmp;
                        wxBoxSizer *name = new wxBoxSizer(wxHORIZONTAL);
                        if (icon.IsOk())
                                name->Add(new wxStaticBitmap(this, wxID_ANY, icon), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
                        wxStaticText *letter = new wxStaticText(this, wxID_ANY, wxString::Format("%c:", 'C' + i));
                        letter->SetFont(GetFont().Bold());
                        name->Add(letter, 0, wxALIGN_CENTER_VERTICAL);
                        grid->Add(name, 0, wxALIGN_CENTER_VERTICAL);
                        wxString file_name = wxFileName(ide_fn[i]).GetFullName();
                        if (host_disk) {
                                hostdisk_info_t info;
                                file_name = hostdisk_query(ide_fn[i], &info) ? wxString("Card not present")
                                                                             : wxString::FromUTF8(info.model);
                        }
                        wxStaticText *file = value_text(this, file_name, wxST_ELLIPSIZE_MIDDLE);
                        file->SetToolTip(ide_fn[i]);
                        grid->Add(file, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
                        ActivityLed *led = new ActivityLed(this);
                        grid->Add(led, 0, wxALIGN_CENTER_VERTICAL);
                        leds.push_back(std::make_pair(i, led));

                        if (host_disk) {
                                /* The geometry to enter in the BIOS of the real machine the card
                                   will go into: the same one the emulated machine uses. */
                                wxString chs = wxString::Format("C %d H %d S %d", hdc[i].tracks, hdc[i].hpc, hdc[i].spt);
                                grid->AddSpacer(0);
                                wxStaticText *geometry = value_text(this, chs);
                                geometry->SetToolTip("Geometry to enter in the BIOS of the real machine (cylinders, heads, sectors)");
                                grid->Add(geometry, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
                                grid->Add(MakeButton("Copy", NULL, "Copy the geometry to the clipboard",
                                                     [chs]() {
                                                             if (wxTheClipboard->Open()) {
                                                                     wxTheClipboard->SetData(new wxTextDataObject(chs));
                                                                     wxTheClipboard->Close();
                                                             }
                                                     }),
                                          0, wxALIGN_CENTER_VERTICAL);
                        }
                }
                wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
                sizer->Add(grid, 1, wxEXPAND | wxALL, FromDIP(6));
                SetSizerAndFit(sizer);
                SetMinSize(wxSize(FromDIP(150), GetBestSize().y));
        }

        void UpdateStatus(const MachineStatus &st) override {
                for (auto &l : leds) {
                        const drive_info_t *info = st.Find(DRIVE_TYPE_HDD, l.first);
                        l.second->SetState(info != NULL, info && info->readflash);
                }
        }

    private:
        std::vector<std::pair<int, ActivityLed *>> leds;
};

/* ---------------------------------------------------------------- factory */

static bool has_cdrom() { return !hdd_controller_current_is_mfm() && cdrom_channel >= 0 && cdrom_channel < 7 && cdrom_drive >= 0; }

static bool has_zip() { return !hdd_controller_current_is_mfm() && zip_channel >= 0 && zip_channel < 7; }

static bool has_hdds() {
        int num = hdd_controller_current_is_mfm() ? 2 : (hdd_controller_current_is_scsi() ? 7 : 4);
        for (int i = 0; i < num; i++)
                if (ide_fn[i][0] && (hdd_controller_current_is_mfm() || (i != cdrom_channel && i != zip_channel)))
                        return true;
        return false;
}

wxString wx_panels_signature() {
        wxString s = wxString::Format("%d,%d,", fdd_get_type(0), fdd_get_type(1));
        s += has_cdrom() ? wxString::Format("cd%d,", cdrom_channel) : wxString();
        s += has_zip() ? wxString::Format("zip%d,", zip_channel) : wxString();
        for (int i = 0; i < 7; i++)
                s += wxString::Format("%s|", ide_fn[i]);
        s += hdd_controller_name;
        return s;
}

std::vector<MachinePanel *> wx_create_panels(wxWindow *parent, PanelHost *host) {
        std::vector<MachinePanel *> panels;
        panels.push_back(new MachineInfoPanel(parent, host));
        panels.push_back(new ControlsPanel(parent, host));
        for (int i = 0; i < 2; i++)
                if (fdd_get_type(i) > 0)
                        panels.push_back(new FloppyPanel(parent, host, i));
        if (has_cdrom())
                panels.push_back(new CdromPanel(parent, host));
        if (has_zip())
                panels.push_back(new ZipPanel(parent, host));
        if (has_hdds())
                panels.push_back(new StoragePanel(parent, host));
        return panels;
}
