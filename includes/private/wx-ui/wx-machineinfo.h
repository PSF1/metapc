#ifndef _WX_MACHINEINFO_H_
#define _WX_MACHINEINFO_H_

#include <wx/wxprec.h>
#ifndef WX_PRECOMP
#include <wx/wx.h>
#endif

#include <vector>

/* A summary of the loaded machine configuration (the emulator's config globals), grouped
   in sections the way the machine manager shows it. */
struct MachineInfoItem {
        wxString label;
        wxString value;
};

struct MachineInfoSection {
        wxString title;
        wxString icon; /* XRC bitmap name */
        std::vector<MachineInfoItem> items;
};

std::vector<MachineInfoSection> wx_machine_info();

wxString wx_machine_model_name();
wxString wx_machine_cpu_name();
wxString wx_machine_memory();
wxString wx_machine_video_name();
wxString wx_machine_sound_name();
wxString wx_floppy_type_name(int type);

/* Name of the lock a running machine holds, so other PCem processes can tell it is running. */
wxString wx_machine_instance_name(const wxString &config_path);

#endif /* _WX_MACHINEINFO_H_ */
