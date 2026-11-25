#ifndef _PCEM_WX_DISPLAY_H_
#define _PCEM_WX_DISPLAY_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <wx/aui/framemanager.h>
#include <vector>

#include "sdl2-host.h"
#include "wx-panels.h"

class DriveStatusPanel;
class MonitorPanel;

/* The machine window: the emulated display, shown as a monitor in the centre, surrounded by
   dockable panels (machine information, controls, drives) that can be moved, floated or
   hidden. The layout is saved per machine. */
class DisplayFrame : public wxFrame, public PanelHost {
    public:
        DisplayFrame(wxWindow *owner, wxMenuBar *menubar, const char *title, SDL_DISPLAY_CALLBACK callback);
        ~DisplayFrame();

        static void *HostNativeCanvas(void *frame);
        static void HostCanvasSize(void *frame, int *width, int *height);
        static void HostSetSize(void *frame, int width, int height, int resizable);
        static void HostGetRect(void *frame, int *x, int *y, int *width, int *height);
        static void HostSetPosition(void *frame, int x, int y);
        static void HostCenter(void *frame);
        static void HostShow(void *frame, int show);
        static void HostSetFullscreen(void *frame, int fullscreen);
        static void HostSetTitle(void *frame, const char *title);
        static void HostSetStatus(void *frame, const char *message, int speed, int fps);

        void RunCommand(int id) override;
        void FocusDisplay() override;
        wxWindow *HostWindow() override { return this; }

    private:
        SDL_DISPLAY_CALLBACK callback;
        wxWindow *owner;
        wxAuiManager aui;
        MonitorPanel *monitor;
        wxPanel *canvas;
        DriveStatusPanel *drives;
        std::vector<MachinePanel *> panels;
        wxString panels_signature;
        wxMenu *panels_menu;
        wxTimer panel_timer;
        int panel_ticks;
        bool in_fullscreen;
        wxString fullscreen_layout;
        wxString default_layout;
        int last_speed, last_fps;
        /* The screen size last asked for, kept while panes come and go. */
        wxSize last_size;
        bool last_resizable;

        void *NativeCanvas();
        void SetDisplaySize(int width, int height, bool resizable);
        void ShowDisplay(bool show);
        void SetDisplayFullscreen(bool fullscreen);
        void SetStatus(const char *message, int speed, int fps);
        void SetStatusField(int field, const wxString &text);
        void LayoutStatus();
        int TopSafeInset();

        void CreatePanels(const wxString &layout);
        void AddPanelPane(MachinePanel *panel, int *right_pos, int *bottom_pos);
        void ApplyLayout(const wxString &layout);
        void ResetLayout();
        void UpdatePanels();
        void BuildPanelsMenu();
        void UpdatePanelsMenu();
        bool HandlePanelsMenu(int id);

        static DisplayFrame *current;
        static void SaveWindowConfig();
        wxString CurrentLayout();
};

#endif /* _PCEM_WX_DISPLAY_H_ */
