#include "wx-display.h"
#include "wx-utils.h"
#include "wx-app.h"
#include "wx-common.h"
#include "wx-frontend.h"

#include <wx/dcbuffer.h>
#include <wx/display.h>
#include <wx/graphics.h>

#include <algorithm>
#include <climits>
#include <string>

extern "C" {
#include "ibm.h"
#include "config.h"
#include "sdl2-display.h"
}

#ifdef __WXOSX__
#include <math.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#ifdef __WXGTK__
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#endif

#define DISPLAY_DRIVES_NAME "pcem_display_drives"
#define DISPLAY_DRIVE_ICON_SPACING 4
#define DISPLAY_MAX_DRIVES 10

extern "C" drive_info_t *get_machine_info(char *, int *);

class DriveStatusPanel : public wxPanel {
    public:
        DriveStatusPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE, DISPLAY_DRIVES_NAME) {
                count = 0;
                hover = -1;
                memset(drives, 0, sizeof(drives));
                memset(flash, 0, sizeof(flash));
                bitmapFDD[0] = wxXmlResource::Get()->LoadBitmap("BITMAP_FLOPPY");
                bitmapFDD[1] = bitmapFDD[0].ConvertToDisabled();
                bitmapHDD[0] = wxXmlResource::Get()->LoadBitmap("BITMAP_HARDDISK");
                bitmapHDD[1] = bitmapHDD[0].ConvertToDisabled();
                bitmapCDROM[0] = wxXmlResource::Get()->LoadBitmap("BITMAP_CDROM");
                bitmapCDROM[1] = bitmapCDROM[0].ConvertToDisabled();
                SetBackgroundStyle(wxBG_STYLE_PAINT);
                Bind(wxEVT_PAINT, &DriveStatusPanel::OnPaint, this);
                Bind(wxEVT_LEFT_UP, &DriveStatusPanel::OnClick, this);
                Bind(wxEVT_RIGHT_UP, &DriveStatusPanel::OnClick, this);
                Bind(wxEVT_MOTION, &DriveStatusPanel::OnMotion, this);
                Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) { hover = -1; });
        }

        int GetRequiredWidth() { return count * (IconSize().x + DISPLAY_DRIVE_ICON_SPACING) + DISPLAY_DRIVE_ICON_SPACING; }

        bool UpdateDrives(const drive_info_t *info, int num) {
                bool count_changed = num != count;
                bool changed = count_changed;

                if (num > DISPLAY_MAX_DRIVES)
                        num = DISPLAY_MAX_DRIVES;
                for (int i = 0; i < num; i++) {
                        int new_flash = info[i].readflash ? 1 : 0;
                        if (i >= count || drives[i].type != info[i].type || drives[i].enabled != info[i].enabled ||
                            drives[i].drive_letter != info[i].drive_letter || strcmp(drives[i].fn, info[i].fn) ||
                            flash[i] != new_flash)
                                changed = true;
                        drives[i] = info[i];
                        flash[i] = new_flash;
                }
                count = num;
                if (changed) {
                        Refresh();
                        if (hover >= 0 && hover < count)
                                SetToolTip(DriveDescription(hover));
                }
                return count_changed;
        }

    private:
        drive_info_t drives[DISPLAY_MAX_DRIVES];
        int flash[DISPLAY_MAX_DRIVES];
        int count;
        int hover;
        wxBitmap bitmapFDD[2];
        wxBitmap bitmapCDROM[2];
        wxBitmap bitmapHDD[2];

        wxSize IconSize() { return bitmapFDD[0].IsOk() ? bitmapFDD[0].GetSize() : wxSize(16, 16); }

        bool IsZip(int i) { return drives[i].type == DRIVE_TYPE_FDD && drives[i].drive_letter >= 'C'; }

        int HitTest(int x) {
                int step = IconSize().x + DISPLAY_DRIVE_ICON_SPACING;
                if (x < DISPLAY_DRIVE_ICON_SPACING)
                        return -1;
                int i = (x - DISPLAY_DRIVE_ICON_SPACING) / step;
                return (i < count) ? i : -1;
        }

        wxString DriveDescription(int i) {
                wxString s;
                if (IsZip(i))
                        s = wxString::Format("ZIP (%c:)", drives[i].drive_letter);
                else if (drives[i].type == DRIVE_TYPE_CDROM)
                        s = wxString::Format("CD-ROM (%c:)", drives[i].drive_letter);
                else if (drives[i].type == DRIVE_TYPE_HDD)
                        s = wxString::Format("Hard disk (%c:)", drives[i].drive_letter);
                else
                        s = wxString::Format("Floppy drive %c:", drives[i].drive_letter);
                if (strlen(drives[i].fn))
                        s += wxString(" - ") + wxString(drives[i].fn);
                else if (!drives[i].enabled)
                        s += " - empty";
                return s;
        }

        void OnPaint(wxPaintEvent &event) {
                wxPaintDC dc(this);
                wxSize size = GetClientSize();
                wxSize icon = IconSize();
                dc.SetBackground(wxBrush(GetParent()->GetBackgroundColour()));
                dc.Clear();
                dc.SetPen(*wxTRANSPARENT_PEN);
                for (int i = 0; i < count; i++) {
                        int x = DISPLAY_DRIVE_ICON_SPACING + i * (icon.x + DISPLAY_DRIVE_ICON_SPACING);
                        int y = (size.y - icon.y) / 2;
                        int disabled = drives[i].enabled ? 0 : 1;
                        wxBitmap *bitmap;
                        if (drives[i].type == DRIVE_TYPE_CDROM)
                                bitmap = &bitmapCDROM[disabled];
                        else if (drives[i].type == DRIVE_TYPE_HDD)
                                bitmap = &bitmapHDD[disabled];
                        else
                                bitmap = &bitmapFDD[disabled];
                        if (bitmap->IsOk())
                                dc.DrawBitmap(*bitmap, x, y, true);
                        if (show_disc_activity) {
                                dc.SetBrush(wxBrush(flash[i] ? wxColour(0, 255, 0) : wxColour(0, 100, 0)));
                                dc.DrawRectangle(x + icon.x - 5, y + icon.y - 5, 5, 5);
                        }
                }
        }

        void OnMotion(wxMouseEvent &event) {
                int i = HitTest(event.GetX());
                if (i != hover) {
                        hover = i;
                        if (i >= 0)
                                SetToolTip(DriveDescription(i));
                        else
                                UnsetToolTip();
                }
        }

        void AppendCDROMItems(wxMenu *popup) {
                wx_refresh_host_cdrom_menu();
                wxFrame *frame = wxDynamicCast(wxGetTopLevelParent(this), wxFrame);
                wxMenu *cdrom = (frame && frame->GetMenuBar()) ? wx_getsubmenu(frame->GetMenuBar(), XRCID("IDM_CDROM")) : NULL;
                if (!cdrom) {
                        popup->Append(XRCID("IDM_CDROM_IMAGE_LOAD"), "Load image...");
                        popup->Append(XRCID("IDM_CDROM_EMPTY"), "Eject");
                        return;
                }
                wxMenuItemList &items = cdrom->GetMenuItems();
                for (wxMenuItemList::iterator it = items.begin(); it != items.end(); ++it) {
                        wxMenuItem *item = *it;
                        if (item->IsSeparator())
                                popup->AppendSeparator();
                        else if (!item->IsSubMenu()) {
                                wxMenuItem *copy =
                                        popup->Append(item->GetId(), item->GetItemLabel(), wxEmptyString, item->GetKind());
                                if (item->IsCheckable())
                                        copy->Check(item->IsChecked());
                                copy->Enable(item->IsEnabled());
                        }
                }
        }

        void OnClick(wxMouseEvent &event) {
                int i = HitTest(event.GetX());
                if (i < 0)
                        return;

                wxMenu popup;
                if (IsZip(i)) {
                        popup.Append(XRCID("IDM_DISC_ZIP"), "Load ZIP disc...");
                        popup.Append(XRCID("IDM_EJECT_ZIP"), "Eject ZIP disc")->Enable(drives[i].enabled != 0);
                } else if (drives[i].type == DRIVE_TYPE_FDD) {
                        int b = drives[i].drive == 1;
                        char letter = drives[i].drive_letter;
                        popup.Append(XRCID(b ? "IDM_DISC_B" : "IDM_DISC_A"), wxString::Format("Change drive %c:...", letter));
                        popup.Append(XRCID(b ? "IDM_EJECT_B" : "IDM_EJECT_A"), wxString::Format("Eject drive %c:", letter))
                                ->Enable(drives[i].enabled != 0);
                        popup.AppendSeparator();
                        popup.Append(XRCID("IDM_DISC_CREATE"), "Create blank disc image...");
                } else if (drives[i].type == DRIVE_TYPE_CDROM)
                        AppendCDROMItems(&popup);
                else
                        popup.Append(wxID_ANY, DriveDescription(i))->Enable(false);

                PopupMenu(&popup, DISPLAY_DRIVE_ICON_SPACING + i * (IconSize().x + DISPLAY_DRIVE_ICON_SPACING), 0);
        }
};

