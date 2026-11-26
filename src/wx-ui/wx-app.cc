#include "wx-app.h"

#include <wx/xrc/xmlres.h>
#include <wx/event.h>
#include "wx-utils.h"
#include "wx-common.h"
#include "wx-frontend.h"
#include "wx-status.h"
#include "wx-config.h"
#include "wx-manager.h"
#include "wx-machineinfo.h"
#include "logging-internal.h"

#include <wx/snglinst.h>
#include <wx/file.h>
#include <wx/stdpaths.h>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

extern "C" {
#include "sdl2-emu.h"
#include "ide.h"
#include "hdd_hostdisk.h"
#include "paths.h"
#include "ibm.h"
#include "config.h"
}

extern int config_override;

/* Held for the life of a machine process, so the manager (and other PCem processes) can tell
   this machine is running. */
static wxSingleInstanceChecker *machine_lock = NULL;
static wxString machine_pid_file;

extern void InitXmlResource();

wxDEFINE_EVENT(WX_CALLBACK_EVENT, CallbackEvent);
wxDEFINE_EVENT(WX_EXIT_EVENT, wxCommandEvent);
wxDEFINE_EVENT(WX_STOP_EMULATION_EVENT, wxCommandEvent);
wxDEFINE_EVENT(WX_STOP_EMULATION_NOW_EVENT, wxCommandEvent);
wxDEFINE_EVENT(WX_EXIT_COMPLETE_EVENT, wxCommandEvent);
wxDEFINE_EVENT(WX_SHOW_WINDOW_EVENT, wxCommandEvent);

wxBEGIN_EVENT_TABLE(Frame, wxFrame) wxEND_EVENT_TABLE()

        wxIMPLEMENT_APP_NO_MAIN(App);

App::App() { this->frame = NULL; }

int App::OnExit() {
        if (!machine_pid_file.empty())
                wxRemoveFile(machine_pid_file);
        delete machine_lock;
        machine_lock = NULL;
        return wxApp::OnExit();
}

bool App::OnInit() {
        SetAppDisplayName("MetaPC");
        wxImage::AddHandler(new wxPNGHandler);
        wxXmlResource::Get()->InitAllHandlers();
        InitXmlResource();

        frame = new Frame(this, "null frame", wxPoint(500, 500), wxSize(100, 100));
        frame->Start();
        return true;
}

int App::OnRun() { return wxApp::OnRun(); }

#include <sstream>

Frame::Frame(App *app, const wxString &title, const wxPoint &pos, const wxSize &size)
        : wxFrame(NULL, wxID_ANY, title, pos, size, 0) // wxDEFAULT_FRAME_STYLE & ~(wxRESIZE_BORDER))
{
        this->closing = false;
        this->menu = wxXmlResource::Get()->LoadMenuBar(wxT("main_menu"));

        Bind(wxEVT_CLOSE_WINDOW, &Frame::OnClose, this);
        Bind(wxEVT_MENU, &Frame::OnCommand, this);
        Bind(wxEVT_TOOL, &Frame::OnCommand, this);
        Bind(WX_SHOW_WINDOW_EVENT, &Frame::OnShowWindowEvent, this);
        Bind(WX_EXIT_EVENT, &Frame::OnExitEvent, this);
        Bind(WX_EXIT_COMPLETE_EVENT, &Frame::OnExitCompleteEvent, this);
        Bind(WX_STOP_EMULATION_EVENT, &Frame::OnStopEmulationEvent, this);
        Bind(WX_STOP_EMULATION_NOW_EVENT, &Frame::OnStopEmulationNowEvent, this);
        Bind(WX_CALLBACK_EVENT, &Frame::OnCallbackEvent, this);

        wx_set_app_icon(this);
        CenterOnScreen();
}

void wx_set_app_icon(wxTopLevelWindow *win) {
#ifdef __WXMSW__
        static wxIconBundle icons(wxT("MAIN_ICO"), 0);
        if (icons.IsOk())
                win->SetIcons(icons);
#else
        (void)win;
#endif
}

/* Takes the running-machine lock for config_file_default. The manager probes the lock every
   second, which briefly holds it, so retry before deciding another process has the machine. */
/* Records this process id, so the manager can force the machine to stop if it hangs. */
static void write_pid_file() {
        machine_pid_file = wx_machine_pid_file(wxString(config_file_default));
        wxFile f;
        if (f.Create(machine_pid_file, true))
                f.Write(wxString::Format("%lu\n", wxGetProcessId()));
}

