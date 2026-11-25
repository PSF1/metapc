#include "wx-machineinfo.h"

#include <wx/filename.h>

extern "C" {
#include "ibm.h"
#include "cpu.h"
#include "fdd.h"
#include "gameport.h"
#include "hdd.h"
#include "ide.h"
#include "lpt.h"
#include "model.h"
#include "mouse.h"
#include "nethandler.h"
#include "scsi_cd.h"
#include "sound.h"
#include "video.h"
#include "cdrom-image.h"
}

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

wxString wx_floppy_type_name(int type) {
        static const char *names[] = {"None",       "5.25\" 360k", "5.25\" 1.2M",        "5.25\" 1.2M Dual RPM",
                                      "3.5\" 720k", "3.5\" 1.44M", "3.5\" 1.44M 3-Mode", "3.5\" 2.88M"};
        if (type < 0 || type >= (int)ARRAY_LEN(names))
                return "Unknown";
        return names[type];
}

wxString wx_machine_model_name() { return wxString(model_getname()); }

wxString wx_machine_cpu_name() {
        MODEL *m = models[model];
        if (!m || cpu_manufacturer < 0 || cpu_manufacturer >= 5 || !m->cpu[cpu_manufacturer].cpus)
                return "Unknown";
        return wxString(m->cpu[cpu_manufacturer].cpus[cpu].name);
}

static wxString fpu_name() {
        for (int c = 0;; c++) {
                const char *name = fpu_get_name_from_index(model, cpu_manufacturer, cpu, c);
                if (!name)
                        break;
                if (fpu_get_type_from_index(model, cpu_manufacturer, cpu, c) == fpu_type)
                        return name;
        }
        return "None";
}

wxString wx_machine_memory() {
        if (mem_size >= 1024 && !(mem_size & 1023))
                return wxString::Format("%d MB", mem_size / 1024);
        return wxString::Format("%d KB", mem_size);
}

wxString wx_machine_video_name() { return wxString(video_card_getname(video_old_to_new(gfxcard))); }

wxString wx_machine_sound_name() { return wxString(sound_card_getname(sound_card_current)); }

static wxString hdd_controller_display_name() {
        for (int c = 0;; c++) {
                char *s = hdd_controller_get_name(c);
                if (!s || !s[0])
                        break;
                if (!strcmp(hdd_controller_get_internal_name(c), hdd_controller_name))
                        return s;
        }
        return hdd_controller_name;
}

static wxString lpt1_display_name() {
        for (int c = 0; lpt_device_get_name(c); c++)
                if (!strcmp(lpt1_device_name, lpt_device_get_internal_name(c)))
                        return lpt_device_get_name(c);
        return "None";
}

static wxString disk_size(int i) {
        long long bytes = (long long)hdc[i].spt * hdc[i].hpc * hdc[i].tracks * 512;
        if (bytes <= 0)
                return wxEmptyString;
        if (bytes >= 1024LL * 1024 * 1024)
                return wxString::Format("%.1f GB", bytes / (1024.0 * 1024 * 1024));
        return wxString::Format("%lld MB", bytes / (1024 * 1024));
}

static wxString channel_name(int channel) {
        static const char *names[] = {"Primary master", "Primary slave", "Secondary master", "Secondary slave",
                                      "Tertiary master", "Tertiary slave", "Quaternary master"};
        if (hdd_controller_current_is_scsi())
                return wxString::Format("SCSI ID %d", channel);
        if (channel >= 0 && channel < (int)ARRAY_LEN(names))
                return names[channel];
        return wxString::Format("Channel %d", channel);
}

static wxString cdrom_media() {
        if (cdrom_drive == CDROM_IMAGE)
                return image_path[0] ? wxFileName(image_path).GetFullName() : wxString("Empty");
        if (cdrom_drive > 0) {
#ifdef _WIN32
                return wxString::Format("Host drive %c:", cdrom_drive);
#else
                return "Host drive";
#endif
        }
        return "Empty";
}

