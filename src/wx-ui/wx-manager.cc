#include "wx-manager.h"
#include "wx-app.h"
#include "wx-config.h"
#include "wx-hostconfig.h"
#include "wx-machineinfo.h"
#include "wx-trw.h"
#include "wx-utils.h"

#include <wx/dir.h>
#include <wx/display.h>
#include <wx/statline.h>
#include <wx/filename.h>
#include <wx/snglinst.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>
#include <wx/xrc/xmlres.h>
#include <wx/textfile.h>
#include <wx/process.h>

#include <algorithm>
#include <climits>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#endif

extern "C" {
#include "ibm.h"
#include "config.h"
#include "paths.h"
#include "cpu.h"
#include "mem_bios.h"
#include "model.h"
}

/* How long a machine counts as "starting" after launch, until its process shows up as running. */
#define MANAGER_START_TIMEOUT_MS 20000
#define MANAGER_POLL_MS 1000

enum {
        ID_MGR_NEW = wxID_HIGHEST + 4000,
        ID_MGR_SETTINGS,
        ID_MGR_START,
        ID_MGR_STOP,
        ID_MGR_COPY,
        ID_MGR_RENAME,
        ID_MGR_DELETE,
        ID_MGR_FOLDER,
        ID_MGR_HOSTCONFIG,
};

static wxBitmap load_bitmap(const char *name) {
        wxBitmap bitmap = wxXmlResource::Get()->LoadBitmap(name);
        return bitmap.IsOk() ? bitmap : wxNullBitmap;
}

/* ---------------------------------------------------------------- machine list */

class MachineListBox : public wxVListBox {
    public:
        MachineListBox(wxWindow *parent, ManagerFrame *manager)
                : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), manager(manager) {
                icon = load_bitmap("BITMAP_MGR_MACHINE");
                running = load_bitmap("BITMAP_RUNNING");
                SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_LISTBOX));
        }

    private:
        ManagerFrame *manager;
        wxBitmap icon;
        wxBitmap running;

        wxCoord OnMeasureItem(size_t n) const override {
                wxClientDC dc(const_cast<MachineListBox *>(this));
                dc.SetFont(GetFont());
                int line = dc.GetCharHeight();
                int icon_h = icon.IsOk() ? icon.GetScaledHeight() : FromDIP(32);
                return std::max(icon_h, line * 2 + FromDIP(4)) + FromDIP(12);
        }

        void OnDrawItem(wxDC &dc, const wxRect &rect, size_t n) const override {
                if (n >= manager->machines.size())
                        return;
                const ManagerFrame::Machine &m = manager->machines[n];
                bool selected = IsSelected(n);
                wxColour text = wxSystemSettings::GetColour(selected ? wxSYS_COLOUR_HIGHLIGHTTEXT : wxSYS_COLOUR_LISTBOXTEXT);

                int pad = FromDIP(6);
                int x = rect.x + pad;
                if (icon.IsOk()) {
                        dc.DrawBitmap(icon, x, rect.y + (rect.height - icon.GetScaledHeight()) / 2, true);
                        x += icon.GetScaledWidth() + pad;
                }

                wxFont bold = GetFont().Bold();
                dc.SetFont(bold);
                dc.SetTextForeground(text);
                int line = dc.GetCharHeight();
                int top = rect.y + (rect.height - line * 2 - FromDIP(2)) / 2;
                dc.DrawText(m.name, x, top);

                dc.SetFont(GetFont());
                int y2 = top + line + FromDIP(2);
                wxString state;
                if (m.running) {
                        state = "Running";
                        if (running.IsOk()) {
                                dc.DrawBitmap(running, x, y2 + (line - running.GetScaledHeight()) / 2, true);
                                x += running.GetScaledWidth() + FromDIP(4);
                        }
                } else if (m.starting)
                        state = "Starting...";
                else
                        state = "Powered off";
                if (!selected)
                        dc.SetTextForeground(m.running ? wxColour(0, 120, 0) : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
                dc.DrawText(state, x, y2);
        }
};

/* ---------------------------------------------------------------- manager frame */

