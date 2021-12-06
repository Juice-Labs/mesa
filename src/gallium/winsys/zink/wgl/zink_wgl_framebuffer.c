/*
 * Copyright © Microsoft Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "zink_wgl_public.h"
#include <Windows.h>
#include "util/u_memory.h"
#include "util/u_inlines.h"
#include "frontend/api.h"
#include "frontend/winsys_handle.h"
#include "stw_device.h"
#include "stw_pixelformat.h"
#include "stw_winsys.h"
#include "zink/zink_screen.h"

enum Limits
{
   num_buffers = 2
};

struct zink_wgl_framebuffer {
   struct stw_winsys_framebuffer base;
   struct zink_screen* screen;
   enum pipe_format pformat;
   HWND window;
   VkSwapchainKHR swapchain;
   struct pipe_resource *buffers[num_buffers];
};

static struct zink_wgl_framebuffer *
zink_wgl_framebuffer(struct stw_winsys_framebuffer *fb)
{
   return (struct zink_wgl_framebuffer *)fb;
}

static void
zink_wgl_framebuffer_destroy(struct stw_winsys_framebuffer *fb,
                              struct pipe_context *ctx)
{
   struct zink_wgl_framebuffer *framebuffer = zink_wgl_framebuffer(fb);
   struct zink_screen *screen = framebuffer->screen;
   struct pipe_fence_handle *fence = NULL;

   if (ctx) {
      /* Ensure all resources are flushed */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence, PIPE_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }
   }

   for (int i = 0; i < num_buffers; ++i) {
      if (framebuffer->buffers[i]) {
         // zink_resource_release(zink_resource(framebuffer->buffers[i]));
         pipe_resource_reference(&framebuffer->buffers[i], NULL);
      }
   }

   VKSCR(DestroySwapchainKHR)(framebuffer->screen->dev, framebuffer->swapchain, NULL);
   framebuffer->swapchain = VK_NULL_HANDLE;   

   free(framebuffer);
}

static void
zink_wgl_framebuffer_resize(struct stw_winsys_framebuffer* fb,
   struct pipe_context* ctx,
   struct pipe_resource* template)
{
   struct zink_wgl_framebuffer* framebuffer = zink_wgl_framebuffer(fb);
   struct zink_screen* screen = framebuffer->screen;

   int width = template->width0;
   assert(width > 0);

   int height = template->height0;
   assert(height > 0);

   VkFormat format = zink_get_format(screen, template->format);
   assert(format != VK_FORMAT_UNDEFINED);

   if (framebuffer->swapchain != VK_NULL_HANDLE) {
      struct pipe_fence_handle* fence = NULL;

      /* Ensure all resources are flushed */
      ctx->flush(ctx, &fence, PIPE_FLUSH_HINT_FINISH);
      if (fence) {
         ctx->screen->fence_finish(ctx->screen, ctx, fence, PIPE_TIMEOUT_INFINITE);
         ctx->screen->fence_reference(ctx->screen, &fence, NULL);
      }

      for (int i = 0; i < num_buffers; ++i) {
         if (framebuffer->buffers[i]) {
            // zink_resource_release(zink_resource(framebuffer->buffers[i]));
            pipe_resource_reference(&framebuffer->buffers[i], NULL);
         }
      }
   }

   uint32_t queue_family_indices[2] = {
      screen->graphics_queue_family,
      screen->present_queue_family
   };

   VkSurfaceCapabilitiesKHR capabilities = {0};
   VkResult result = VKSCR(GetPhysicalDeviceSurfaceCapabilitiesKHR)(screen->pdev, screen->surface, &capabilities);
   assert(result == VK_SUCCESS);

   VkSwapchainCreateInfoKHR info = { 0 };
   info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
   info.pNext = NULL;
   info.flags = 0;
   info.surface = screen->surface;
   info.minImageCount = 2;
   info.imageFormat = format;
   info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   info.imageExtent.width = width;
   info.imageExtent.height = height;
   info.imageArrayLayers = 1;
   info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
   info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
   info.queueFamilyIndexCount = screen->graphics_queue_family == screen->present_queue_family ? 1 : 2;
   info.pQueueFamilyIndices = queue_family_indices;
   info.preTransform = capabilities.currentTransform;
   info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
   info.presentMode =  VK_PRESENT_MODE_FIFO_KHR;
   info.clipped = VK_TRUE;
   info.oldSwapchain = framebuffer->swapchain;

   VkSwapchainKHR swapchain = VK_NULL_HANDLE;
   VkDevice device = screen->dev;
   result = VKSCR(CreateSwapchainKHR)(device, &info, NULL, &swapchain);
   assert(result == VK_SUCCESS);
   assert(swapchain != VK_NULL_HANDLE);   
   if (framebuffer->swapchain != VK_NULL_HANDLE)
   {
      VKSCR(DestroySwapchainKHR)(device, framebuffer->swapchain, NULL);
      framebuffer->swapchain = VK_NULL_HANDLE;
   }
   framebuffer->swapchain = swapchain;
}

static boolean
zink_wgl_framebuffer_present(struct stw_winsys_framebuffer *fb)
{
   struct zink_wgl_framebuffer *framebuffer = zink_wgl_framebuffer(fb);
   if (!framebuffer->swapchain) {
      debug_printf("zink: Cannot present; no swapchain");
      return false;
   }

   // if (stw_dev->swap_interval < 1)
   //    return S_OK == framebuffer->swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
   // else
   //     return S_OK == framebuffer->swapchain->Present(stw_dev->swap_interval, 0);
   return false;
}

static struct pipe_resource *
zink_wgl_framebuffer_get_resource(struct stw_winsys_framebuffer *pframebuffer,
                                   enum st_attachment_type statt)
{
   struct zink_wgl_framebuffer *framebuffer = zink_wgl_framebuffer(pframebuffer);
   struct pipe_screen *pscreen = &framebuffer->screen->base;

   if (!framebuffer->swapchain)
      return NULL;

   return NULL;
}

struct stw_winsys_framebuffer *
zink_wgl_create_framebuffer(struct pipe_screen *screen,
                             HWND hWnd,
                             int iPixelFormat)
{
   const struct stw_pixelformat_info *pfi =
      stw_pixelformat_get_info(iPixelFormat);
   if (!(pfi->pfd.dwFlags & PFD_DOUBLEBUFFER) ||
       (pfi->pfd.dwFlags & PFD_SUPPORT_GDI))
      return NULL;

   struct zink_wgl_framebuffer *fb = CALLOC_STRUCT(zink_wgl_framebuffer);
   if (!fb)
      return NULL;

   // new (fb) struct zink_wgl_framebuffer();

   fb->window = hWnd;
   fb->screen = zink_screen(screen);
   fb->base.destroy = zink_wgl_framebuffer_destroy;
   fb->base.resize = zink_wgl_framebuffer_resize;
   fb->base.present = zink_wgl_framebuffer_present;
   fb->base.get_resource = zink_wgl_framebuffer_get_resource;

   return &fb->base;
}
