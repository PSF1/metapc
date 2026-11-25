#ifndef _WX_MANAGER_H_
#define _WX_MANAGER_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/splitter.h>
#include <wx/vlbox.h>
#include <map>
#include <vector>

class Frame;
class MachineListBox;

/* The machine manager: lists the configured machines, shows the selected one's hardware and
   launches each machine in its own PCem process (the emulator core runs one machine per
   process), staying open so several machines can run side by side. */
class ManagerFrame : public wxFrame {
    public:
        ManagerFrame(Frame *app_frame);
        ~ManagerFrame();

        struct Machine {
                wxString name;
                bool running;
                bool starting;
        };

    private:
        Frame *app_frame;
        wxSplitterWindow *splitter;
        MachineListBox *list;
        wxScrolledWindow *details;
        wxTimer timer;
        std::vector<Machine> machines;
        std::map<wxString, wxLongLong> launch_times;
        std::map<wxString, long> launch_pids;
        wxString details_shown;
        bool details_running;

        wxString ConfigPath(const wxString &name);
        wxString SelectedName();
        int SelectedIndex();
        void SelectName(const wxString &name);

        void BuildMenu();
        void BuildToolBar();
        void RefreshMachines(bool force_details = false);
        bool IsRunning(const wxString &name);
        void UpdateCommands();
        void UpdateDetails(bool force);
        bool AskName(const wxString &title, wxString &name);

        void OnNew(wxCommandEvent &event);
        void OnSettings(wxCommandEvent &event);
        void OnStart(wxCommandEvent &event);
        void OnStop(wxCommandEvent &event);
        long MachinePid(const wxString &name);
        void OnCopy(wxCommandEvent &event);
        void OnRename(wxCommandEvent &event);
        void OnDelete(wxCommandEvent &event);
        void OnOpenFolder(wxCommandEvent &event);
        void OnHostConfig(wxCommandEvent &event);
        void OnAbout(wxCommandEvent &event);
        void OnContextMenu(wxContextMenuEvent &event);
        void OnClose(wxCloseEvent &event);

        void LoadGeometry();
        void SaveGeometry();

        friend class MachineListBox;
};

#endif /* _WX_MANAGER_H_ */