ManagerFrame::ManagerFrame(Frame *app_frame)
        : wxFrame(NULL, wxID_ANY, "MetaPC Machine Manager", wxDefaultPosition, wxDefaultSize), app_frame(app_frame),
          timer(this), details_running(false) {
        wx_set_app_icon(this);

        BuildMenu();
        BuildToolBar();

        splitter = new wxSplitterWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxSP_LIVE_UPDATE | wxSP_3DSASH);
        splitter->SetMinimumPaneSize(FromDIP(160));
        list = new MachineListBox(splitter, this);
        details = new wxScrolledWindow(splitter, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxHSCROLL);
        details->SetScrollRate(FromDIP(10), FromDIP(10));
        details->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_WINDOW));
        splitter->SplitVertically(list, details, FromDIP(260));
        splitter->SetSashGravity(0.0);

        CreateStatusBar();

        list->Bind(wxEVT_LISTBOX, [this](wxCommandEvent &) {
                UpdateCommands();
                UpdateDetails(false);
        });
        list->Bind(wxEVT_LISTBOX_DCLICK, &ManagerFrame::OnStart, this);
        list->Bind(wxEVT_CONTEXT_MENU, &ManagerFrame::OnContextMenu, this);
        list->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) {
                if (event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER) {
                        wxCommandEvent cmd;
                        OnStart(cmd);
                } else if (event.GetKeyCode() == WXK_DELETE) {
                        wxCommandEvent cmd;
                        OnDelete(cmd);
                } else
                        event.Skip();
        });

        Bind(wxEVT_MENU, &ManagerFrame::OnNew, this, ID_MGR_NEW);
        Bind(wxEVT_MENU, &ManagerFrame::OnSettings, this, ID_MGR_SETTINGS);
        Bind(wxEVT_MENU, &ManagerFrame::OnStart, this, ID_MGR_START);
        Bind(wxEVT_MENU, &ManagerFrame::OnStop, this, ID_MGR_STOP);
        Bind(wxEVT_MENU, &ManagerFrame::OnCopy, this, ID_MGR_COPY);
        Bind(wxEVT_MENU, &ManagerFrame::OnRename, this, ID_MGR_RENAME);
        Bind(wxEVT_MENU, &ManagerFrame::OnDelete, this, ID_MGR_DELETE);
        Bind(wxEVT_MENU, &ManagerFrame::OnOpenFolder, this, ID_MGR_FOLDER);
        Bind(wxEVT_MENU, &ManagerFrame::OnHostConfig, this, ID_MGR_HOSTCONFIG);
        Bind(wxEVT_MENU, &ManagerFrame::OnAbout, this, wxID_ABOUT);
        Bind(wxEVT_MENU, [this](wxCommandEvent &) { Close(); }, wxID_EXIT);
        Bind(wxEVT_CLOSE_WINDOW, &ManagerFrame::OnClose, this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { RefreshMachines(); });

        LoadGeometry();
        RefreshMachines(true);
        timer.Start(MANAGER_POLL_MS);
        list->SetFocus();
}

ManagerFrame::~ManagerFrame() { timer.Stop(); }

void ManagerFrame::BuildMenu() {
        wxMenuBar *menubar = new wxMenuBar();

        wxMenu *file = new wxMenu();
#ifdef USE_NETWORKING
        file->Append(ID_MGR_HOSTCONFIG, "&Host network settings...");
        file->AppendSeparator();
#endif
        file->Append(ID_MGR_FOLDER, "&Open configs folder");
        file->AppendSeparator();
        file->Append(wxID_EXIT, "E&xit");
        menubar->Append(file, "&File");

        wxMenu *machine = new wxMenu();
        machine->Append(ID_MGR_NEW, "&New...\tCtrl+N");
        machine->Append(ID_MGR_SETTINGS, "&Settings...\tCtrl+S");
        machine->Append(ID_MGR_START, "S&tart\tCtrl+Enter");
        machine->Append(ID_MGR_STOP, "&Force stop...");
        machine->AppendSeparator();
        machine->Append(ID_MGR_COPY, "&Clone...");
        machine->Append(ID_MGR_RENAME, "&Rename...\tF2");
        machine->Append(ID_MGR_DELETE, "&Delete");
        menubar->Append(machine, "&Machine");

        wxMenu *help = new wxMenu();
        help->Append(wxID_ABOUT, "&About MetaPC...");
        menubar->Append(help, "&Help");

        SetMenuBar(menubar);
}

