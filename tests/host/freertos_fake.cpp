#include "fake_freertos_test.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

struct FakeTaskControl {
	std::mutex mutex;
	std::condition_variable condition;
	uint32_t notifications = 0;
};

struct FakeQueue {
	std::mutex mutex;
	std::condition_variable condition;
	std::deque<std::vector<uint8_t>> items;
	size_t capacity = 0;
	size_t itemSize = 0;
};

struct FakeSemaphore {
	std::recursive_timed_mutex mutex;
};

struct FakeEventGroup {
	std::mutex mutex;
	std::condition_variable condition;
	EventBits_t bits = 0;
};

namespace {
struct TaskExit final : std::exception {
};

thread_local FakeTaskControl *currentTask = nullptr;
thread_local bool holdCurrentEventWaiter = false;
std::mutex tasksMutex;
std::set<FakeTaskControl *> tasks;
std::atomic<int64_t> timeOffsetUs{0};
std::atomic<size_t> stackHighWaterMark{777};
std::atomic<bool> notificationsSuspended{false};
const auto clockStart = std::chrono::steady_clock::now();

std::mutex heldEventMutex;
std::condition_variable heldEventCondition;
uint32_t heldEventWaiterCount = 0;
bool releaseHeldEventWaiters = false;

BaseType_t createTask(TaskFunction_t entry, void *arg, TaskHandle_t *handle) {
	if (entry == nullptr || handle == nullptr) {
		return pdFAIL;
	}
	auto *task = new (std::nothrow) FakeTaskControl();
	if (task == nullptr) {
		return pdFAIL;
	}
	{
		std::lock_guard<std::mutex> lock(tasksMutex);
		tasks.insert(task);
	}
	*handle = task;
	std::thread([task, entry, arg]() {
		currentTask = task;
		try {
			entry(arg);
		} catch (const TaskExit &) {
		}
		currentTask = nullptr;
		{
			std::lock_guard<std::mutex> lock(tasksMutex);
			tasks.erase(task);
		}
		delete task;
	}).detach();
	return pdPASS;
}

void notifyAllTaskWaiters() {
	std::lock_guard<std::mutex> lock(tasksMutex);
	for (FakeTaskControl *task : tasks) {
		task->condition.notify_all();
	}
}
} // namespace

extern "C" BaseType_t xTaskCreate(
    TaskFunction_t entry,
    const char *,
    uint32_t,
    void *arg,
    UBaseType_t,
    TaskHandle_t *handle
) {
	return createTask(entry, arg, handle);
}

extern "C" BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t entry,
    const char *,
    uint32_t,
    void *arg,
    UBaseType_t,
    TaskHandle_t *handle,
    BaseType_t
) {
	return createTask(entry, arg, handle);
}

extern "C" void vTaskDelete(TaskHandle_t handle) {
	if (handle == nullptr || handle == currentTask) {
		throw TaskExit();
	}
}

extern "C" void vTaskDelay(TickType_t ticks) {
	if (ticks == portMAX_DELAY) {
		std::this_thread::sleep_for(24h);
		return;
	}
	std::this_thread::sleep_for(std::chrono::milliseconds(ticks));
}

extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) {
	return currentTask;
}

extern "C" BaseType_t xTaskNotifyGive(TaskHandle_t handle) {
	if (handle == nullptr) {
		return pdFAIL;
	}
	{
		std::lock_guard<std::mutex> lock(handle->mutex);
		handle->notifications++;
	}
	handle->condition.notify_all();
	return pdPASS;
}

extern "C" uint32_t ulTaskNotifyTake(BaseType_t clearOnExit, TickType_t ticksToWait) {
	FakeTaskControl *task = currentTask;
	if (task == nullptr) {
		return 0;
	}
	std::unique_lock<std::mutex> lock(task->mutex);
	auto ready = [task]() {
		return task->notifications > 0 && !notificationsSuspended.load();
	};
	if (!ready()) {
		if (ticksToWait == portMAX_DELAY) {
			task->condition.wait(lock, ready);
		} else {
			task->condition.wait_for(lock, std::chrono::milliseconds(ticksToWait), ready);
		}
	}
	if (!ready()) {
		return 0;
	}
	const uint32_t result = task->notifications;
	if (clearOnExit == pdTRUE) {
		task->notifications = 0;
	} else {
		task->notifications--;
	}
	return result;
}

