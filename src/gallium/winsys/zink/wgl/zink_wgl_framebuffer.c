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
   maximum_buffers = 8
};

struct zink_wgl_framebuffer {
   struct stw_winsys_framebuffer base;
   struct zink_screen* screen;
   enum pipe_format pformat;
   int width;
   int height;
   HWND window;
   VkSurfaceKHR surface;
   VkSwapchainKHR swapchain;
   VkSemaphore image_available [maximum_buffers];
   VkSemaphore draw_finished [maximum_buffers];
   VkFence present_done_fence [maximum_buffers];
   int frame;
   int acquired_image;
   struct pipe_resource *buffers[maximum_buffers];
   int surface_formats_count;
   VkSurfaceFormatKHR* surface_formats;
   int present_modes_count;
   VkPresentModeKHR* present_modes;
};

static struct zink_wgl_framebuffer *
zink_wgl_framebuffer(struct stw_winsys_framebuffer *fb)
{
   return (struct zink_wgl_framebuffer *)fb;
}

static void
zink_framebuffer_acquire_next_image(struct zink_wgl_framebuffer *framebuffer)
{
   assert(framebuffer);
   assert(framebuffer->screen->dev != VK_NULL_HANDLE);
   assert(framebuffer->swapchain != VK_NULL_HANDLE);
   assert(framebuffer->image_available != VK_NULL_HANDLE);

   struct zink_screen *screen = framebuffer->screen;
   VkDevice device = screen->dev;
   VkSwapchainKHR swapchain = framebuffer->swapchain;

   ++framebuffer->frame;
   int frame = framebuffer->frame % maximum_buffers;
   VkSemaphore semaphore = framebuffer->image_available[frame];
   VkFence fence = framebuffer->present_done_fence[frame];

   VkResult result = VKSCR(WaitForFences)(device, 1, &fence, VK_TRUE, UINT64_MAX);
   assert(result == VK_SUCCESS);

   result = VKSCR(ResetFences)(device, 1, &fence);
   assert(result == VK_SUCCESS);

   uint32_t index = ~0;
   result = VKSCR(AcquireNextImageKHR)(device, swapchain, UINT64_MAX, semaphore, fence, &index);
   assert(result == VK_SUCCESS);
   framebuffer->acquired_image = index;
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

   for (int i = 0; i < maximum_buffers; ++i) {
      if (framebuffer->buffers[i]) {
         // zink_resource_release(zink_resource(framebuffer->buffers[i]));
         pipe_resource_reference(&framebuffer->buffers[i], NULL);
      }
   }

   free(framebuffer->present_modes);
   framebuffer->present_modes = NULL;
   framebuffer->present_modes = 0;

   free(framebuffer->surface_formats);
   framebuffer->surface_formats = NULL;
   framebuffer->surface_formats_count = 0;

   for (int i = 0; i < maximum_buffers; ++i) {
      if (framebuffer->present_done_fence[i] != VK_NULL_HANDLE) {
         VKSCR(WaitForFences)(screen->dev, 1, &framebuffer->present_done_fence[i], VK_TRUE, UINT64_MAX);
      }
      VKSCR(DestroySemaphore)(screen->dev, framebuffer->draw_finished[i], NULL);
      framebuffer->draw_finished[i] = VK_NULL_HANDLE;
      VKSCR(DestroySemaphore)(screen->dev, framebuffer->image_available[i], NULL);
      framebuffer->image_available[i] = VK_NULL_HANDLE;
      VKSCR(DestroyFence)(screen->dev, framebuffer->present_done_fence[i], NULL);
      framebuffer->present_done_fence[i] = VK_NULL_HANDLE;
   }

   VKSCR(DestroySwapchainKHR)(screen->dev, framebuffer->swapchain, NULL);
   framebuffer->swapchain = VK_NULL_HANDLE;

   VKSCR(DestroySurfaceKHR)(screen->instance, framebuffer->surface, NULL);
   framebuffer->surface = VK_NULL_HANDLE;

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

      for (int i = 0; i < maximum_buffers; ++i) {
         if (framebuffer->buffers[i]) {
            // zink_resource_release(zink_resource(framebuffer->buffers[i]));
            pipe_resource_reference(&framebuffer->buffers[i], NULL);
         }
      }
   }

   if (framebuffer->swapchain == VK_NULL_HANDLE) {
      assert(framebuffer->pformat == PIPE_FORMAT_NONE);
      framebuffer->pformat = template->format;

      assert(framebuffer->surface == VK_NULL_HANDLE);
      VkSurfaceKHR surface = VK_NULL_HANDLE;
#ifdef _WIN32
      VkWin32SurfaceCreateInfoKHR surface_create = {0};
      surface_create.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
      surface_create.hwnd = framebuffer->window;
      surface_create.hinstance = GetModuleHandle(NULL);
      VkResult result = VKSCR(CreateWin32SurfaceKHR)(screen->instance, &surface_create, NULL, &surface);
      assert(result == VK_SUCCESS);
#endif
      framebuffer->surface = surface;

      // TODO: The effective surface formats are those queried from the
      // temporary surface created when the physical device is choosen
      // in choose_pdev() (see zink_screen.c).  That's not correct though
      // because the surface used here might be different and support
      // different formats.  Hence the query here.  These aren't used yet
      // but calling this function silences validation layer warnings.
      uint32_t surface_formats_count = 0;
      VKSCR(GetPhysicalDeviceSurfaceFormatsKHR)(screen->pdev, surface, &surface_formats_count, NULL);
      framebuffer->surface_formats = malloc( sizeof(VkSurfaceFormatKHR*) * surface_formats_count );
      VKSCR(GetPhysicalDeviceSurfaceFormatsKHR)(screen->pdev, surface, &surface_formats_count, framebuffer->surface_formats);
      framebuffer->surface_formats_count = surface_formats_count;

      // TODO: The effective present modes are those queried from the
      // temporary surface created when the physical device is choosen
      // in choose_pdev() (see zink_screen.c).  That's not correct though
      // because the surface used here might be different and support
      // different modes.  Hence the query here.  These aren't used yet
      // but calling this function silences validation layer warnings.
      uint32_t present_modes_count = 0;
      VKSCR(GetPhysicalDeviceSurfacePresentModesKHR)(screen->pdev, surface, &present_modes_count, NULL);
      framebuffer->present_modes = malloc( sizeof(VkPresentModeKHR*) * present_modes_count );
      VKSCR(GetPhysicalDeviceSurfacePresentModesKHR)(screen->pdev, surface, &present_modes_count, framebuffer->present_modes);
      framebuffer->present_modes_count = present_modes_count;

      VkDevice device = screen->dev;
      VkSemaphoreCreateInfo semaphore_create = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, NULL, 0};
      VkFenceCreateInfo fence_create = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
      for (int i = 0; i < maximum_buffers; ++i) {
         assert(framebuffer->image_available[i] == VK_NULL_HANDLE);
         result = VKSCR(CreateSemaphore)(device, &semaphore_create, NULL, &framebuffer->image_available[i]);
         assert(result == VK_SUCCESS);
         assert(framebuffer->draw_finished[i] == VK_NULL_HANDLE);
         result = VKSCR(CreateSemaphore)(device, &semaphore_create, NULL, &framebuffer->draw_finished[i]);
         assert(result == VK_SUCCESS);
         assert(framebuffer->present_done_fence[i] == VK_NULL_HANDLE);
         result = VKSCR(CreateFence)(device, &fence_create, NULL, &framebuffer->present_done_fence[i]);
         assert(result == VK_SUCCESS);
      }
   }

   uint32_t queue_family_indices[2] = {
      screen->graphics_queue_family,
      screen->present_queue_family
   };

   VkSurfaceCapabilitiesKHR capabilities = {0};
   VkResult result = VKSCR(GetPhysicalDeviceSurfaceCapabilitiesKHR)(screen->pdev, framebuffer->surface, &capabilities);
   assert(result == VK_SUCCESS);

   VkBool32 supports_present = VK_FALSE;
   result = VKSCR(GetPhysicalDeviceSurfaceSupportKHR)(screen->pdev, screen->present_queue_family, framebuffer->surface, &supports_present);
   assert(result == VK_SUCCESS);
   assert(supports_present == VK_TRUE);

   // HACK: Hard-code usage here to match that returned by
   // get_image_usage_for_feats() in zink_resource.c.  The latter usage is
   // what is stored in the Zink object that tracks usage for later
   // operations but must match when the swapchain images are created.
   VkImageUsageFlags usage =
      VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_TRANSFER_DST_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT |
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
      VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT
   ;

   VkSwapchainCreateInfoKHR info = {0};
   info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
   info.pNext = NULL;
   info.flags = 0;
   info.surface = framebuffer->surface;
   info.minImageCount = 2;
   info.imageFormat = format;
   info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
   info.imageExtent.width = width;
   info.imageExtent.height = height;
   info.imageArrayLayers = 1;
   info.imageUsage = usage;
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

   uint32_t images_count = maximum_buffers;
   VkImage images [maximum_buffers] = {0};
   VKSCR(GetSwapchainImagesKHR)(device, swapchain, &images_count, images);

   for (uint32_t i = 0; i < images_count; ++i) {
      assert(images[i] != VK_NULL_HANDLE);

      struct winsys_handle handle = {0};
      handle.type = WINSYS_HANDLE_TYPE_VK_RES;
      handle.format = template->format;
      handle.vulkan_handle = (intptr_t) images[i];

      struct pipe_resource templ;
      memset(&templ, 0, sizeof(templ));
      templ.target = PIPE_TEXTURE_2D;
      templ.format = framebuffer->pformat;
      templ.width0 = width;
      templ.height0 = height;
      templ.depth0 = 1;
      templ.array_size = 1;
      templ.nr_samples = 1;
      templ.last_level = 0;
      templ.bind = PIPE_BIND_DISPLAY_TARGET | PIPE_BIND_RENDER_TARGET | PIPE_BIND_SCANOUT;
      templ.usage = PIPE_USAGE_DEFAULT;
      templ.flags = 0;

      struct pipe_screen* pscreen = &framebuffer->screen->base;
      pipe_resource_reference(
         &framebuffer->buffers[i],
         pscreen->resource_from_handle(pscreen, &templ, &handle, PIPE_HANDLE_USAGE_FRAMEBUFFER_WRITE)
      );
   }

   if (framebuffer->swapchain != VK_NULL_HANDLE)
   {
      VKSCR(DestroySwapchainKHR)(device, framebuffer->swapchain, NULL);
      framebuffer->swapchain = VK_NULL_HANDLE;
   }

   framebuffer->swapchain = swapchain;
   framebuffer->width = width;
   framebuffer->height = height;

   zink_framebuffer_acquire_next_image(framebuffer);
}

