#include "wx-app.h"
#include "wx-frontend.h"
#include <SDL2/SDL.h>
#include <string>

#if WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

extern "C" {
int pc_main(int, char **);
}

#if WIN32
/* A machine that uses a CompactFlash card starts itself again with administrator rights (see
   relaunch_elevated() in wx-app.cc). If the user is not an administrator, Windows asks for
   another account's password and the machine runs as that account. It is told to keep using
   the user's data folder (--data-dir), but that account may not be allowed to read it: then
   the ROMs would not be found and the emulator would stop with a cryptic error. Checked here,
   before the emulator starts, so the user is told why. */
static bool elevated_account_can_start(int argc, char **argv) {
        const char *by = NULL, *dir = NULL;
        for (int i = 1; i + 1 < argc; i++) {
                if (!_stricmp(argv[i], "--elevated-by"))
                        by = argv[i + 1];
                else if (!_stricmp(argv[i], "--data-dir"))
                        dir = argv[i + 1];
        }
        if (!by || !dir)
                return true;

        char me[256];
        DWORD len = sizeof(me);
        if (!GetUserNameA(me, &len) || !_stricmp(me, by))
                return true;

        std::string pattern = dir;
        if (!pattern.empty() && pattern.back() != '/' && pattern.back() != '\\')
                pattern += "\\";
        WIN32_FIND_DATAA found;
        HANDLE h = FindFirstFileA((pattern + "*").c_str(), &found);
        if (h != INVALID_HANDLE_VALUE) {
                FindClose(h);
                return true;
        }

        wxString msg = wxString::Format(
                "This machine uses a CompactFlash card, which needs an administrator's permission. "
                "Windows started it with the administrator account \"%s\", but that account cannot read "
                "the MetaPC files of \"%s\" (ROMs, machine settings and CMOS) in:\n\n%s\n\n"
                "To use the card, either sign in to Windows with an account that is an administrator, "
                "or give the account \"%s\" access to that folder.",
                me, by, dir, me);
        MessageBoxW(NULL, msg.wc_str(), L"MetaPC", MB_OK | MB_ICONERROR);
        return false;
}
#endif

#if WIN32
int __stdcall WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, char *, int nShowCmd)
#else
int main(int argc, char **argv)
#endif
{
#if WIN32
        int argc = __argc;
        char **argv = __argv;
#endif

#ifdef FORCE_X11
        setenv("SDL_VIDEODRIVER", "x11", 1);
        setenv("GDK_BACKEND", "x11", 1);
#endif

#if WIN32
        if (!elevated_account_can_start(argc, argv))
                return -1;
#endif

        wx_frontend_init();

        if (!pc_main(argc, argv))
                return -1;

        wxApp::SetInstance(new App());
        wxEntry(argc, argv);
        return 0;
}
