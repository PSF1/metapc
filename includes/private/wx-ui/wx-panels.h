#ifndef _WX_PANELS_H_
#define _WX_PANELS_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <functional>
#include <vector>

extern "C" {
#include "sdl2-common.h"
}

#define PANEL_MAX_DRIVES 16

/* Machine state gathered once per refresh and handed to every panel (reading the drive activity
   flags clears them, so they must be read in one place). */
struct MachineStatus {
        drive_info_t drives[PANEL_MAX_DRIVES];
        int num_drives;
        int state; /* emulation_state_t */
        int speed; /* % of real time */
        int fps;
        const drive_info_t *Find(drive_type_t type, int drive) const;
};

/* What the machine window hosts the panels in: the video output and the emulator commands. */
class PanelHost {
    public:
        virtual ~PanelHost() {}
        /* Runs a menu command (an XRC IDM_* id) as if chosen from the machine's menu. */
        virtual void RunCommand(int id) = 0;
        /* Gives keyboard focus back to the emulated display. */
        virtual void FocusDisplay() = 0;
        virtual wxWindow *HostWindow() = 0;
};

class MachinePanel : public wxPanel {
    public:
        MachinePanel(wxWindow *parent, PanelHost *host, const wxString &name, const wxString &caption);
        const wxString &PaneName() const { return name; }
        const wxString &Caption() const { return caption; }
        virtual void UpdateStatus(const MachineStatus &status) {}
        /* Where the pane docks in the default layout. */
        virtual bool DefaultRight() const { return false; }

    protected:
        PanelHost *host;
        wxString name;
        wxString caption;

        void Command(int id);
        wxButton *MakeButton(const wxString &label, const char *bitmap, const wxString &tooltip,
                             std::function<void()> action);
};

class ActivityLed : public wxWindow {
    public:
        ActivityLed(wxWindow *parent);
        void SetState(bool present, bool active);

    private:
        bool present, active;
        void OnPaint(wxPaintEvent &event);
};

/* One entry per pane the current configuration has, in default layout order. */
std::vector<MachinePanel *> wx_create_panels(wxWindow *parent, PanelHost *host);
/* Changes when the configuration's set of panes does (e.g. a floppy drive was added). */
wxString wx_panels_signature();

/* Recently used media images, kept in the global config. kind: "floppy", "cdrom" or "zip". */
void wx_recent_add(const char *kind, const wxString &path);
wxArrayString wx_recent_get(const char *kind);

#endif /* _WX_PANELS_H_ */