static boolean
zink_wgl_framebuffer_present(struct stw_winsys_framebuffer* fb)
{
   struct zink_wgl_framebuffer* framebuffer = zink_wgl_framebuffer(fb);
   if (!framebuffer->swapchain) {
      debug_printf("zink: Cannot present; no swapchain");
      return false;
   }

   struct zink_screen* screen = framebuffer->screen;
   // if (stw_dev->swap_interval < 1)
   //    return S_OK == framebuffer->swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
   // else
   //     return S_OK == framebuffer->swapchain->Present(stw_dev->swap_interval, 0);

   VkSemaphore draw_finished = zink_framebuffer_draw_finished(framebuffer);

   VkResult results[1] = {VK_SUCCESS};
   VkPresentInfoKHR info = {0};
   info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
   info.pNext = NULL;
   info.waitSemaphoreCount = 1;
   info.pWaitSemaphores = &draw_finished;
   info.swapchainCount = 1;
   info.pSwapchains = &framebuffer->swapchain;
   info.pImageIndices = &framebuffer->acquired_image;
   info.pResults = results;

   VkQueue queue = framebuffer->screen->present_queue;
   assert(framebuffer->screen->present_queue == framebuffer->screen->queue);
   VkResult result = VKSCR(QueuePresentKHR)(queue, &info);
   assert(result == VK_SUCCESS);
   assert(results[0] == VK_SUCCESS);
   zink_framebuffer_acquire_next_image(framebuffer);
   return true;
}