/* ---------------------------------------------------------------- monitor */

/* Bezel geometry, in DIPs: the desk margin around the monitor, and the bezel around the screen. */
#define MONITOR_OUTER 10
#define MONITOR_SIDE 18
#define MONITOR_TOP 18
#define MONITOR_BOTTOM 34
/* The screen filter and CRT buttons on the chin. */
#define MONITOR_BUTTON_HEIGHT 16
static const double PI = 3.14159265358979;

enum {
        ID_FILTER_FIRST = wxID_HIGHEST + 6100,
        ID_FILTER_LAST = ID_FILTER_FIRST + VIDEO_FILTER_AMBER,
        ID_CRT_EFFECT,
};

enum { BUTTON_NONE = -1, BUTTON_FILTER, BUTTON_CRT };

struct ScreenFilter {
        const char *label; /* menu */
        const char *badge; /* on the button */
        wxColour swatch;
};

static const ScreenFilter screen_filters[] = {
        {"&Colour", "Colour", wxColour(0, 0, 0)},
        {"&Black and white", "B/W", wxColour(220, 220, 220)},
        {"&Green phosphor", "Green", wxColour(51, 255, 80)},
        {"&Amber (yellow) phosphor", "Amber", wxColour(255, 190, 0)},
};

static void append_filter_items(wxMenu *menu, int current) {
        for (int i = 0; i <= VIDEO_FILTER_AMBER; i++)
                menu->AppendRadioItem(ID_FILTER_FIRST + i, screen_filters[i].label);
        menu->Check(ID_FILTER_FIRST + current, true);
}

/* The centre pane: draws a monitor around the canvas SDL renders into. With a fixed-size
   display (the default "Original" resolution) the screen keeps its size and the monitor sits
   centred on the desk; with a resizable display the screen fills the pane. */