std::vector<MachineInfoSection> wx_machine_info() {
        std::vector<MachineInfoSection> sections;

        MachineInfoSection system = {"System", "BITMAP_MOTHERBOARD", {}};
        system.items.push_back({"Model", wx_machine_model_name()});
        system.items.push_back({"CPU", wx_machine_cpu_name()});
        system.items.push_back({"FPU", fpu_name()});
        system.items.push_back({"Memory", wx_machine_memory()});
        system.items.push_back({"Dynamic recompiler", cpu_use_dynarec ? "Enabled" : "Disabled"});
        sections.push_back(system);

        MachineInfoSection display = {"Display", "BITMAP_VIDEO", {}};
        display.items.push_back({"Video card", wx_machine_video_name()});
        if (voodoo_enabled)
                display.items.push_back({"3D accelerator", "3dfx Voodoo"});
        sections.push_back(display);

        MachineInfoSection storage = {"Storage", "BITMAP_HARDDISK", {}};
        for (int i = 0; i < 2; i++) {
                int type = fdd_get_type(i);
                if (type > 0)
                        storage.items.push_back({wxString::Format("Floppy %c:", 'A' + i), wx_floppy_type_name(type)});
        }
        storage.items.push_back({"Controller", hdd_controller_display_name()});
        bool mfm = hdd_controller_current_is_mfm();
        int num_hdds = mfm ? 2 : (hdd_controller_current_is_scsi() ? 7 : 4);
        for (int i = 0; i < num_hdds; i++) {
                if (!mfm && i == cdrom_channel && cdrom_drive >= 0) {
                        storage.items.push_back({channel_name(i), wxString::Format("CD-ROM (%s)", cdrom_media())});
                } else if (!mfm && i == zip_channel) {
                        storage.items.push_back({channel_name(i), "ZIP drive"});
                } else if (ide_fn[i][0]) {
                        wxString value = wxFileName(ide_fn[i]).GetFullName();
                        wxString size = disk_size(i);
                        if (!size.empty())
                                value += " (" + size + ")";
                        storage.items.push_back({mfm ? wxString::Format("Drive %d", i) : channel_name(i), value});
                }
        }
        sections.push_back(storage);

        MachineInfoSection audio = {"Audio", "BITMAP_SOUND", {}};
        audio.items.push_back({"Sound card", wx_machine_sound_name()});
        wxString extras;
        if (GAMEBLASTER)
                extras += "Game Blaster, ";
        if (GUS)
                extras += "Gravis UltraSound, ";
        if (SSI2001)
                extras += "SSI-2001, ";
        if (!extras.empty())
                audio.items.push_back({"Additional", extras.Left(extras.length() - 2)});
        sections.push_back(audio);

#ifdef USE_NETWORKING
        MachineInfoSection network = {"Network", "BITMAP_NETWORK", {}};
        network.items.push_back({"Adapter", network_card_getname(network_card_current)});
        sections.push_back(network);
#endif

        MachineInfoSection ports = {"Peripherals", "BITMAP_PORT", {}};
        char *mouse = mouse_get_name(mouse_type);
        ports.items.push_back({"Mouse", mouse ? wxString(mouse) : wxString("None")});
        char *joystick = joystick_get_name(joystick_type);
        ports.items.push_back({"Joystick", joystick ? wxString(joystick) : wxString("None")});
        ports.items.push_back({"LPT1", lpt1_display_name()});
        sections.push_back(ports);

        return sections;
}

wxString wx_machine_instance_name(const wxString &config_path) {
        wxFileName fn(config_path);
        fn.Normalize(wxPATH_NORM_ABSOLUTE | wxPATH_NORM_DOTS);
        wxString path = fn.GetFullPath();
#ifdef _WIN32
        path.MakeLower();
#endif
        /* FNV-1a over the path: the name must be usable as a mutex/lock file name. */
        unsigned long long hash = 1469598103934665603ULL;
        wxScopedCharBuffer utf8 = path.utf8_str();
        for (const char *p = utf8.data(); *p; p++) {
                hash ^= (unsigned char)*p;
                hash *= 1099511628211ULL;
        }
        return wxString::Format("PCem-%s-%016llx", wxGetUserId(), hash);
}

wxString wx_machine_pid_file(const wxString &config_path) {
        return wxFileName(wxFileName::GetTempDir(), wx_machine_instance_name(config_path) + ".pid").GetFullPath();
}
