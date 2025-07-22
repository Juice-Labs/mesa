#include "zink_misc_ext.h"
#include "zink_screen.h"
#include "util/u_memory.h"
#include "util/u_debug.h"
#include "vk_enum_to_str.h"

#ifdef _WIN32
#include <string.h>
#endif

static struct zink_gpu_info *gpu_list = NULL;
static uint32_t gpu_count = 0;
static bool gpu_enum_initialized = false;

#ifdef _WIN32
/* Affinity DC management */
#define MAX_AFFINITY_DCS 16
static struct zink_affinity_dc affinity_dcs[MAX_AFFINITY_DCS];
static uint32_t next_affinity_dc_handle = 1;
#endif

bool zink_misc_init_gpu_enum(struct zink_screen *screen)
{
   VkResult result;
   VkPhysicalDevice *pdevs = NULL;
   
   if (gpu_enum_initialized) {
      return true;
   }
   
   if (!screen || !screen->instance) {
      debug_printf("ZINK: Invalid screen or Vulkan instance\n");
      return false;
   }
   
   /* Enumerate physical devices */
   result = VKSCR(EnumeratePhysicalDevices)(screen->instance, &gpu_count, NULL);
   if (result != VK_SUCCESS || gpu_count == 0) {
      debug_printf("ZINK: vkEnumeratePhysicalDevices failed (%s) or no devices found\n", 
                   vk_Result_to_str(result));
      return false;
   }
   
   pdevs = MALLOC(sizeof(VkPhysicalDevice) * gpu_count);
   if (!pdevs) {
      debug_printf("ZINK: Failed to allocate memory for physical devices\n");
      return false;
   }
   
   result = VKSCR(EnumeratePhysicalDevices)(screen->instance, &gpu_count, pdevs);
   if (result != VK_SUCCESS) {
      debug_printf("ZINK: vkEnumeratePhysicalDevices failed (%s)\n", vk_Result_to_str(result));
      FREE(pdevs);
      return false;
   }
   
   /* Allocate GPU info array */
   gpu_list = CALLOC(gpu_count, sizeof(struct zink_gpu_info));
   if (!gpu_list) {
      debug_printf("ZINK: Failed to allocate memory for GPU list\n");
      FREE(pdevs);
      return false;
   }
   
   /* Initialize GPU info structures */
   for (uint32_t i = 0; i < gpu_count; i++) {
      gpu_list[i].pdev = pdevs[i];
#ifdef _WIN32
      gpu_list[i].handle = (HGPUNV)(uintptr_t)(i + 1); /* Handle cannot be 0/NULL */
#else
      gpu_list[i].handle = (void*)(uintptr_t)(i + 1);
#endif
      
      /* Get device properties */
      VKSCR(GetPhysicalDeviceProperties)(pdevs[i], &gpu_list[i].props);
      
      debug_printf("ZINK: GPU %u: %s (vendor: 0x%04x, device: 0x%04x)\n",
                   i, gpu_list[i].props.deviceName,
                   gpu_list[i].props.vendorID, gpu_list[i].props.deviceID);
   }
   
   FREE(pdevs);
   gpu_enum_initialized = true;
   
   debug_printf("ZINK: Successfully enumerated %u GPUs\n", gpu_count);
   return true;
}

void zink_misc_cleanup_gpu_enum(void)
{
   if (gpu_list) {
      FREE(gpu_list);
      gpu_list = NULL;
   }
   
   gpu_count = 0;
   gpu_enum_initialized = false;
   
#ifdef _WIN32
   /* Cleanup any remaining affinity DCs */
   for (int i = 0; i < MAX_AFFINITY_DCS; i++) {
      if (affinity_dcs[i].handle != NULL) {
         FREE(affinity_dcs[i].gpu_list);
         affinity_dcs[i].handle = NULL;
         affinity_dcs[i].gpu_count = 0;
         affinity_dcs[i].gpu_list = NULL;
      }
   }
   next_affinity_dc_handle = 1;
#endif
}

#ifdef _WIN32
bool zink_misc_enum_gpus(uint32_t gpu_index, HGPUNV *gpu_handle)
{
   if (!gpu_enum_initialized) {
      debug_printf("ZINK: GPU enumeration not initialized\n");
      return false;
   }
   
   if (!gpu_handle) {
      debug_printf("ZINK: NULL gpu_handle pointer\n");
      return false;
   }
   
   if (gpu_index >= gpu_count) {
      /* Index out of range - this is expected behavior when enumerating */
      return false;
   }
   
   *gpu_handle = gpu_list[gpu_index].handle;
   return true;
}

struct zink_gpu_info* zink_misc_get_gpu_info(HGPUNV gpu_handle)
{
   if (!gpu_enum_initialized || !gpu_handle) {
      return NULL;
   }
   
   /* Convert handle back to index */
   uint32_t index = (uint32_t)(uintptr_t)gpu_handle - 1;
   
   if (index >= gpu_count) {
      debug_printf("ZINK: Invalid GPU handle: %p\n", gpu_handle);
      return NULL;
   }
   
   return &gpu_list[index];
}

