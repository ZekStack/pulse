#include "host_freertos.h"

#include <esp_timer.h>
#include <freertos/event_groups.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

struct HostTask {
	std::thread thread;
	std::mutex waitMutex;
	std::condition_variable *waitCv = nullptr;
	std::atomic<bool> abortWait{false};
	std::mutex notificationMutex;
	std::condition_variable notificationCv;
	uint32_t notificationCount = 0;
};

struct HostQueue {
	std::mutex mutex;
	std::condition_variable cv;
	std::deque<std::vector<uint8_t>> items;
	size_t capacity = 0;
	size_t itemSize = 0;
	bool deleted = false;
};

struct HostRecursiveMutex {
	std::recursive_timed_mutex mutex;
};

struct HostEventGroup {
	std::mutex mutex;
	std::condition_variable cv;
	EventBits_t bits = 0;
	bool deleted = false;
};

namespace {
std::atomic<int64_t> fakeTimeUs{0};
std::atomic<uint32_t> stackHighWaterBytes{777};
thread_local HostTask *currentTask = nullptr;
std::mutex registryMutex;
std::vector<HostQueue *> queues;
std::vector<HostEventGroup *> eventGroups;

void notifyAllWaiters() {
	std::lock_guard<std::mutex> lock(registryMutex);
	for (HostQueue *queue : queues) {
		if (queue != nullptr) {
			queue->cv.notify_all();
		}
	}
	for (HostEventGroup *event : eventGroups) {
		if (event != nullptr) {
			event->cv.notify_all();
		}
	}
}

bool shouldAbortCurrentWait() {
	return currentTask != nullptr && currentTask->abortWait.load(std::memory_order_acquire);
}

void setCurrentWaitCv(std::condition_variable *cv) {
	if (currentTask == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(currentTask->waitMutex);
	currentTask->waitCv = cv;
}

void clearCurrentWaitCv() {
	if (currentTask == nullptr) {
		return;
	}
	std::lock_guard<std::mutex> lock(currentTask->waitMutex);
	currentTask->waitCv = nullptr;
	currentTask->abortWait.store(false, std::memory_order_release);
}
} // namespace

void host_reset_time() {
	fakeTimeUs.store(0, std::memory_order_release);
	notifyAllWaiters();
}

void host_advance_time_ms(uint64_t milliseconds) {
	fakeTimeUs.fetch_add(static_cast<int64_t>(milliseconds * 1000), std::memory_order_acq_rel);
	notifyAllWaiters();
}

void host_set_stack_high_water_mark(uint32_t bytes) {
	stackHighWaterBytes.store(bytes, std::memory_order_release);
}

extern "C" int64_t esp_timer_get_time(void) {
	return fakeTimeUs.load(std::memory_order_acquire);
}

extern "C" SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
	return new (std::nothrow) HostRecursiveMutex();
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t handle) {
	delete handle;
}

extern "C" BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t handle, TickType_t timeout) {
	if (handle == nullptr) {
		return pdFALSE;
	}
	if (timeout == portMAX_DELAY) {
		handle->mutex.lock();
		return pdTRUE;
	}
	return handle->mutex.try_lock_for(std::chrono::milliseconds(timeout)) ? pdTRUE : pdFALSE;
}

extern "C" BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
	if (handle == nullptr) {
		return pdFALSE;
	}
	handle->mutex.unlock();
	return pdTRUE;
}

extern "C" QueueHandle_t xQueueCreate(UBaseType_t capacity, UBaseType_t itemSize) {
	HostQueue *queue = new (std::nothrow) HostQueue();
	if (queue == nullptr) {
		return nullptr;
	}
	queue->capacity = capacity;
	queue->itemSize = itemSize;
	{
		std::lock_guard<std::mutex> lock(registryMutex);
		queues.push_back(queue);
	}
	return queue;
}

extern "C" void vQueueDelete(QueueHandle_t queue) {
	if (queue == nullptr) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(queue->mutex);
		queue->deleted = true;
	}
	queue->cv.notify_all();
	{
		std::lock_guard<std::mutex> lock(registryMutex);
		queues.erase(std::remove(queues.begin(), queues.end(), queue), queues.end());
	}
	delete queue;
}