class MonitorPanel : public wxPanel {
    public:
        MonitorPanel(wxWindow *parent)
                : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE), bezel(true), fullscreen(false),
                  top_inset(0), screen_size(wxDefaultSize), power(EMULATION_STOPPED), filter(VIDEO_FILTER_COLOR),
                  crt(false), hover(BUTTON_NONE) {
                SetBackgroundStyle(wxBG_STYLE_PAINT);
                canvas = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxSize(640, 480), wxWANTS_CHARS);
                Bind(wxEVT_PAINT, &MonitorPanel::OnPaint, this);
                Bind(wxEVT_MOTION, [this](wxMouseEvent &event) { SetHover(HitButton(event.GetPosition())); });
                Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent &event) { SetHover(BUTTON_NONE); });
                Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent &event) {
                        int hit = HitButton(event.GetPosition());
                        if (hit == BUTTON_CRT)
                                SetCrt(!crt);
                        else if (hit == BUTTON_FILTER) {
                                wxRect button = ButtonRect(BUTTON_FILTER);
                                wxMenu popup;
                                append_filter_items(&popup, filter);
                                PopupMenu(&popup, button.GetLeft(), button.GetBottom() + 1);
                        }
                });
                Bind(
                        wxEVT_MENU, [this](wxCommandEvent &event) { SetFilter(event.GetId() - ID_FILTER_FIRST); }, ID_FILTER_FIRST,
                        ID_FILTER_LAST);
                Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
                        PlaceCanvas();
                        Refresh();
                });
        }

        wxPanel *Canvas() { return canvas; }
        bool HasBezel() const { return bezel; }

        void SetBezel(bool on) {
                bezel = on;
                PlaceCanvas();
                Refresh();
        }

        void SetFullscreen(bool on, int inset) {
                fullscreen = on;
                top_inset = inset;
                PlaceCanvas();
                Refresh();
        }

        void SetScreenSize(const wxSize &size) {
                screen_size = size;
                PlaceCanvas();
                Refresh();
        }

        /* Space the monitor needs around the screen. */
        wxSize ChromeSize() const {
                if (!ShowsBezel())
                        return wxSize(0, 0);
                return wxSize(2 * FromDIP(MONITOR_OUTER + MONITOR_SIDE),
                              2 * FromDIP(MONITOR_OUTER) + FromDIP(MONITOR_TOP) + FromDIP(MONITOR_BOTTOM));
        }

        int GetFilter() const { return filter; }

        void SetFilter(int f) {
                if (f < VIDEO_FILTER_COLOR || f > VIDEO_FILTER_AMBER)
                        f = VIDEO_FILTER_COLOR;
                filter = f;
                video_set_color_filter(f);
                UpdateToolTip();
                if (ShowsBezel())
                        Refresh();
        }

        bool GetCrt() const { return crt; }

        void SetCrt(bool on) {
                crt = on;
                video_set_crt_effect(on);
                UpdateToolTip();
                if (ShowsBezel())
                        Refresh();
        }

        void SetPowerState(int state) {
                if (state != power) {
                        power = state;
                        if (ShowsBezel())
                                Refresh();
                }
        }

    private:
        wxPanel *canvas;
        bool bezel;
        bool fullscreen;
        int top_inset;
        wxSize screen_size;
        int power;
        int filter;
        bool crt;
        int hover;

        bool ShowsBezel() const { return bezel && !fullscreen; }

        void UpdateToolTip() {
                if (hover == BUTTON_FILTER) {
                        wxString label = screen_filters[filter].label;
                        label.Replace("&", "");
                        SetToolTip("Screen filter: " + label);
                } else if (hover == BUTTON_CRT)
                        SetToolTip(crt ? "CRT effect: on" : "CRT effect: off");
                else
                        UnsetToolTip();
        }

        void SetHover(int button) {
                if (button == hover)
                        return;
                hover = button;
                SetCursor(button != BUTTON_NONE ? wxCursor(wxCURSOR_HAND) : wxNullCursor);
                UpdateToolTip();
                Refresh();
        }

        int HitButton(const wxPoint &pt) const {
                if (ButtonRect(BUTTON_FILTER).Contains(pt))
                        return BUTTON_FILTER;
                if (ButtonRect(BUTTON_CRT).Contains(pt))
                        return BUTTON_CRT;
                return BUTTON_NONE;
        }

        int ChinCentre(const wxRect &screen) const {
                return screen.GetBottom() + FromDIP(4) + (FromDIP(MONITOR_BOTTOM) - FromDIP(4)) / 2;
        }

        /* The buttons, at the left of the chin (the power LED is at the right): the screen
           filter, then the CRT effect. Each is a pill with a swatch or LED and a label. */
        wxRect ButtonRect(int button) const {
                if (!ShowsBezel())
                        return wxRect();
                wxRect screen = ScreenRect();
                int h = FromDIP(MONITOR_BUTTON_HEIGHT);
                int filter_w = FromDIP(6) + h / 2 + FromDIP(5) + GetTextExtent("Colour").x + FromDIP(8);
                wxRect r(screen.x, ChinCentre(screen) - h / 2, filter_w, h);
                if (button == BUTTON_CRT) {
                        r.x += filter_w + FromDIP(6);
                        r.width = FromDIP(6) + h / 2 + FromDIP(5) + GetTextExtent("CRT").x + FromDIP(8);
                }
                return r;
        }

        /* Draws a button's pill and label; returns the centre of its swatch. */
        wxPoint2DDouble DrawButton(wxGraphicsContext *gc, int button, const char *label) {
                wxRect r = ButtonRect(button);
                bool hot = hover == button;
                gc->SetPen(wxPen(hot ? wxColour(120, 120, 128) : wxColour(28, 28, 30)));
                gc->SetBrush(wxBrush(hot ? wxColour(70, 70, 76) : wxColour(48, 48, 52)));
                gc->DrawRoundedRectangle(r.x, r.y, r.width, r.height, r.height / 2.0);
                double sr = r.height / 4.0;
                wxPoint2DDouble c(r.x + FromDIP(6) + sr, r.y + r.height / 2.0);
                gc->SetFont(GetFont(), hot ? wxColour(225, 225, 230) : wxColour(170, 170, 176));
                double tw, th;
                gc->GetTextExtent(label, &tw, &th);
                gc->DrawText(label, c.m_x + sr + FromDIP(5), c.m_y - th / 2);
                return c;
        }

        wxRect ScreenRect() const {
                wxSize client = GetClientSize();
                if (fullscreen)
                        return wxRect(0, top_inset, client.x, std::max(0, client.y - top_inset));
                wxSize chrome = ChromeSize();
                wxSize avail(std::max(1, client.x - chrome.x), std::max(1, client.y - chrome.y));
                wxSize screen = avail;
                if (screen_size.x > 0 && screen_size.y > 0) {
                        screen.x = std::min(screen_size.x, avail.x);
                        screen.y = std::min(screen_size.y, avail.y);
                }
                int x = (client.x - (screen.x + chrome.x)) / 2;
                int y = (client.y - (screen.y + chrome.y)) / 2;
                if (ShowsBezel()) {
                        x += FromDIP(MONITOR_OUTER + MONITOR_SIDE);
                        y += FromDIP(MONITOR_OUTER + MONITOR_TOP);
                }
                return wxRect(std::max(0, x), std::max(0, y), screen.x, screen.y);
        }

        void PlaceCanvas() {
                wxRect r = ScreenRect();
                if (canvas->GetRect() != r)
                        canvas->SetSize(r);
        }

        void OnPaint(wxPaintEvent &event) {
                wxAutoBufferedPaintDC dc(this);
                if (!ShowsBezel()) {
                        dc.SetBackground(*wxBLACK_BRUSH);
                        dc.Clear();
                        return;
                }
                wxColour desk = wxSystemSettings::GetColour(wxSYS_COLOUR_APPWORKSPACE);
                dc.SetBackground(wxBrush(desk));
                dc.Clear();

                wxRect screen = ScreenRect();
                wxRect body(screen.x - FromDIP(MONITOR_SIDE), screen.y - FromDIP(MONITOR_TOP),
                            screen.width + 2 * FromDIP(MONITOR_SIDE), screen.height + FromDIP(MONITOR_TOP) + FromDIP(MONITOR_BOTTOM));
                double radius = FromDIP(10);

                wxGraphicsContext *gc = wxGraphicsContext::Create(dc);
                if (!gc)
                        return;
                /* drop shadow */
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->SetBrush(wxBrush(desk.ChangeLightness(80)));
                gc->DrawRoundedRectangle(body.x + FromDIP(2), body.y + FromDIP(3), body.width, body.height, radius);
                /* bezel */
                gc->SetBrush(gc->CreateLinearGradientBrush(0, body.y, 0, body.GetBottom(), wxColour(66, 66, 70), wxColour(36, 36, 39)));
                gc->SetPen(wxPen(wxColour(22, 22, 24)));
                gc->DrawRoundedRectangle(body.x, body.y, body.width - 1, body.height - 1, radius);
                gc->SetPen(wxPen(wxColour(90, 90, 96)));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRoundedRectangle(body.x + 1, body.y + 1, body.width - 3, body.height - 3, radius - 1);
                /* recessed screen surround */
                wxRect surround = screen;
                surround.Inflate(FromDIP(4));
                gc->SetPen(wxPen(wxColour(80, 80, 84)));
                gc->SetBrush(*wxBLACK_BRUSH);
                gc->DrawRoundedRectangle(surround.x, surround.y, surround.width, surround.height, FromDIP(3));

                /* badge and power LED on the chin */
                int chin_y = ChinCentre(screen);

                /* screen filter button: a swatch of the filter's colour and its name */
                double sr = FromDIP(MONITOR_BUTTON_HEIGHT) / 4.0;
                wxPoint2DDouble swatch = DrawButton(gc, BUTTON_FILTER, screen_filters[filter].badge);
                double sx = swatch.m_x, sy = swatch.m_y;
                if (filter == VIDEO_FILTER_COLOR) {
                        static const wxColour rgb[3] = {wxColour(230, 60, 60), wxColour(60, 200, 60), wxColour(70, 110, 240)};
                        gc->SetPen(*wxTRANSPARENT_PEN);
                        for (int i = 0; i < 3; i++) {
                                wxGraphicsPath path = gc->CreatePath();
                                path.MoveToPoint(sx, sy);
                                path.AddArc(sx, sy, sr, (-90 + 120 * i) * PI / 180, (30 + 120 * i) * PI / 180, true);
                                path.CloseSubpath();
                                gc->SetBrush(wxBrush(rgb[i]));
                                gc->FillPath(path);
                        }
                } else {
                        gc->SetPen(wxPen(screen_filters[filter].swatch.ChangeLightness(60)));
                        gc->SetBrush(wxBrush(screen_filters[filter].swatch));
                        gc->DrawEllipse(sx - sr, sy - sr, 2 * sr, 2 * sr);
                }

                /* CRT button: an LED that is lit while the effect is on */
                wxPoint2DDouble led_c = DrawButton(gc, BUTTON_CRT, "CRT");
                wxColour crt_led = crt ? wxColour(90, 200, 255) : wxColour(70, 70, 74);
                gc->SetPen(wxPen(crt_led.ChangeLightness(60)));
                gc->SetBrush(wxBrush(crt_led));
                gc->DrawEllipse(led_c.m_x - sr, led_c.m_y - sr, 2 * sr, 2 * sr);

                wxFont font = GetFont().Bold();
                gc->SetFont(font, wxColour(150, 150, 156));
                double tw, th;
                gc->GetTextExtent("MetaPC", &tw, &th);
                gc->DrawText("MetaPC", body.x + (body.width - tw) / 2, chin_y - th / 2);

                wxColour led = power == EMULATION_RUNNING ? wxColour(60, 230, 60)
                                                          : (power == EMULATION_PAUSED ? wxColour(240, 170, 20) : wxColour(70, 70, 70));
                double lr = FromDIP(3);
                double lx = body.GetRight() - FromDIP(MONITOR_SIDE) - lr;
                gc->SetPen(wxPen(led.ChangeLightness(60)));
                gc->SetBrush(wxBrush(led));
                gc->DrawEllipse(lx - lr, chin_y - lr, 2 * lr, 2 * lr);
                delete gc;
        }
};