void ManagerFrame::BuildToolBar() {
        wxToolBar *tb = CreateToolBar(wxTB_HORIZONTAL | wxTB_FLAT | wxTB_TEXT | wxTB_NODIVIDER);
        tb->AddTool(ID_MGR_NEW, "New", load_bitmap("BITMAP_MGR_NEW"), "Create a new machine");
        tb->AddTool(ID_MGR_SETTINGS, "Settings", load_bitmap("BITMAP_MGR_SETTINGS"), "Change the selected machine's hardware");
        tb->AddTool(ID_MGR_START, "Start", load_bitmap("BITMAP_MGR_START"), "Start the selected machine in a new window");
        tb->AddTool(ID_MGR_STOP, "Stop", load_bitmap("BITMAP_MGR_STOP"),
                    "Force the selected machine's process to stop (for a machine that does not respond)");
        tb->AddSeparator();
        tb->AddTool(ID_MGR_COPY, "Clone", load_bitmap("BITMAP_MGR_COPY"), "Copy the selected machine's configuration");
        tb->AddTool(ID_MGR_RENAME, "Rename", load_bitmap("BITMAP_MGR_RENAME"), "Rename the selected machine");
        tb->AddTool(ID_MGR_DELETE, "Delete", load_bitmap("BITMAP_MGR_DELETE"), "Delete the selected machine");
#ifdef USE_NETWORKING
        tb->AddSeparator();
        tb->AddTool(ID_MGR_HOSTCONFIG, "Network", load_bitmap("BITMAP_MGR_NETWORK"), "Host network settings");
#endif
        tb->Realize();
}

wxString ManagerFrame::ConfigPath(const wxString &name) {
        char s[512];
        strcpy(s, configs_path);
        put_backslash(s);
        return wxString(s) + name + ".cfg";
}

int ManagerFrame::SelectedIndex() {
        int sel = list->GetSelection();
        return (sel >= 0 && sel < (int)machines.size()) ? sel : -1;
}

wxString ManagerFrame::SelectedName() {
        int sel = SelectedIndex();
        return sel >= 0 ? machines[sel].name : wxString();
}

void ManagerFrame::SelectName(const wxString &name) {
        for (size_t i = 0; i < machines.size(); i++) {
                if (machines[i].name == name) {
                        list->SetSelection(i);
                        list->ScrollToRow(i);
                        break;
                }
        }
        UpdateCommands();
        UpdateDetails(false);
}

bool ManagerFrame::IsRunning(const wxString &name) {
#ifdef _WIN32
        /* The machine holds a mutex with this name while it runs. A machine running with
           administrator rights (it uses a CompactFlash card) creates one this process may not
           open: being refused access still means it exists. */
        HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, wx_machine_instance_name(ConfigPath(name)).wc_str());
        if (mutex) {
                CloseHandle(mutex);
                return true;
        }
        return GetLastError() == ERROR_ACCESS_DENIED;
#else
        wxLogNull nolog;
        wxSingleInstanceChecker checker;
        if (!checker.Create(wx_machine_instance_name(ConfigPath(name))))
                return false;
        return checker.IsAnotherRunning();
#endif
}

/* Rescans the configs folder (machines may be added from outside) and each machine's state. */
void ManagerFrame::RefreshMachines(bool force_details) {
        wxArrayString names;
        wxDir dir(configs_path);
        wxString file;
        if (dir.IsOpened()) {
                for (bool found = dir.GetFirst(&file, "*.cfg", wxDIR_FILES); found; found = dir.GetNext(&file))
                        names.Add(wxFileName(file).GetName());
        }
        names.Sort([](const wxString &a, const wxString &b) { return a.CmpNoCase(b); });

        wxString selected = SelectedName();
        wxLongLong now = wxGetLocalTimeMillis();

        std::vector<Machine> updated;
        for (const wxString &name : names) {
                Machine m;
                m.name = name;
                m.running = IsRunning(name);
                m.starting = false;
                auto it = launch_times.find(name);
                if (it != launch_times.end()) {
                        if (m.running || now - it->second > MANAGER_START_TIMEOUT_MS)
                                launch_times.erase(it);
                        else
                                m.starting = true;
                }
                updated.push_back(m);
        }

        bool changed = updated.size() != machines.size();
        for (size_t i = 0; !changed && i < updated.size(); i++)
                changed = updated[i].name != machines[i].name || updated[i].running != machines[i].running ||
                          updated[i].starting != machines[i].starting;
        if (!changed && !force_details)
                return;

        machines = updated;
        list->SetItemCount(machines.size());
        int index = -1;
        for (size_t i = 0; i < machines.size(); i++)
                if (machines[i].name == selected)
                        index = i;
        if (index < 0 && !machines.empty())
                index = 0;
        list->SetSelection(index);
        list->RefreshAll();

        int running = 0;
        for (const Machine &m : machines)
                running += m.running;
        SetStatusText(wxString::Format("%d machine%s, %d running", (int)machines.size(), machines.size() == 1 ? "" : "s",
                                       running));

        UpdateCommands();
        UpdateDetails(force_details);
}