bool zink_misc_enum_gpu_devices(HGPUNV gpu_handle, uint32_t device_index, PGPU_DEVICE gpu_device)
{
   if (!gpu_enum_initialized || !gpu_handle || !gpu_device) {
      debug_printf("ZINK: GPU enumeration not initialized or invalid parameters\n");
      return false;
   }

   /* Only support device index 0 (the GPU itself) */
   if (device_index != 0) {
      return false;
   }

   struct zink_gpu_info* gpu_info = zink_misc_get_gpu_info(gpu_handle);
   if (!gpu_info) {
      return false;
   }

   /* Fill GPU_DEVICE structure */
   memset(gpu_device, 0, sizeof(GPU_DEVICE));
   gpu_device->cb = sizeof(GPU_DEVICE);
   
   /* Use device name from Vulkan properties */
   strncpy(gpu_device->DeviceName, gpu_info->props.deviceName, sizeof(gpu_device->DeviceName) - 1);
   gpu_device->DeviceName[sizeof(gpu_device->DeviceName) - 1] = '\0';
   
   /* Create device description string with vendor and device info */
   snprintf(gpu_device->DeviceString, sizeof(gpu_device->DeviceString) - 1,
            "Vulkan GPU: %s (VendorID: 0x%04x, DeviceID: 0x%04x)",
            gpu_info->props.deviceName,
            gpu_info->props.vendorID,
            gpu_info->props.deviceID);
   gpu_device->DeviceString[sizeof(gpu_device->DeviceString) - 1] = '\0';
   
   /* Set basic flags - mark as display device */
   gpu_device->Flags = 0x1; /* DISPLAY_DEVICE_ATTACHED_TO_DESKTOP equivalent */
   
   /* Set virtual screen to cover primary monitor */
   gpu_device->rcVirtualScreen.left = 0;
   gpu_device->rcVirtualScreen.top = 0;
   gpu_device->rcVirtualScreen.right = GetSystemMetrics(SM_CXSCREEN);
   gpu_device->rcVirtualScreen.bottom = GetSystemMetrics(SM_CYSCREEN);
   
   debug_printf("ZINK: Enumerated GPU device %u: %s\n", device_index, gpu_device->DeviceName);
   return true;
}
#endif

uint32_t zink_misc_get_gpu_count(void)
{
   if (!gpu_enum_initialized) {
      return 0;
   }
   
   return gpu_count;
}

#ifdef _WIN32

HDC zink_misc_create_affinity_dc(const HGPUNV *gpu_list)
{
   if (!gpu_enum_initialized || !gpu_list) {
      debug_printf("ZINK: GPU enumeration not initialized or invalid GPU list\n");
      return NULL;
   }

   /* Find free slot */
   int free_slot = -1;
   for (int i = 0; i < MAX_AFFINITY_DCS; i++) {
      if (affinity_dcs[i].handle == NULL) {
         free_slot = i;
         break;
      }
   }

   if (free_slot == -1) {
      debug_printf("ZINK: No free affinity DC slots\n");
      return NULL;
   }

   /* Count GPUs in the list (NULL-terminated) */
   uint32_t count = 0;
   while (gpu_list[count] != NULL) {
      count++;
   }

   if (count == 0) {
      debug_printf("ZINK: Empty GPU list for affinity DC\n");
      return NULL;
   }

   /* Allocate and copy GPU list */
   HGPUNV *gpu_copy = CALLOC(count, sizeof(HGPUNV));
   if (!gpu_copy) {
      debug_printf("ZINK: Failed to allocate GPU list copy\n");
      return NULL;
   }

   memcpy(gpu_copy, gpu_list, count * sizeof(HGPUNV));

   /* Generate unique handle */
   HDC handle = (HDC)(uintptr_t)(0x1000 + next_affinity_dc_handle++);

   /* Store affinity DC */
   affinity_dcs[free_slot].handle = handle;
   affinity_dcs[free_slot].gpu_count = count;
   affinity_dcs[free_slot].gpu_list = gpu_copy;

   debug_printf("ZINK: Created affinity DC %p with %u GPUs\n", handle, count);
   return handle;
}

bool zink_misc_delete_affinity_dc(HDC hdc)
{
   if (!hdc) {
      return false;
   }

   /* Find the affinity DC */
   for (int i = 0; i < MAX_AFFINITY_DCS; i++) {
      if (affinity_dcs[i].handle == hdc) {
         /* Free the GPU list */
         FREE(affinity_dcs[i].gpu_list);
         
         /* Clear the slot */
         affinity_dcs[i].handle = NULL;
         affinity_dcs[i].gpu_count = 0;
         affinity_dcs[i].gpu_list = NULL;
         
         debug_printf("ZINK: Deleted affinity DC %p\n", hdc);
         return true;
      }
   }

   debug_printf("ZINK: Affinity DC %p not found\n", hdc);
   return false;
}

bool zink_misc_enum_gpus_from_affinity_dc(HDC hdc, uint32_t gpu_index, HGPUNV *gpu_handle)
{
   if (!hdc || !gpu_handle) {
      return false;
   }

   /* Find the affinity DC */
   for (int i = 0; i < MAX_AFFINITY_DCS; i++) {
      if (affinity_dcs[i].handle == hdc) {
         if (gpu_index >= affinity_dcs[i].gpu_count) {
            /* Index out of range */
            return false;
         }

         *gpu_handle = affinity_dcs[i].gpu_list[gpu_index];
         return true;
      }
   }

   debug_printf("ZINK: Affinity DC %p not found\n", hdc);
   return false;
}

#endif 