/* ---------------------------------------------------------------- machine window */

static const char *WINDOW_SECTION = "Machine window";
/* Config lines are limited to 255 characters, so the layout is stored in pieces, each
   prefixed with a quote so the parser keeps leading '=' and spaces. */
#define LAYOUT_CHUNK 180

enum {
        ID_PANEL_FIRST = wxID_HIGHEST + 6000,
        ID_PANEL_LAST = ID_PANEL_FIRST + 63,
        ID_MONITOR_FRAME,
        ID_RESET_LAYOUT,
};

static wxString read_layout() {
        int parts = wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "layout_parts", 0);
        std::string s;
        for (int i = 0; i < parts; i++) {
                const char *p = wx_config_get_string(CFG_MACHINE, WINDOW_SECTION, wxString::Format("layout%d", i).mb_str(), "");
                if (!p || p[0] != '"')
                        return wxEmptyString;
                s += p + 1;
        }
        return wxString::FromUTF8(s.c_str());
}

static void write_layout(const wxString &layout) {
        std::string s(layout.utf8_str().data());
        int parts = 0;
        for (size_t pos = 0; pos < s.size(); pos += LAYOUT_CHUNK, parts++) {
                std::string chunk = "\"" + s.substr(pos, LAYOUT_CHUNK);
                wx_config_set_string(CFG_MACHINE, WINDOW_SECTION, wxString::Format("layout%d", parts).mb_str(), chunk.c_str());
        }
        wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "layout_parts", parts);
}