extern "C" UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) {
	return static_cast<UBaseType_t>(stackHighWaterMark.load());
}

extern "C" QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t itemSize) {
	auto *queue = new (std::nothrow) FakeQueue();
	if (queue == nullptr) {
		return nullptr;
	}
	queue->capacity = length;
	queue->itemSize = itemSize;
	return queue;
}

extern "C" BaseType_t xQueueSend(
    QueueHandle_t queue,
    const void *item,
    TickType_t ticksToWait
) {
	if (queue == nullptr || item == nullptr) {
		return pdFALSE;
	}
	std::unique_lock<std::mutex> lock(queue->mutex);
	auto hasCapacity = [queue]() { return queue->items.size() < queue->capacity; };
	if (!hasCapacity()) {
		if (ticksToWait == 0) {
			return pdFALSE;
		}
		if (ticksToWait == portMAX_DELAY) {
			queue->condition.wait(lock, hasCapacity);
		} else if (!queue->condition.wait_for(
		               lock,
		               std::chrono::milliseconds(ticksToWait),
		               hasCapacity
		           )) {
			return pdFALSE;
		}
	}
	std::vector<uint8_t> bytes(queue->itemSize);
	std::memcpy(bytes.data(), item, queue->itemSize);
	queue->items.push_back(std::move(bytes));
	lock.unlock();
	queue->condition.notify_all();
	return pdTRUE;
}

extern "C" BaseType_t xQueueReceive(
    QueueHandle_t queue,
    void *item,
    TickType_t ticksToWait
) {
	if (queue == nullptr || item == nullptr) {
		return pdFALSE;
	}
	std::unique_lock<std::mutex> lock(queue->mutex);
	auto hasItem = [queue]() { return !queue->items.empty(); };
	if (!hasItem()) {
		if (ticksToWait == 0) {
			return pdFALSE;
		}
		if (ticksToWait == portMAX_DELAY) {
			queue->condition.wait(lock, hasItem);
		} else if (!queue->condition.wait_for(
		               lock,
		               std::chrono::milliseconds(ticksToWait),
		               hasItem
		           )) {
			return pdFALSE;
		}
	}
	std::memcpy(item, queue->items.front().data(), queue->itemSize);
	queue->items.pop_front();
	lock.unlock();
	queue->condition.notify_all();
	return pdTRUE;
}

extern "C" UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue) {
	if (queue == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(queue->mutex);
	return static_cast<UBaseType_t>(queue->items.size());
}

extern "C" void vQueueDelete(QueueHandle_t queue) {
	delete queue;
}

extern "C" SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
	return new (std::nothrow) FakeSemaphore();
}

extern "C" BaseType_t xSemaphoreTakeRecursive(
    SemaphoreHandle_t semaphore,
    TickType_t ticksToWait
) {
	if (semaphore == nullptr) {
		return pdFALSE;
	}
	if (ticksToWait == portMAX_DELAY) {
		semaphore->mutex.lock();
		return pdTRUE;
	}
	if (ticksToWait == 0) {
		return semaphore->mutex.try_lock() ? pdTRUE : pdFALSE;
	}
	return semaphore->mutex.try_lock_for(std::chrono::milliseconds(ticksToWait)) ? pdTRUE :
	                                                                               pdFALSE;
}

extern "C" BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t semaphore) {
	if (semaphore == nullptr) {
		return pdFALSE;
	}
	semaphore->mutex.unlock();
	return pdTRUE;
}

extern "C" void vSemaphoreDelete(SemaphoreHandle_t semaphore) {
	delete semaphore;
}

extern "C" EventGroupHandle_t xEventGroupCreate(void) {
	return new (std::nothrow) FakeEventGroup();
}

