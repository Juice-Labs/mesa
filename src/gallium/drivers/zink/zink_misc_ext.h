#ifndef ZINK_MISC_EXT_H
#define ZINK_MISC_EXT_H

#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef _WIN32
#include <windows.h>
#ifndef HGPUNV
DECLARE_HANDLE(HGPUNV);
#endif

/* GPU_DEVICE structure for WGL_NV_gpu_affinity */
#ifndef PGPU_DEVICE
typedef struct _GPU_DEVICE {
  DWORD cb; 
  CHAR DeviceName[32]; 
  CHAR DeviceString[128]; 
  DWORD Flags; 
  RECT rcVirtualScreen; 
} GPU_DEVICE, *PGPU_DEVICE;
#endif
#endif

struct zink_screen;

#ifdef __cplusplus
extern "C" {
#endif

/* GPU enumeration for WGL_NV_gpu_affinity */
struct zink_gpu_info {
   VkPhysicalDevice pdev;
   VkPhysicalDeviceProperties props;
#ifdef _WIN32
   HGPUNV handle;
#else
   void* handle;
#endif
};

/* Initialize GPU enumeration system */
bool zink_misc_init_gpu_enum(struct zink_screen *screen);

/* Cleanup GPU enumeration system */
void zink_misc_cleanup_gpu_enum(void);

/* Get GPU count */
uint32_t zink_misc_get_gpu_count(void);

/* Windows-specific functions for WGL_NV_gpu_affinity */
#ifdef _WIN32
/* Enumerate GPUs for WGL_NV_gpu_affinity */
bool zink_misc_enum_gpus(uint32_t gpu_index, HGPUNV *gpu_handle);

/* Get GPU info by handle */
struct zink_gpu_info* zink_misc_get_gpu_info(HGPUNV gpu_handle);

/* Enumerate GPU devices for a specific GPU */
bool zink_misc_enum_gpu_devices(HGPUNV gpu_handle, uint32_t device_index, PGPU_DEVICE gpu_device);

/* Affinity DC management */
struct zink_affinity_dc {
   HDC handle;
   uint32_t gpu_count;
   HGPUNV *gpu_list;
};

/* Create affinity DC */
HDC zink_misc_create_affinity_dc(const HGPUNV *gpu_list);

/* Delete affinity DC */
bool zink_misc_delete_affinity_dc(HDC hdc);

/* Enumerate GPUs from affinity DC */
bool zink_misc_enum_gpus_from_affinity_dc(HDC hdc, uint32_t gpu_index, HGPUNV *gpu_handle);

#endif

#ifdef __cplusplus
}
#endif

#endif /* ZINK_MISC_EXT_H */ 