void ManagerFrame::UpdateCommands() {
        int sel = SelectedIndex();
        bool has = sel >= 0;
        bool busy = has && (machines[sel].running || machines[sel].starting);
        wxToolBar *tb = GetToolBar();
        wxMenuBar *mb = GetMenuBar();
        auto enable = [&](int id, bool on) {
                tb->EnableTool(id, on);
                mb->Enable(id, on);
        };
        enable(ID_MGR_SETTINGS, has && !busy);
        enable(ID_MGR_START, has && !busy);
        enable(ID_MGR_STOP, busy);
        enable(ID_MGR_COPY, has);
        enable(ID_MGR_RENAME, has && !busy);
        enable(ID_MGR_DELETE, has && !busy);
}

/* ---------------------------------------------------------------- details pane */

static wxStaticText *add_text(wxWindow *parent, const wxString &text, const wxFont &font,
                              const wxColour &colour = wxNullColour) {
        wxStaticText *t = new wxStaticText(parent, wxID_ANY, text, wxDefaultPosition, wxDefaultSize, wxST_NO_AUTORESIZE);
        t->SetFont(font);
        if (colour.IsOk())
                t->SetForegroundColour(colour);
        t->SetLabel(text);
        t->SetInitialSize(t->GetBestSize());
        return t;
}

static void add_section(wxWindow *parent, wxSizer *sizer, const MachineInfoSection &section) {
        wxWindow *top = parent;
        wxBoxSizer *header = new wxBoxSizer(wxHORIZONTAL);
        wxBitmap bitmap = load_bitmap(section.icon.mb_str());
        if (bitmap.IsOk())
                header->Add(new wxStaticBitmap(top, wxID_ANY, bitmap), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, top->FromDIP(6));
        header->Add(add_text(top, section.title, top->GetFont().Bold()), 0, wxALIGN_CENTER_VERTICAL);
        sizer->Add(header, 0, wxLEFT | wxRIGHT | wxTOP, top->FromDIP(10));

        wxFlexGridSizer *grid = new wxFlexGridSizer(2, top->FromDIP(4), top->FromDIP(16));
        wxColour label_colour = wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT);
        for (const MachineInfoItem &item : section.items) {
                grid->Add(add_text(top, item.label + ":", top->GetFont(), label_colour));
                grid->Add(add_text(top, item.value, top->GetFont()));
        }
        sizer->Add(grid, 0, wxLEFT | wxTOP, top->FromDIP(32));
        sizer->Add(new wxStaticLine(top), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, top->FromDIP(10));
}