extern "C" BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t timeout) {
	if (queue == nullptr || item == nullptr) {
		return pdFALSE;
	}
	std::unique_lock<std::mutex> lock(queue->mutex);
	auto available = [&]() { return queue->deleted || queue->items.size() < queue->capacity; };
	if (!available()) {
		if (timeout == 0) {
			return pdFALSE;
		}
		if (timeout == portMAX_DELAY) {
			queue->cv.wait(lock, available);
		} else if (!queue->cv.wait_for(lock, std::chrono::milliseconds(timeout), available)) {
			return pdFALSE;
		}
	}
	if (queue->deleted || queue->items.size() >= queue->capacity) {
		return pdFALSE;
	}
	std::vector<uint8_t> bytes(queue->itemSize);
	std::memcpy(bytes.data(), item, queue->itemSize);
	queue->items.push_back(std::move(bytes));
	lock.unlock();
	queue->cv.notify_all();
	return pdTRUE;
}

extern "C" BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t timeout) {
	if (queue == nullptr || item == nullptr) {
		return pdFALSE;
	}
	std::unique_lock<std::mutex> lock(queue->mutex);
	const int64_t startUs = fakeTimeUs.load(std::memory_order_acquire);
	const int64_t deadlineUs = timeout == portMAX_DELAY ? INT64_MAX :
	                                                       startUs +
	                                                           static_cast<int64_t>(timeout) * 1000;
	setCurrentWaitCv(&queue->cv);
	auto ready = [&]() {
		return queue->deleted || !queue->items.empty() || shouldAbortCurrentWait() ||
		       fakeTimeUs.load(std::memory_order_acquire) >= deadlineUs;
	};
	while (!ready()) {
		queue->cv.wait(lock);
	}
	clearCurrentWaitCv();
	if (queue->deleted || queue->items.empty()) {
		return pdFALSE;
	}
	std::vector<uint8_t> bytes = std::move(queue->items.front());
	queue->items.pop_front();
	std::memcpy(item, bytes.data(), queue->itemSize);
	lock.unlock();
	queue->cv.notify_all();
	return pdTRUE;
}

extern "C" UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
	if (queue == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(queue->mutex);
	return static_cast<UBaseType_t>(queue->items.size());
}

extern "C" EventGroupHandle_t xEventGroupCreate(void) {
	HostEventGroup *event = new (std::nothrow) HostEventGroup();
	if (event == nullptr) {
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(registryMutex);
		eventGroups.push_back(event);
	}
	return event;
}

extern "C" void vEventGroupDelete(EventGroupHandle_t event) {
	if (event == nullptr) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(event->mutex);
		event->deleted = true;
	}
	event->cv.notify_all();
	{
		std::lock_guard<std::mutex> lock(registryMutex);
		eventGroups.erase(std::remove(eventGroups.begin(), eventGroups.end(), event), eventGroups.end());
	}
	delete event;
}

