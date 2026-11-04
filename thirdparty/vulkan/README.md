# Vulkan headers

`include/vulkan/vulkan_core.h`, `vk_platform.h` and `include/vk_video/*.h` come from the Khronos Vulkan-Headers
(version 1.4.363, as shipped with the LunarG Vulkan SDK). License: Apache-2.0 OR MIT (see the SPDX line in each file).

Only the declarations are used (`VK_NO_PROTOTYPES`): nothing links against Vulkan. The backend
(`vk/w3dVkGL.cpp`) asks the system loader (vulkan-1.dll / libvulkan.so.1, opened by SDL) for every function at
startup, so the editor still runs on machines without Vulkan.