void ManagerFrame::UpdateDetails(bool force) {
        int sel = SelectedIndex();
        wxString name = SelectedName();
        bool running = sel >= 0 && machines[sel].running;
        if (!force && name == details_shown && running == details_running)
                return;
        details_shown = name;
        details_running = running;

        details->Freeze();
        details->DestroyChildren();
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);

        if (sel < 0) {
                sizer->Add(add_text(details,
                                    "No machines yet.\n\nUse New to create your first machine.",
                                    details->GetFont()),
                           0, wxALL, FromDIP(20));
        } else {
                wxString path = ConfigPath(name);
                loadconfig((char *)(const char *)path.mb_str());

                wxFont title_font = details->GetFont().Bold().Scaled(1.6f);
                sizer->Add(add_text(details, name, title_font), 0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
                wxString subtitle = wx_machine_model_name() + "  |  " + wx_machine_cpu_name() + "  |  " + wx_machine_memory();
                sizer->Add(add_text(details, subtitle, details->GetFont(), wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT)), 0,
                           wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
                if (running)
                        sizer->Add(add_text(details,
                                            "This machine is running. Change its settings from its own window, or shut it down "
                                            "to change them here.",
                                            details->GetFont().Italic(), wxColour(0, 120, 0)),
                                   0, wxLEFT | wxRIGHT | wxTOP, FromDIP(12));
                sizer->Add(new wxStaticLine(details), 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

                MachineInfoSection general = {"General", "BITMAP_GENERAL", {}};
                general.items.push_back({"Name", name});
                general.items.push_back({"State", running ? "Running" : "Powered off"});
                general.items.push_back({"Configuration file", path});
                add_section(details, sizer, general);
                for (const MachineInfoSection &section : wx_machine_info())
                        add_section(details, sizer, section);
                sizer->AddSpacer(FromDIP(12));
        }

        details->SetSizer(sizer, true);
        details->FitInside();
        details->Layout();
        details->Scroll(0, 0);
        details->Thaw();
}

/* ---------------------------------------------------------------- commands */

bool ManagerFrame::AskName(const wxString &title, wxString &name) {
        while (wx_textentrydialog(this, "Enter name:", title, name, 1, 64, name)) {
                if (name.find_first_of("/<>:\"\\|?*") != wxString::npos) {
                        wx_simple_messagebox("Invalid name", "The following characters cannot be in the "
                                                             "name: /, <, >, :, \", \\, |, ?, *");
                        continue;
                }
                if (wxFileExists(ConfigPath(name))) {
                        wx_simple_messagebox("Already exists", "A configuration with that name already exists.");
                        continue;
                }
                return true;
        }
        return false;
}

/* Asks the new machine's name and how to set it up: the classic configuration dialog, or
   starting from a real motherboard found on The Retro Web. */
ManagerFrame::NewMachineMode ManagerFrame::AskNewMachine(wxString &name) {
        enum { ID_NEW_CLASSIC = wxID_HIGHEST + 4100, ID_NEW_BY_BOARD };

        wxDialog dlg(this, wxID_ANY, "New machine");
        wxBoxSizer *sizer = new wxBoxSizer(wxVERTICAL);
        sizer->Add(new wxStaticText(&dlg, wxID_ANY, "Enter name:"), 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(12));
        wxTextCtrl *text =
                new wxTextCtrl(&dlg, wxID_ANY, name, wxDefaultPosition, dlg.FromDIP(wxSize(360, -1)), wxTE_PROCESS_ENTER);
        text->SetMaxLength(64);
        sizer->Add(text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(12));
        wxStaticText *hint = new wxStaticText(&dlg, wxID_ANY,
                                              "Classic: choose the emulated hardware yourself.\n"
                                              "By motherboard: search a real board on The Retro Web, check whether MetaPC can "
                                              "emulate it, then choose the rest.");
        hint->SetForegroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
        hint->Wrap(dlg.FromDIP(360));
        sizer->Add(hint, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP(12));

        wxBoxSizer *buttons = new wxBoxSizer(wxHORIZONTAL);
        wxButton *classic = new wxButton(&dlg, ID_NEW_CLASSIC, "Classic");
        wxButton *by_board = new wxButton(&dlg, ID_NEW_BY_BOARD, "By motherboard");
        buttons->Add(classic, 0, wxRIGHT, dlg.FromDIP(6));
        buttons->Add(by_board, 0, wxRIGHT, dlg.FromDIP(6));
        buttons->Add(new wxButton(&dlg, wxID_CANCEL, "Cancel"), 0);
        sizer->Add(buttons, 0, wxALIGN_RIGHT | wxALL, dlg.FromDIP(12));
        dlg.SetSizerAndFit(sizer);
        dlg.CentreOnParent();
        classic->SetDefault();
        text->SetFocus();

        auto choose = [&](int id) {
                wxString value = text->GetValue().Strip(wxString::both);
                if (value.empty()) {
                        wx_messagebox(&dlg, "Enter a name for the machine.", "New machine", wxOK);
                        return;
                }
                if (value.find_first_of("/<>:\"\\|?*") != wxString::npos) {
                        wx_simple_messagebox("Invalid name", "The following characters cannot be in the "
                                                             "name: /, <, >, :, \", \\, |, ?, *");
                        return;
                }
                if (wxFileExists(ConfigPath(value))) {
                        wx_simple_messagebox("Already exists", "A configuration with that name already exists.");
                        return;
                }
                name = value;
                dlg.EndModal(id);
        };
        classic->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { choose(ID_NEW_CLASSIC); });
        by_board->Bind(wxEVT_BUTTON, [&](wxCommandEvent &) { choose(ID_NEW_BY_BOARD); });
        text->Bind(wxEVT_TEXT_ENTER, [&](wxCommandEvent &) { choose(ID_NEW_CLASSIC); });

        int result = dlg.ShowModal();
        return result == ID_NEW_CLASSIC ? NEW_CLASSIC : result == ID_NEW_BY_BOARD ? NEW_BY_BOARD : NEW_CANCEL;
}

/* Loads the default settings, so a new machine starts clean instead of inheriting those of
   the machine shown last. cfg does not exist yet, so every setting takes its default. */
static void load_default_config(const wxString &cfg) { loadconfig((char *)(const char *)cfg.mb_str()); }