static bool lock_machine() {
        wxString name = wx_machine_instance_name(wxString(config_file_default));
        for (int tries = 0; tries < 10; tries++) {
                machine_lock = new wxSingleInstanceChecker();
                if (!machine_lock->Create(name) || !machine_lock->IsAnotherRunning()) {
                        write_pid_file();
                        return true;
                }
                delete machine_lock;
                machine_lock = NULL;
                wxMilliSleep(50);
        }
        return false;
}

#ifdef _WIN32
/* Using a disk of the host directly (a CompactFlash card) needs administrator rights on Windows.
   Rather than having the whole program run as administrator, a machine that uses one asks for
   them when it starts: it starts itself again elevated, which shows the Windows permission
   prompt. */
static bool machine_uses_host_disk() {
        for (int i = 0; i < 7; i++)
                if (i != cdrom_channel && i != zip_channel && hostdisk_is_device_path(ide_fn[i]))
                        return true;
        return false;
}

static bool process_is_elevated() {
        HANDLE token;
        TOKEN_ELEVATION elevation;
        DWORD size;
        bool elevated = false;

        if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size))
                        elevated = elevation.TokenIsElevated != 0;
                CloseHandle(token);
        }
        return elevated;
}

/* Quotes one argument for the Windows command line (CommandLineToArgvW rules). */
static wxString quote_arg(const wxString &arg) {
        const wxUniChar backslash('\\');
        if (!arg.empty() && arg.find_first_of(" \t\"") == wxString::npos)
                return arg;
        wxString out = "\"";
        int backslashes = 0;
        for (wxString::const_iterator it = arg.begin(); it != arg.end(); ++it) {
                if (*it == backslash) {
                        backslashes++;
                        continue;
                }
                if (*it == '"')
                        out += wxString(backslash, 2 * backslashes + 1) + "\"";
                else
                        out += wxString(backslash, backslashes) + *it;
                backslashes = 0;
        }
        out += wxString(backslash, 2 * backslashes) + "\"";
        return out;
}

/* Starts this machine again with administrator rights. False if the user declined. */
static bool relaunch_elevated() {
        wxString params;
        for (int i = 1; i < wxTheApp->argc; i++)
                params += (i > 1 ? " " : "") + quote_arg(wxTheApp->argv[i]);
        /* If the user is not an administrator, Windows asks for another account's password and
           the machine runs as that account, whose home folder is different: keep using this
           user's data folder (ROMs, settings, CMOS), and say who asked so problems can be
           explained. */
        params += " --data-dir " + quote_arg(wxString(pcem_path));
        params += " --elevated-by " + quote_arg(wxGetUserId());
        wxString exe = wxStandardPaths::Get().GetExecutablePath();
        wxString cwd = wxGetCwd();

        SHELLEXECUTEINFOW sei;
        memset(&sei, 0, sizeof(sei));
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_NOASYNC;
        sei.lpVerb = L"runas";
        sei.lpFile = exe.wc_str();
        sei.lpParameters = params.wc_str();
        sei.lpDirectory = cwd.wc_str();
        sei.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&sei))
                return true;
        pclog("UI: elevation failed or declined (error %lu)\n", GetLastError());
        return false;
}
#endif

void Frame::Start() {
        pclog("UI: starting\n");
        if (!wx_start(this)) {
                pclog("UI: start failed\n");
                Quit(0);
                return;
        }
        if (!config_override) {
                pclog("UI: opening the machine manager\n");
                /* No machine given: this process is the machine manager. Each machine it
                   starts runs in its own process (pcem --config <file>). */
                (new ManagerFrame(this))->Show();
                return;
        }
#ifdef _WIN32
        if (machine_uses_host_disk() && !process_is_elevated()) {
                pclog("UI: the machine uses a disk of the host, asking for administrator rights\n");
                if (relaunch_elevated()) {
                        Quit(0);
                        return;
                }
                /* Declined: start anyway; the hard disk reports that it cannot open the card. */
        }
#endif
        if (!lock_machine()) {
                pclog("UI: the machine is already running in another process\n");
                wx_messagebox(NULL, wxString::Format("The machine \"%s\" is already running.", config_name), "MetaPC",
                              wxOK | wxICON_INFORMATION);
                Quit(1);
                return;
        }
        has_been_inited = 1;
        pclog("UI: starting the emulation\n");
        start_emulation(this);
}