extern "C" EventBits_t xEventGroupSetBits(EventGroupHandle_t event, EventBits_t bits) {
	if (event == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(event->mutex);
	event->bits |= bits;
	event->cv.notify_all();
	return event->bits;
}

extern "C" EventBits_t xEventGroupClearBits(EventGroupHandle_t event, EventBits_t bits) {
	if (event == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(event->mutex);
	const EventBits_t previous = event->bits;
	event->bits &= ~bits;
	return previous;
}

extern "C" EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t event,
    EventBits_t bits,
    BaseType_t clearOnExit,
    BaseType_t waitForAll,
    TickType_t timeout
) {
	if (event == nullptr) {
		return 0;
	}
	std::unique_lock<std::mutex> lock(event->mutex);
	const int64_t startUs = fakeTimeUs.load(std::memory_order_acquire);
	const int64_t deadlineUs = timeout == portMAX_DELAY ? INT64_MAX :
	                                                       startUs +
	                                                           static_cast<int64_t>(timeout) * 1000;
	auto ready = [&]() {
		const bool matched = waitForAll == pdTRUE ? (event->bits & bits) == bits :
		                                             (event->bits & bits) != 0;
		return event->deleted || matched ||
		       fakeTimeUs.load(std::memory_order_acquire) >= deadlineUs;
	};
	while (!ready()) {
		event->cv.wait(lock);
	}
	EventBits_t result = event->bits;
	const bool matched = waitForAll == pdTRUE ? (result & bits) == bits : (result & bits) != 0;
	if (matched && clearOnExit == pdTRUE) {
		event->bits &= ~bits;
	}
	return result;
}

extern "C" BaseType_t xTaskCreate(
    TaskFunction_t entry,
    const char *,
    uint32_t,
    void *arg,
    UBaseType_t,
    TaskHandle_t *out
) {
	if (entry == nullptr || out == nullptr) {
		return pdFAIL;
	}
	HostTask *task = new (std::nothrow) HostTask();
	if (task == nullptr) {
		return pdFAIL;
	}
	try {
		task->thread = std::thread([task, entry, arg]() {
			currentTask = task;
			entry(arg);
		});
	} catch (...) {
		delete task;
		return pdFAIL;
	}
	*out = task;
	return pdPASS;
}

extern "C" BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry,
    const char *name,
    uint32_t stack,
    void *arg,
    UBaseType_t priority,
    TaskHandle_t *out,
    BaseType_t
) {
	return xTaskCreate(entry, name, stack, arg, priority, out);
}

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
	return currentTask;
}

extern "C" void vTaskDelete(TaskHandle_t handle) {
	HostTask *task = handle != nullptr ? handle : currentTask;
	if (task == nullptr) {
		return;
	}
	if (task == currentTask) {
		if (task->thread.joinable()) {
			task->thread.detach();
		}
		currentTask = nullptr;
		delete task;
		return;
	}
	if (task->thread.joinable()) {
		task->thread.join();
	}
	delete task;
}

extern "C" void vTaskDelay(TickType_t ticks) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

extern "C" BaseType_t xTaskAbortDelay(TaskHandle_t task) {
	if (task == nullptr) {
		return pdFALSE;
	}
	std::condition_variable *cv = nullptr;
	{
		std::lock_guard<std::mutex> lock(task->waitMutex);
		task->abortWait.store(true, std::memory_order_release);
		cv = task->waitCv;
	}
	if (cv != nullptr) {
		cv->notify_all();
	}
	return pdTRUE;
}


extern "C" BaseType_t xTaskNotifyGive(TaskHandle_t task) {
	if (task == nullptr) {
		return pdFALSE;
	}
	{
		std::lock_guard<std::mutex> lock(task->notificationMutex);
		task->notificationCount++;
	}
	task->notificationCv.notify_all();
	return pdTRUE;
}

extern "C" uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t timeout) {
	HostTask *task = currentTask;
	if (task == nullptr) {
		return 0;
	}
	std::unique_lock<std::mutex> lock(task->notificationMutex);
	const int64_t startUs = fakeTimeUs.load(std::memory_order_acquire);
	const int64_t deadlineUs = timeout == portMAX_DELAY ? INT64_MAX :
	                                                       startUs +
	                                                           static_cast<int64_t>(timeout) * 1000;
	{
		std::lock_guard<std::mutex> registryLock(registryMutex);
		// Fake-time advancement notifies task waiters through notifyAllWaiters below.
	}
	auto ready = [&]() {
		return task->notificationCount > 0 ||
		       fakeTimeUs.load(std::memory_order_acquire) >= deadlineUs;
	};
	while (!ready()) {
		task->notificationCv.wait_for(lock, std::chrono::milliseconds(1));
	}
	const uint32_t count = task->notificationCount;
	if (count > 0) {
		if (clearOnExit == pdTRUE) {
			task->notificationCount = 0;
		} else {
			task->notificationCount--;
		}
	}
	return count;
}

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
	return stackHighWaterBytes.load(std::memory_order_acquire);
}