/* Switches the loaded configuration to another model with that model's defaults. */
static void set_model_defaults(int new_model) {
        model = new_model;
        romset = model_getromset();
        cpu_manufacturer = 0;
        cpu = 0;
        fpu_type = fpu_get_type(model, cpu_manufacturer, cpu, "none");
        /* The same default and minimum loadconfig() applies; AT models count min_ram in MB. */
        bool in_mb = (models[model]->flags & MODEL_AT) && models[model]->ram_granularity < 128;
        int min_kb = in_mb ? models[model]->min_ram * 1024 : models[model]->min_ram;
        mem_size = 4096 < min_kb ? min_kb : 4096;
}

void ManagerFrame::OnNew(wxCommandEvent &event) {
        wxString name;
        NewMachineMode mode = AskNewMachine(name);
        if (mode == NEW_CANCEL)
                return;

        wxString cfg = ConfigPath(name);
        if (mode == NEW_BY_BOARD) {
                TrwSelection selection;
                if (!trw_choose_motherboard(this, selection)) {
                        UpdateDetails(true);
                        return;
                }
                load_default_config(cfg);
                set_model_defaults(selection.model);
                custom_bios_set(selection.bios_path.empty() ? NULL : (const char *)selection.bios_path.mb_str(),
                                models[model]->internal_name);
                trw_save_attribution(selection.board, models[model]->internal_name);
        } else
                load_default_config(cfg);

        has_been_inited = 0;
        if (ConfigDialog::Open(this)) {
                saveconfig((char *)(const char *)cfg.mb_str());
                RefreshMachines(true);
                SelectName(name);
        }
        UpdateDetails(true);
}

void ManagerFrame::OnSettings(wxCommandEvent &event) {
        int sel = SelectedIndex();
        if (sel < 0 || machines[sel].running || machines[sel].starting)
                return;

        wxString cfg = ConfigPath(machines[sel].name);
        loadconfig((char *)(const char *)cfg.mb_str());
        has_been_inited = 0;
        if (ConfigDialog::Open(this))
                saveconfig((char *)(const char *)cfg.mb_str());
        UpdateDetails(true);
}

void ManagerFrame::OnStart(wxCommandEvent &event) {
        int sel = SelectedIndex();
        if (sel < 0)
                return;
        Machine &m = machines[sel];
        if (m.running || m.starting)
                return;

        wxString exe = wxStandardPaths::Get().GetExecutablePath();
        wxString cfg = ConfigPath(m.name);
        wxWCharBuffer exe_w = exe.wc_str(), cfg_w = cfg.wc_str();
        const wchar_t *argv[] = {exe_w.data(), L"--config", cfg_w.data(), NULL};
        long pid = wxExecute(argv, wxEXEC_ASYNC);
        if (pid <= 0) {
                wx_messagebox(this, wxString::Format("Could not start \"%s\".", m.name), "MetaPC", wxOK | wxICON_ERROR);
                return;
        }
        pclog("Launched machine %s (pid %ld)\n", (const char *)m.name.mb_str(), pid);
        launch_times[m.name] = wxGetLocalTimeMillis();
        launch_pids[m.name] = pid;
        m.starting = true;
        list->RefreshRow(sel);
        UpdateCommands();
}

/* True if pid is a running process of this same program, so a stale pid file (whose number
   Windows may have given to another process since) never makes us kill something else. */
static bool is_our_process(long pid) {
        if (pid <= 0 || (unsigned long)pid == wxGetProcessId())
                return false;
#ifdef _WIN32
        HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
        if (!h)
                return false;
        wchar_t path[MAX_PATH * 2];
        DWORD len = MAX_PATH * 2;
        bool ok = QueryFullProcessImageNameW(h, 0, path, &len) != 0;
        DWORD code = 0;
        bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
        CloseHandle(h);
        if (!ok || !alive)
                return false;
        wxFileName running(wxString(path, len)), self(wxStandardPaths::Get().GetExecutablePath());
        return running.GetFullName().IsSameAs(self.GetFullName(), false);
#else
        return wxProcess::Exists(pid);
#endif
}

#ifdef _WIN32
/* A machine that runs with administrator rights (it uses a CompactFlash card) can only be
   stopped with them too: asks for them and stops it with taskkill. */
static bool kill_elevated(long pid) {
        wxString params = wxString::Format("/F /PID %ld", pid);
        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = L"taskkill.exe";
        sei.lpParameters = params.wc_str();
        sei.nShow = SW_HIDE;
        if (!ShellExecuteExW(&sei))
                return false;
        if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, 10000);
                CloseHandle(sei.hProcess);
        }
        return !is_our_process(pid);
}
#endif