static struct pipe_resource *
zink_wgl_framebuffer_get_resource(struct stw_winsys_framebuffer *pframebuffer,
                                   enum st_attachment_type statt)
{
   struct zink_wgl_framebuffer *framebuffer = zink_wgl_framebuffer(pframebuffer);
   struct zink_screen *screen = framebuffer->screen;
   struct pipe_screen *pscreen = &framebuffer->screen->base;

   if (framebuffer->swapchain == VK_NULL_HANDLE)
      return NULL;

   // TODO: What does the following do wrt. D3D12?  I guess that's returning
   // the front buffer instead of (one) of the back buffer(s).  Not sure if
   // that's valid for Vulkan but I'll see what I can do.
   // if (statt == ST_ATTACHMENT_FRONT_LEFT)
   //    index = !index;

   int index = framebuffer->acquired_image;
   if (index >= 0)
   {
      struct pipe_resource *resource = framebuffer->buffers[index];
      assert(resource);
      pipe_reference(NULL, &resource->reference);
      return resource;
   }
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

VkSemaphore zink_framebuffer_present_finished(struct zink_wgl_framebuffer* framebuffer)
{
   int frame = framebuffer->frame % maximum_buffers;
   return framebuffer->image_available[frame];
}

VkSemaphore zink_framebuffer_draw_finished(struct zink_wgl_framebuffer* framebuffer)
{
   int frame = framebuffer->frame % maximum_buffers;
   return framebuffer->draw_finished[frame];
}