DisplayFrame *DisplayFrame::current = NULL;

DisplayFrame::DisplayFrame(wxWindow *owner, wxMenuBar *menubar, const char *title, SDL_DISPLAY_CALLBACK callback)
        : wxFrame(owner, wxID_ANY, title, wxDefaultPosition, wxDefaultSize, wxDEFAULT_FRAME_STYLE), callback(callback),
          owner(owner), panels_menu(NULL), panel_timer(this), panel_ticks(0), in_fullscreen(false), last_speed(0), last_fps(0),
          last_size(640, 480), last_resizable(true) {
        static bool registered = false;
        if (!registered) {
                add_config_callback(NULL, SaveWindowConfig, NULL);
                registered = true;
        }
        current = this;

        wx_set_app_icon(this);
        if (menubar) {
                SetMenuBar(menubar);
                panels_menu = new wxMenu();
                int view = menubar->FindMenu("View");
                if (view != wxNOT_FOUND)
                        menubar->Insert(view, panels_menu, "&Panels");
                else
                        menubar->Append(panels_menu, "&Panels");
        }

        wxStatusBar *status = CreateStatusBar(4);
        int widths[4] = {-1, DISPLAY_DRIVE_ICON_SPACING, 90, 90};
        status->SetStatusWidths(4, widths);
        drives = new DriveStatusPanel(status);
        status->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
                LayoutStatus();
                event.Skip();
        });

        aui.SetManagedWindow(this);
        aui.SetFlags(wxAUI_MGR_DEFAULT | wxAUI_MGR_LIVE_RESIZE);

        monitor = new MonitorPanel(this);
        monitor->SetBezel(wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "monitor_frame", 1) != 0);
        monitor->SetFilter(wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "screen_filter", VIDEO_FILTER_COLOR));
        monitor->SetCrt(wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "crt_effect", 0) != 0);
        canvas = monitor->Canvas();
        canvas->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas->SetBackgroundColour(*wxBLACK);
        aui.AddPane(monitor, wxAuiPaneInfo().Name("monitor").CenterPane().PaneBorder(false));
        /* Start big enough that the panes get their natural sizes; SetDisplaySize then fits. */
        SetClientSize(FromDIP(wxSize(1100, 800)));

        CreatePanels(read_layout());
        SetDisplaySize(640, 480, true);

        Bind(wxEVT_MENU, [this](wxCommandEvent &event) {
                if (event.GetId() == wxID_EXIT)
                        this->callback(SDL_DISPLAY_CLOSE, 0, 0);
                else if (!HandlePanelsMenu(event.GetId()))
                        this->owner->GetEventHandler()->ProcessEvent(event);
        });
        /* Rescan host optical drives whenever the CD-ROM menu opens (USB drives come and go). */
        Bind(wxEVT_MENU_OPEN, [this](wxMenuEvent &event) {
                wxMenuBar *menubar = GetMenuBar();
                if (menubar && event.GetMenu() && event.GetMenu() == wx_getsubmenu(menubar, XRCID("IDM_CDROM")))
                        wx_refresh_host_cdrom_menu();
                if (event.GetMenu() && event.GetMenu() == panels_menu)
                        UpdatePanelsMenu();
                event.Skip();
        });
        Bind(wxEVT_AUI_PANE_CLOSE, [this](wxAuiManagerEvent &event) {
                /* Keep the screen its size: give the closed pane's space back. */
                CallAfter([this]() { SetDisplaySize(last_size.x, last_size.y, last_resizable); });
                event.Skip();
        });
        Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &event) {
                if (event.CanVeto()) {
                        event.Veto();
                        this->callback(SDL_DISPLAY_CLOSE, 0, 0);
                } else
                        event.Skip();
        });
        Bind(wxEVT_ACTIVATE, [this](wxActivateEvent &event) {
                this->callback(event.GetActive() ? SDL_DISPLAY_ACTIVATE : SDL_DISPLAY_DEACTIVATE, 0, 0);
                event.Skip();
        });
        Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent &event) {
                if (event.GetEventObject() == this)
                        this->callback(SDL_DISPLAY_DESTROY, 0, 0);
                event.Skip();
        });
        canvas->Bind(wxEVT_SIZE, [this](wxSizeEvent &event) {
                wxSize size = canvas->GetClientSize();
                this->callback(SDL_DISPLAY_SIZE, size.x, size.y);
                event.Skip();
        });
        canvas->Bind(wxEVT_PAINT, [this](wxPaintEvent &event) { wxPaintDC dc(canvas); });
        canvas->Bind(wxEVT_LEFT_UP, [this](wxMouseEvent &event) { this->callback(SDL_DISPLAY_MOUSE_UP, 1, 0); });
        canvas->Bind(wxEVT_MIDDLE_UP, [this](wxMouseEvent &event) { this->callback(SDL_DISPLAY_MOUSE_UP, 2, 0); });
        canvas->Bind(wxEVT_RIGHT_UP, [this](wxMouseEvent &event) { this->callback(SDL_DISPLAY_MOUSE_UP, 3, 0); });
        auto mouse_down = [this](int button) {
                return [this, button](wxMouseEvent &event) {
                        this->callback(SDL_DISPLAY_MOUSE_DOWN, button, 0);
                        event.Skip();
                };
        };
        canvas->Bind(wxEVT_LEFT_DOWN, mouse_down(1));
        canvas->Bind(wxEVT_LEFT_DCLICK, mouse_down(1));
        canvas->Bind(wxEVT_MIDDLE_DOWN, mouse_down(2));
        canvas->Bind(wxEVT_MIDDLE_DCLICK, mouse_down(2));
        canvas->Bind(wxEVT_RIGHT_DOWN, mouse_down(3));
        canvas->Bind(wxEVT_RIGHT_DCLICK, mouse_down(3));