/* The process id of a running (or starting) machine: the one it recorded itself, or else the
   one this manager launched. 0 if none is known. */
long ManagerFrame::MachinePid(const wxString &name) {
        wxTextFile file;
        wxString pid_file = wx_machine_pid_file(ConfigPath(name));
        if (wxFileExists(pid_file) && file.Open(pid_file) && file.GetLineCount() > 0) {
                long pid = 0;
                if (file.GetFirstLine().Trim().ToLong(&pid) && is_our_process(pid))
                        return pid;
        }
        auto it = launch_pids.find(name);
        if (it != launch_pids.end() && is_our_process(it->second))
                return it->second;
        return 0;
}

/* Kills the selected machine's process, for a machine that hangs or does not respond. A
   responsive machine should be shut down from its own window, which saves its state. */
void ManagerFrame::OnStop(wxCommandEvent &event) {
        int sel = SelectedIndex();
        if (sel < 0)
                return;
        Machine &m = machines[sel];
        if (!m.running && !m.starting)
                return;
        wxString name = m.name;

        long pid = MachinePid(name);
        if (!pid) {
                wx_messagebox(this,
                              wxString::Format("Could not find the process of \"%s\". If it is still running, close it "
                                               "from its own window or from the Task Manager.",
                                               name),
                              "MetaPC", wxOK | wxICON_ERROR);
                return;
        }

        wxMessageDialog dlg(this,
                            wxString::Format("Force \"%s\" to stop?\n\n"
                                             "Its process (pid %ld) will be terminated immediately, as if the computer "
                                             "lost power. Anything not saved inside the emulated machine is lost, the "
                                             "CMOS settings and the last window layout are not saved, and its disk "
                                             "images may be left inconsistent.\n\n"
                                             "Use this only when the machine does not respond. Otherwise shut it down "
                                             "from its own window.",
                                             name, pid),
                            "Force stop", wxYES_NO | wxNO_DEFAULT | wxICON_WARNING);
        dlg.SetYesNoLabels("&Force stop", "&Cancel");
        if (dlg.ShowModal() != wxID_YES)
                return;

        /* It may have exited while the question was open. */
        if (!is_our_process(pid)) {
                RefreshMachines(true);
                return;
        }
        pclog("Force stopping machine %s (pid %ld)\n", (const char *)name.mb_str(), pid);
        bool killed = wxKill(pid, wxSIGKILL, NULL, wxKILL_CHILDREN) == wxKILL_OK || !is_our_process(pid);
#ifdef _WIN32
        if (!killed)
                killed = kill_elevated(pid);
#endif
        if (!killed) {
                wx_messagebox(this, wxString::Format("Could not stop \"%s\".", name), "MetaPC", wxOK | wxICON_ERROR);
                return;
        }
        wxRemoveFile(wx_machine_pid_file(ConfigPath(name)));
        launch_times.erase(name);
        launch_pids.erase(name);
        /* The lock goes away with the process; give the system a moment to release it. */
        for (int i = 0; i < 20 && IsRunning(name); i++)
                wxMilliSleep(50);
        RefreshMachines(true);
}

void ManagerFrame::OnCopy(wxCommandEvent &event) {
        wxString old_name = SelectedName();
        if (old_name.empty())
                return;
        wxString name = old_name;
        if (!AskName("Clone machine", name))
                return;
        wxCopyFile(ConfigPath(old_name), ConfigPath(name), true);
        RefreshMachines(true);
        SelectName(name);
}

void ManagerFrame::OnRename(wxCommandEvent &event) {
        int sel = SelectedIndex();
        if (sel < 0 || machines[sel].running || machines[sel].starting)
                return;
        wxString old_name = machines[sel].name;
        wxString name = old_name;
        if (!AskName("Rename machine", name))
                return;
        wxRenameFile(ConfigPath(old_name), ConfigPath(name), false);
        RefreshMachines(true);
        SelectName(name);
}

void ManagerFrame::OnDelete(wxCommandEvent &event) {
        int sel = SelectedIndex();
        if (sel < 0 || machines[sel].running || machines[sel].starting)
                return;
        wxString name = machines[sel].name;
        if (wx_messagebox(this, wxString::Format("Do you want to delete \"%s\"?", name), "MetaPC", wxOK | wxCANCEL) == wxOK) {
                wxRemoveFile(ConfigPath(name));
                RefreshMachines(true);
        }
}

