#ifndef ZINK_WGL_PUBLIC_H
#define ZINK_WGL_PUBLIC_H

#include <Windows.h>
#include <vulkan/vulkan.h>

struct pipe_resource;
struct pipe_screen;
struct pipe_context;
struct stw_winsys;

struct pipe_screen* zink_wgl_create_screen(struct sw_winsys* winsys, HDC hdc);
void zink_wgl_present(struct pipe_screen *screen, struct pipe_context* context, struct pipe_resource* res, HDC hdc);
unsigned int zink_wgl_get_pfd_flags(struct pipe_screen* screen);
struct stw_winsys_framebuffer* zink_wgl_create_framebuffer(struct pipe_screen* screen, HWND hWnd, int iPixelFormat);
VkSemaphore zink_framebuffer_present_finished(struct zink_wgl_framebuffer* framebuffer);
VkSemaphore zink_framebuffer_draw_finished(struct zink_wgl_framebuffer* framebuffer);

#endif