#ifdef __WXGTK__
        canvas->Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent &event) { this->callback(SDL_DISPLAY_KEY, event.GetRawKeyFlags(), 1); });
        canvas->Bind(wxEVT_KEY_UP, [this](wxKeyEvent &event) { this->callback(SDL_DISPLAY_KEY, event.GetRawKeyFlags(), 0); });
#else
        canvas->Bind(wxEVT_KEY_DOWN, [](wxKeyEvent &event) {});
        canvas->Bind(wxEVT_KEY_UP, [](wxKeyEvent &event) {});
#endif
        canvas->Bind(wxEVT_CHAR, [](wxKeyEvent &event) {});

        Bind(wxEVT_TIMER, [this](wxTimerEvent &) { UpdatePanels(); });
        panel_timer.Start(100);
}

DisplayFrame::~DisplayFrame() {
        panel_timer.Stop();
        if (current == this)
                current = NULL;
        aui.UnInit();
}

/* ---------------------------------------------------------------- panels */

void DisplayFrame::AddPanelPane(MachinePanel *panel, int *right_pos, int *bottom_pos) {
        wxSize best = panel->GetBestSize();
        wxAuiPaneInfo info;
        info.Name(panel->PaneName())
                .Caption(panel->Caption())
                .CloseButton(true)
                .MaximizeButton(false)
                .PinButton(false)
                .Floatable(true)
                .Dockable(true)
                .DestroyOnClose(false)
                .FloatingSize(best + FromDIP(wxSize(16, 40)));
        if (panel->DefaultRight())
                info.Right().Layer(1).Row(0).Position((*right_pos)++).BestSize(std::max(best.x, FromDIP(240)), best.y);
        else
                info.Bottom().Layer(0).Row(0).Position((*bottom_pos)++).BestSize(std::max(best.x, FromDIP(200)), best.y);
        info.MinSize(panel->GetMinSize().x > 0 ? panel->GetMinSize().x : FromDIP(150), best.y);
        aui.AddPane(panel, info);
}

/* (Re)creates the panes the current configuration has, then applies a saved layout. */
void DisplayFrame::CreatePanels(const wxString &layout) {
        for (MachinePanel *panel : panels) {
                aui.DetachPane(panel);
                panel->Destroy();
        }
        panels = wx_create_panels(this, this);
        panels_signature = wx_panels_signature();

        int right_pos = 0, bottom_pos = 0;
        for (MachinePanel *panel : panels)
                AddPanelPane(panel, &right_pos, &bottom_pos);
        aui.Update();
        default_layout = aui.SavePerspective();

        if (!layout.empty())
                ApplyLayout(layout);
        BuildPanelsMenu();
}

void DisplayFrame::ApplyLayout(const wxString &layout) {
        aui.LoadPerspective(layout, false);
        for (MachinePanel *panel : panels) {
                wxAuiPaneInfo &info = aui.GetPane(panel);
                /* Captions and minimum sizes come from the code, not from an old layout. */
                info.Caption(panel->Caption());
                wxSize best = panel->GetBestSize();
                info.MinSize(panel->GetMinSize().x > 0 ? panel->GetMinSize().x : FromDIP(150), best.y);
                /* Panes the saved layout doesn't know (e.g. a drive added since) keep their default place. */
                if (!layout.Contains("name=" + panel->PaneName() + ";"))
                        info.Show();
        }
        aui.Update();
}

void DisplayFrame::ResetLayout() {
        ApplyLayout(default_layout);
        if (!IsMaximized())
                SetDisplaySize(last_size.x, last_size.y, last_resizable);
}

wxString DisplayFrame::CurrentLayout() { return in_fullscreen ? fullscreen_layout : aui.SavePerspective(); }

void DisplayFrame::BuildPanelsMenu() {
        if (!panels_menu)
                return;
        while (panels_menu->GetMenuItemCount())
                panels_menu->Destroy(panels_menu->FindItemByPosition(0));
        for (size_t i = 0; i < panels.size() && ID_PANEL_FIRST + (int)i <= ID_PANEL_LAST; i++)
                panels_menu->AppendCheckItem(ID_PANEL_FIRST + i, panels[i]->Caption());
        panels_menu->AppendSeparator();
        panels_menu->AppendCheckItem(ID_MONITOR_FRAME, "&Monitor frame");
        wxMenu *filters = new wxMenu();
        append_filter_items(filters, monitor->GetFilter());
        panels_menu->AppendSubMenu(filters, "Screen &filter");
        panels_menu->AppendCheckItem(ID_CRT_EFFECT, "&CRT effect");
        panels_menu->Append(ID_RESET_LAYOUT, "&Reset layout");
        UpdatePanelsMenu();
}