void ManagerFrame::OnOpenFolder(wxCommandEvent &event) { wxLaunchDefaultApplication(wxString(configs_path)); }

void ManagerFrame::OnHostConfig(wxCommandEvent &event) {
#ifdef USE_NETWORKING
        HostConfigDialog::Open(this);
#endif
}

void ManagerFrame::OnAbout(wxCommandEvent &event) {
        wxDialog dlg;
        wxXmlResource::Get()->LoadDialog(&dlg, this, "AboutDlg");
        wxString version = dlg.FindWindow("VERSION_TEXT")->GetLabel();
        version.Replace("PCEM-VERSION-STRING", PCEM_VERSION_STRING);
        dlg.FindWindow("VERSION_TEXT")->SetLabel(version);
        dlg.Fit();
        dlg.ShowModal();
}

void ManagerFrame::OnContextMenu(wxContextMenuEvent &event) {
        wxPoint pos = event.GetPosition();
        if (pos != wxDefaultPosition) {
                wxPoint client = list->ScreenToClient(pos);
                int item = list->VirtualHitTest(client.y);
                if (item != wxNOT_FOUND) {
                        list->SetSelection(item);
                        UpdateCommands();
                        UpdateDetails(false);
                }
        }

        wxMenu menu;
        int sel = SelectedIndex();
        if (sel >= 0) {
                menu.Append(ID_MGR_START, "S&tart");
                menu.Append(ID_MGR_STOP, "&Force stop...");
                menu.Append(ID_MGR_SETTINGS, "&Settings...");
                menu.AppendSeparator();
                menu.Append(ID_MGR_COPY, "&Clone...");
                menu.Append(ID_MGR_RENAME, "&Rename...");
                menu.Append(ID_MGR_DELETE, "&Delete");
                bool busy = machines[sel].running || machines[sel].starting;
                menu.Enable(ID_MGR_START, !busy);
                menu.Enable(ID_MGR_STOP, busy);
                menu.Enable(ID_MGR_SETTINGS, !busy);
                menu.Enable(ID_MGR_RENAME, !busy);
                menu.Enable(ID_MGR_DELETE, !busy);
                menu.AppendSeparator();
        }
        menu.Append(ID_MGR_NEW, "&New...");
        PopupMenu(&menu);
}

/* ---------------------------------------------------------------- persistence */

static const char *MGR_SECTION = "Machine Manager";

void ManagerFrame::LoadGeometry() {
        int w = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "width", 0);
        int h = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "height", 0);
        int x = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "x", INT_MIN);
        int y = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "y", INT_MIN);
        int sash = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "sash", 0);
        int maximized = wx_config_get_int(CFG_GLOBAL, MGR_SECTION, "maximized", 0);
        wxString selected = wx_config_get_string(CFG_GLOBAL, MGR_SECTION, "selected", "");

        if (w > 200 && h > 150)
                SetSize(w, h);
        else
                SetSize(FromDIP(wxSize(860, 560)));
        if (x != INT_MIN && y != INT_MIN && wxDisplay::GetFromPoint(wxPoint(x + 40, y + 20)) != wxNOT_FOUND)
                SetPosition(wxPoint(x, y));
        else
                CenterOnScreen();
        if (sash > 0)
                splitter->SetSashPosition(sash);
        if (maximized)
                Maximize();

        RefreshMachines(true);
        if (!selected.empty())
                SelectName(selected);
}

void ManagerFrame::SaveGeometry() {
        char global_config_file[512];
        append_filename(global_config_file, pcem_path, (char *)"pcem.cfg", 511);
        /* Other PCem processes write this file too: start from what is on disk now. */
        config_load(CFG_GLOBAL, global_config_file);

        bool maximized = IsMaximized();
        if (!maximized && !IsIconized()) {
                wxRect r = GetRect();
                wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "x", r.x);
                wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "y", r.y);
                wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "width", r.width);
                wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "height", r.height);
        }
        wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "maximized", maximized);
        wx_config_set_int(CFG_GLOBAL, MGR_SECTION, "sash", splitter->GetSashPosition());
        wx_config_set_string(CFG_GLOBAL, MGR_SECTION, "selected", SelectedName().mb_str());
        saveconfig_global_only();
}

void ManagerFrame::OnClose(wxCloseEvent &event) {
        timer.Stop();
        SaveGeometry();
        Destroy();
        app_frame->ManagerClosed();
}
