#pragma once

#include "../Curier.h"

#include <cstddef>

extern "C" {
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
}

#if __has_include("freertos/idf_additions.h")
extern "C" {
#include "freertos/idf_additions.h"
}
#define CURIER_HAS_IDF_TASK_CAPS 1
#else
#define CURIER_HAS_IDF_TASK_CAPS 0
#endif

#if CURIER_HAS_IDF_TASK_CAPS && defined(configSUPPORT_STATIC_ALLOCATION) &&                        \
    (configSUPPORT_STATIC_ALLOCATION == 1) && defined(MALLOC_CAP_SPIRAM)
#define CURIER_CAN_USE_EXTERNAL_STACKS 1
#else
#define CURIER_CAN_USE_EXTERNAL_STACKS 0
#endif

namespace curier_internal::task {

constexpr size_t kMinStackSizeBytes = 1024;

inline bool validStackSize(size_t stackBytes) {
	return stackBytes >= kMinStackSizeBytes && (stackBytes % sizeof(StackType_t)) == 0;
}

inline bool externalStackSupported() {
#if CURIER_CAN_USE_EXTERNAL_STACKS
	return heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0;
#else
	return false;
#endif
}

inline BaseType_t create(
    TaskFunction_t entry,
    const char *name,
    size_t stackBytes,
    void *argument,
    UBaseType_t priority,
    TaskHandle_t *handle,
    BaseType_t coreId,
    bool usePsram,
    bool &createdWithCaps
) {
	createdWithCaps = false;
	if (!validStackSize(stackBytes)) {
		return pdFAIL;
	}
	if (usePsram) {
#if CURIER_CAN_USE_EXTERNAL_STACKS
		if (!externalStackSupported()) {
			return pdFAIL;
		}
		const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
		    entry,
		    name,
		    static_cast<configSTACK_DEPTH_TYPE>(stackBytes),
		    argument,
		    priority,
		    handle,
		    coreId,
		    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
		);
		createdWithCaps = created == pdPASS;
		return created;
#else
		return pdFAIL;
#endif
	}
	if (coreId == tskNO_AFFINITY) {
		return xTaskCreate(
		    entry,
		    name,
		    static_cast<uint32_t>(stackBytes),
		    argument,
		    priority,
		    handle
		);
	}
	return xTaskCreatePinnedToCore(
	    entry,
	    name,
	    static_cast<uint32_t>(stackBytes),
	    argument,
	    priority,
	    handle,
	    coreId
	);
}

inline void deleteCurrent(bool createdWithCaps) {
#if CURIER_CAN_USE_EXTERNAL_STACKS
	if (createdWithCaps) {
		vTaskDeleteWithCaps(xTaskGetCurrentTaskHandle());
		return;
	}
#else
	(void)createdWithCaps;
#endif
	vTaskDelete(nullptr);
}

} // namespace curier_internal::task