void DisplayFrame::UpdatePanelsMenu() {
        if (!panels_menu)
                return;
        for (size_t i = 0; i < panels.size(); i++) {
                wxMenuItem *item = panels_menu->FindItem(ID_PANEL_FIRST + i);
                if (item)
                        item->Check(aui.GetPane(panels[i]).IsShown());
        }
        panels_menu->Check(ID_MONITOR_FRAME, monitor->HasBezel());
        panels_menu->Check(ID_FILTER_FIRST + monitor->GetFilter(), true);
        panels_menu->Check(ID_CRT_EFFECT, monitor->GetCrt());
}

bool DisplayFrame::HandlePanelsMenu(int id) {
        if (id >= ID_PANEL_FIRST && id < ID_PANEL_FIRST + (int)panels.size()) {
                if (in_fullscreen)
                        return true;
                wxAuiPaneInfo &info = aui.GetPane(panels[id - ID_PANEL_FIRST]);
                info.Show(!info.IsShown());
                aui.Update();
                SetDisplaySize(last_size.x, last_size.y, last_resizable);
                return true;
        }
        if (id == ID_MONITOR_FRAME) {
                monitor->SetBezel(!monitor->HasBezel());
                SetDisplaySize(last_size.x, last_size.y, last_resizable);
                return true;
        }
        if (id >= ID_FILTER_FIRST && id <= ID_FILTER_LAST) {
                monitor->SetFilter(id - ID_FILTER_FIRST);
                return true;
        }
        if (id == ID_CRT_EFFECT) {
                monitor->SetCrt(!monitor->GetCrt());
                return true;
        }
        if (id == ID_RESET_LAYOUT) {
                if (!in_fullscreen)
                        ResetLayout();
                return true;
        }
        return false;
}

void DisplayFrame::UpdatePanels() {
        if (!IsShown())
                return;

        MachineStatus status;
        char text[4096];
        int num = 0;
        drive_info_t *info = get_machine_info(text, &num);
        status.num_drives = std::min(num, PANEL_MAX_DRIVES);
        memcpy(status.drives, info, status.num_drives * sizeof(drive_info_t));
        status.state = emulation_state;
        status.speed = last_speed;
        status.fps = last_fps;

        for (MachinePanel *panel : panels)
                if (aui.GetPane(panel).IsShown())
                        panel->UpdateStatus(status);
        monitor->SetPowerState(status.state);
        if (drives->UpdateDrives(status.drives, status.num_drives))
                LayoutStatus();

        /* The configuration can change while running (Settings from the Machine window). */
        if (++panel_ticks % 10 == 0 && wx_panels_signature() != panels_signature && !in_fullscreen)
                CreatePanels(CurrentLayout());
}

void DisplayFrame::RunCommand(int id) {
        wxCommandEvent event(wxEVT_MENU, id);
        event.SetEventObject(this);
        owner->GetEventHandler()->ProcessEvent(event);
}

void DisplayFrame::FocusDisplay() {
        CallAfter([this]() {
                if (IsActive())
                        canvas->SetFocus();
        });
}

/* Config save callback: the window's place and layout, per machine. */
void DisplayFrame::SaveWindowConfig() {
        DisplayFrame *f = current;
        if (!f)
                return;
        int x = wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "x", INT_MIN);
        int y = wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "y", INT_MIN);
        bool maximized = f->IsMaximized();
        if (!f->in_fullscreen && !f->IsIconized() && !maximized && f->IsShown()) {
                wxPoint pos = f->GetPosition();
                x = pos.x;
                y = pos.y;
        }
        config_free_section(CFG_MACHINE, (char *)WINDOW_SECTION);
        if (x != INT_MIN && y != INT_MIN) {
                wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "x", x);
                wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "y", y);
        }
        wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "maximized", maximized);
        wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "monitor_frame", f->monitor->HasBezel());
        wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "screen_filter", f->monitor->GetFilter());
        wx_config_set_int(CFG_MACHINE, WINDOW_SECTION, "crt_effect", f->monitor->GetCrt());
        write_layout(f->CurrentLayout());
}

/* ---------------------------------------------------------------- display host */

void DisplayFrame::LayoutStatus() {
        wxStatusBar *status = GetStatusBar();
        if (!status)
                return;
        int widths[4] = {-1, drives->GetRequiredWidth(), 90, 90};
        status->SetStatusWidths(4, widths);
        wxRect rect;
        if (status->GetFieldRect(1, rect))
                drives->SetSize(rect);
}

void *DisplayFrame::NativeCanvas() {
#ifdef __WXGTK__
        GdkWindow *gdk_window = canvas->GTKGetDrawingWindow();
        if (!gdk_window || !GDK_IS_X11_WINDOW(gdk_window))
                return NULL;
        return (void *)(uintptr_t)gdk_x11_window_get_xid(gdk_window);
#else
        return (void *)canvas->GetHandle();
#endif
}

/* Sizes the window so the screen (the canvas) is width x height. A fixed-size display keeps
   that size whatever the window does; a resizable one follows the window. */
void DisplayFrame::SetDisplaySize(int width, int height, bool resizable) {
        if (width <= 0 || height <= 0)
                return;
        last_size = wxSize(width, height);
        last_resizable = resizable;
        if (in_fullscreen || IsFullScreen())
                return;
        monitor->SetScreenSize(resizable ? wxDefaultSize : wxSize(width, height));
        if (IsMaximized())
                return;
        /* The docked panes may have been squeezed before, so the window may need a couple of
           passes to settle. */
        wxSize need = wxSize(width, height) + monitor->ChromeSize();
        for (int pass = 0; pass < 3; pass++) {
                wxSize have = monitor->GetSize();
                if (need == have)
                        break;
                SetClientSize(GetClientSize() + (need - have));
                aui.Update();
        }
}