void Frame::ManagerClosed() { Quit(1); }

void Frame::OnCallbackEvent(CallbackEvent &event) {
        WX_CALLBACK callback = event.GetCallback();
        callback(event.GetData());
}

void Frame::OnStopEmulationEvent(wxCommandEvent &event) {
        if (emulation_state != EMULATION_STOPPED) {
                pause_emulation();
                int ret = wxID_OK;
                pclog_flush();
                if (confirm_on_stop_emulation) {
                        wxDialog dlg;
                        wxXmlResource::Get()->LoadDialog(&dlg, this, "ConfirmRememberDlg");
                        dlg.FindWindow("IDC_CONFIRM_LABEL")->SetLabel("Stop emulation?");
                        dlg.FindWindow("IDC_CONFIRM_REMEMBER")->SetLabel("Do not ask again");
                        dlg.Fit();
                        ret = dlg.ShowModal();
                        if (ret == wxID_OK)
                                confirm_on_stop_emulation = !((wxCheckBox *)dlg.FindWindow("IDC_CONFIRM_REMEMBER"))->IsChecked();
                }

                if (ret == wxID_OK) {
                        stop_emulation();
                        Quit(1);
                } else
                        resume_emulation();
        }
}

void Frame::OnStopEmulationNowEvent(wxCommandEvent &event) {
        stop_emulation();
        pclog_flush();
        Quit(1);
}

void Frame::OnShowWindowEvent(wxCommandEvent &event) {
        wxWindow *window = (wxWindow *)event.GetEventObject();
        int shown = window->IsShown();
        int value = event.GetInt();
        if (value < 0)
                window->Show(!shown);
        else
                window->Show(value > 0);
        if (!shown)
                window->Refresh();
}

void Frame::OnCommand(wxCommandEvent &event) { wx_handle_command(this, event.GetId(), event.IsChecked()); }

void Frame::OnClose(wxCloseEvent &event) {
        pclog_end();
        QueueCommand(WX_EXIT_EVENT, 0);
}

wxMenuBar *Frame::GetMenu() { return menu; }

void Frame::QueueCommand(wxEventType type, int value) {
        wxCommandEvent *event = new wxCommandEvent(type, wxID_ANY);
        event->SetEventObject(this);
        event->SetInt(value);
        wxQueueEvent(this, event);
}

void Frame::QueueCallback(WX_CALLBACK callback, void *data) { wxQueueEvent(this, new CallbackEvent(callback, data)); }

void Frame::StopEmulation() { QueueCommand(WX_STOP_EMULATION_EVENT, 0); }

void Frame::StopEmulationNow() { QueueCommand(WX_STOP_EMULATION_NOW_EVENT, 0); }

void Frame::ToggleVisible() { QueueCommand(WX_SHOW_WINDOW_EVENT, -1); }

void Frame::ShowStatus() {
        if (!FindWindowById(STATUS_WINDOW_ID))
                (new StatusFrame(this))->Show();
}

void Frame::CloseStatus() {
        wxWindow *status = FindWindowById(STATUS_WINDOW_ID);
        if (status)
                status->Close();
}

/* Exit */

void Frame::Quit(bool stop_emulator) {
        if (closing)
                return;
        closing = true;
        if (stop_emulator) {
                if (!wx_stop(this)) {
                        // cancel quit
                        closing = false;
                        return;
                }
        }
        pclog_end();
        Destroy();
}

void Frame::OnExitEvent(wxCommandEvent &event) {
        if (closing)
                return;
        closing = true;
        CExitThread *exitThread = new CExitThread(this);
        exitThread->Run();
}

void Frame::OnExitCompleteEvent(wxCommandEvent &event) {
        if (event.GetInt()) {
                pclog_end();
                Destroy();
        } else
                closing = false;
}

CExitThread::CExitThread(Frame *frame) { this->frame = frame; }

wxThread::ExitCode CExitThread::Entry() {
        wxCommandEvent *event = new wxCommandEvent(WX_EXIT_COMPLETE_EVENT, wxID_ANY);
        event->SetInt(wx_stop(frame));
        wxQueueEvent(frame, event);
        return 0;
}

