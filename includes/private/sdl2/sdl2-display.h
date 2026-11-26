#ifndef _SDL2_DISPLAY_H_
#define _SDL2_DISPLAY_H_

#ifdef __cplusplus
extern "C" {
#endif

int display_init();
void display_close();
void display_start(void *hwnd);
void display_stop();
void display_resize(int width, int height);
int display_raw_mouse(int *dx, int *dy, int *buttons);
int display_host_mouse_buttons();

/* Colour filter for the emulated screen, as on a monochrome monitor. */
#define VIDEO_FILTER_COLOR 0
#define VIDEO_FILTER_GRAY 1
#define VIDEO_FILTER_GREEN 2
#define VIDEO_FILTER_AMBER 3
void video_set_color_filter(int filter);
int video_get_color_filter();
/* Scanlines, a softer picture and darker edges, as on a CRT; combines with the filter. */
void video_set_crt_effect(int on);
int video_get_crt_effect();

#ifdef __cplusplus
}
#endif

extern int mousecapture;

extern int video_scale_mode;
extern int video_vsync;
extern int video_focus_dim;
extern int video_fullscreen_mode;
extern int video_alternative_update_lock;

extern int win_doresize;

extern int window_doreset;
extern int window_dosetresize;
extern int renderer_doreset;
extern int window_dofullscreen;
extern int window_dowindowed;
extern int window_doremember;
extern int window_doinputgrab;
extern int window_doinputrelease;
extern int window_dotogglefullscreen;

#endif /* _SDL2_DISPLAY_H_ */