void DisplayFrame::ShowDisplay(bool show) {
        if (!show && IsFullScreen())
                ShowFullScreen(false);
        Show(show);
        if (show) {
                Raise();
                canvas->SetFocus();
        }
}

int DisplayFrame::TopSafeInset() {
#ifdef __WXOSX__
        typedef struct {
                double top, left, bottom, right;
        } edge_insets_t;
        void *nswindow = (void *)GetWXWindow();
        if (!nswindow)
                return 0;
        void *screen = ((void *(*)(void *, SEL))objc_msgSend)(nswindow, sel_registerName("screen"));
        SEL sel = sel_registerName("safeAreaInsets");
        if (!screen || !((BOOL(*)(void *, SEL, SEL))objc_msgSend)(screen, sel_registerName("respondsToSelector:"), sel))
                return 0;
        edge_insets_t insets;
#if defined(__x86_64__)
        ((void (*)(edge_insets_t *, void *, SEL))objc_msgSend_stret)(&insets, screen, sel);
#else
        insets = ((edge_insets_t(*)(void *, SEL))objc_msgSend)(screen, sel);
#endif
        return insets.top > 0 ? (int)ceil(insets.top) : 0;
#else
        return 0;
#endif
}

void DisplayFrame::SetDisplayFullscreen(bool fullscreen) {
        if (fullscreen && !in_fullscreen) {
                /* Fullscreen shows only the screen: hide the panels and the monitor frame. */
                fullscreen_layout = aui.SavePerspective();
                for (MachinePanel *panel : panels)
                        aui.GetPane(panel).Hide();
                in_fullscreen = true;
                monitor->SetFullscreen(true, 0);
                aui.Update();
        }
        if (fullscreen) {
#ifdef __WXGTK__
                GdkWindow *gdk_window = gtk_widget_get_window((GtkWidget *)GetHandle());
                if (gdk_window) {
                        GdkGeometry geometry = {};
                        gdk_window_set_geometry_hints(gdk_window, &geometry, (GdkWindowHints)0);
                        gdk_display_flush(gdk_window_get_display(gdk_window));
                }
#endif
        }
        ShowFullScreen(fullscreen, wxFULLSCREEN_ALL);

        wxStatusBar *status = GetStatusBar();
        if (status && status->IsShown() != !fullscreen)
                status->Show(!fullscreen);

        if (!fullscreen && in_fullscreen) {
                in_fullscreen = false;
                monitor->SetFullscreen(false, 0);
                ApplyLayout(fullscreen_layout);
        }
#ifdef __WXOSX__
        if (fullscreen)
                monitor->SetFullscreen(true, TopSafeInset());
#endif
        SendSizeEvent();
        if (fullscreen)
                Raise();
}

void DisplayFrame::SetStatusField(int field, const wxString &text) {
        wxStatusBar *status = GetStatusBar();
        if (!status || field >= status->GetFieldsCount())
                return;
        if (status->GetStatusText(field) != text)
                status->SetStatusText(text, field);
}

void DisplayFrame::SetStatus(const char *message, int speed, int fps) {
        last_speed = speed;
        last_fps = fps;
        SetStatusField(0, message);
        SetStatusField(2, wxString::Format("Speed: %d%%", speed));
        SetStatusField(3, wxString::Format("FPS: %d", fps));
#ifdef __WXOSX__
        wxStatusBar *status = GetStatusBar();
        if (status)
                status->Update();
#endif
}

void *DisplayFrame::HostNativeCanvas(void *frame) { return ((DisplayFrame *)frame)->NativeCanvas(); }

void DisplayFrame::HostCanvasSize(void *frame, int *width, int *height) {
        wxSize size = ((DisplayFrame *)frame)->canvas->GetClientSize();
        *width = size.x;
        *height = size.y;
}

void DisplayFrame::HostSetSize(void *frame, int width, int height, int resizable) {
        ((DisplayFrame *)frame)->SetDisplaySize(width, height, resizable != 0);
}

/* Window position, and the size of the screen (what HostSetSize sets). */
void DisplayFrame::HostGetRect(void *frame, int *x, int *y, int *width, int *height) {
        DisplayFrame *f = (DisplayFrame *)frame;
        wxPoint pos = f->GetPosition();
        wxSize size = f->canvas->GetClientSize();
        *x = pos.x;
        *y = pos.y;
        *width = size.x;
        *height = size.y;
}

void DisplayFrame::HostSetPosition(void *frame, int x, int y) { ((DisplayFrame *)frame)->SetPosition(wxPoint(x, y)); }

/* Places a new window where this machine's window was last time, else centred. */
void DisplayFrame::HostCenter(void *frame) {
        DisplayFrame *f = (DisplayFrame *)frame;
        int x = wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "x", INT_MIN);
        int y = wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "y", INT_MIN);
        if (x != INT_MIN && y != INT_MIN && wxDisplay::GetFromPoint(wxPoint(x + 40, y + 20)) != wxNOT_FOUND)
                f->SetPosition(wxPoint(x, y));
        else
                f->CenterOnScreen();
        if (wx_config_get_int(CFG_MACHINE, WINDOW_SECTION, "maximized", 0))
                f->Maximize();
}

void DisplayFrame::HostShow(void *frame, int show) { ((DisplayFrame *)frame)->ShowDisplay(show != 0); }

void DisplayFrame::HostSetFullscreen(void *frame, int fullscreen) {
        ((DisplayFrame *)frame)->SetDisplayFullscreen(fullscreen != 0);
}

void DisplayFrame::HostSetTitle(void *frame, const char *title) { ((DisplayFrame *)frame)->SetTitle(title); }

void DisplayFrame::HostSetStatus(void *frame, const char *message, int speed, int fps) {
        ((DisplayFrame *)frame)->SetStatus(message, speed, fps);
}