extern "C" EventBits_t xEventGroupSetBits(
    EventGroupHandle_t eventGroup,
    EventBits_t bits
) {
	if (eventGroup == nullptr) {
		return 0;
	}
	{
		std::lock_guard<std::mutex> lock(eventGroup->mutex);
		eventGroup->bits |= bits;
	}
	eventGroup->condition.notify_all();
	return eventGroup->bits;
}

extern "C" EventBits_t xEventGroupClearBits(
    EventGroupHandle_t eventGroup,
    EventBits_t bits
) {
	if (eventGroup == nullptr) {
		return 0;
	}
	std::lock_guard<std::mutex> lock(eventGroup->mutex);
	const EventBits_t before = eventGroup->bits;
	eventGroup->bits &= ~bits;
	return before;
}

extern "C" EventBits_t xEventGroupWaitBits(
    EventGroupHandle_t eventGroup,
    EventBits_t bitsToWaitFor,
    BaseType_t clearOnExit,
    BaseType_t waitForAllBits,
    TickType_t ticksToWait
) {
	if (eventGroup == nullptr) {
		return 0;
	}
	std::unique_lock<std::mutex> lock(eventGroup->mutex);
	auto ready = [eventGroup, bitsToWaitFor, waitForAllBits]() {
		if (waitForAllBits == pdTRUE) {
			return (eventGroup->bits & bitsToWaitFor) == bitsToWaitFor;
		}
		return (eventGroup->bits & bitsToWaitFor) != 0;
	};
	if (!ready()) {
		if (ticksToWait == portMAX_DELAY) {
			eventGroup->condition.wait(lock, ready);
		} else {
			eventGroup->condition.wait_for(lock, std::chrono::milliseconds(ticksToWait), ready);
		}
	}
	const bool completed = ready();
	const EventBits_t result = eventGroup->bits;
	if (clearOnExit == pdTRUE && completed) {
		eventGroup->bits &= ~bitsToWaitFor;
	}
	lock.unlock();

	if (holdCurrentEventWaiter && completed) {
		std::unique_lock<std::mutex> heldLock(heldEventMutex);
		heldEventWaiterCount++;
		heldEventCondition.notify_all();
		heldEventCondition.wait(heldLock, []() { return releaseHeldEventWaiters; });
		heldEventWaiterCount--;
		holdCurrentEventWaiter = false;
	}
	return result;
}

extern "C" void vEventGroupDelete(EventGroupHandle_t eventGroup) {
	delete eventGroup;
}

extern "C" int64_t esp_timer_get_time(void) {
	const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - clockStart
	);
	return elapsed.count() + timeOffsetUs.load();
}

extern "C" size_t heap_caps_get_total_size(uint32_t) {
	return 0;
}

void fakeAdvanceTimeMs(uint64_t milliseconds) {
	timeOffsetUs.fetch_add(static_cast<int64_t>(milliseconds * 1000));
}

void fakeWakeAllTasks() {
	std::lock_guard<std::mutex> lock(tasksMutex);
	for (FakeTaskControl *task : tasks) {
		xTaskNotifyGive(task);
	}
}

void fakeSetStackHighWaterMark(size_t bytes) {
	stackHighWaterMark.store(bytes);
}

size_t fakeActiveTaskCount() {
	std::lock_guard<std::mutex> lock(tasksMutex);
	return tasks.size();
}

void fakeSuspendTaskNotifications() {
	notificationsSuspended.store(true);
}

void fakeResumeTaskNotifications() {
	notificationsSuspended.store(false);
	notifyAllTaskWaiters();
}

void fakeHoldCurrentEventWaiter() {
	std::lock_guard<std::mutex> lock(heldEventMutex);
	releaseHeldEventWaiters = false;
	holdCurrentEventWaiter = true;
}

bool fakeWaitForHeldEventWaiter(uint32_t timeoutMs) {
	std::unique_lock<std::mutex> lock(heldEventMutex);
	return heldEventCondition.wait_for(
	    lock,
	    std::chrono::milliseconds(timeoutMs),
	    []() { return heldEventWaiterCount > 0; }
	);
}

void fakeReleaseHeldEventWaiters() {
	{
		std::lock_guard<std::mutex> lock(heldEventMutex);
		releaseHeldEventWaiters = true;
	}
	heldEventCondition.notify_all();
}
