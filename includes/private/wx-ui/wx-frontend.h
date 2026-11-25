#ifndef _WX_FRONTEND_H_
#define _WX_FRONTEND_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

void wx_frontend_init();
int wx_start(wxWindow *hwnd);
int wx_stop(wxWindow *hwnd);
void wx_handle_command(wxWindow *hwnd, int id, bool checked);
void wx_refresh_host_cdrom_menu();

/* Media changes for the machine window's drive panels (and the menus). */
void wx_load_disc_file(int drive, const wxString &file);
void wx_load_cdrom_image(const wxString &file);
void wx_load_zip_file(const wxString &file);

bool wx_getfile(wxWindow *parent, const wxString &wildcard, const wxString &path, wxString &result,
                const wxString &caption = "Open");
bool wx_getsfile(wxWindow *parent, const wxString &wildcard, const wxString &path, const wxString &ext, wxString &result);

#endif /* _WX_FRONTEND_H_ */
