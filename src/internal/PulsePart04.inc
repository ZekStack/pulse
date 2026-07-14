	void cleanupAndPublishStopped() {
		std::unique_ptr<PulseTimerRecord *[]> detachedTimers;
		std::unique_ptr<PulseTimerRecord *[]> detachedActiveTimers;
		uint32_t detachedTimerCount = 0;
		QueueHandle_t detachedQueue = nullptr;
		const size_t finalStack = pulse_task_support::currentStackHighWaterMarkBytes();
		bool withCaps = false;

		{
			PulseLock lock(mutex);
			if (lock) {
				detachedTimers = std::move(timers);
				detachedActiveTimers = std::move(activeTimers);
				detachedTimerCount = timerCount;
				detachedQueue = commandQueue;
				commandQueue = nullptr;
				timerCapacity = 0;
				timerCount = 0;
				activeTimerCount = 0;
				taskHandle = nullptr;
				withCaps = createdWithCaps;
			}
		}

		if (detachedTimers) {
			for (uint32_t index = 0; index < detachedTimerCount; index++) {
				if (detachedTimers[index] != nullptr) {
					detachedTimers[index]->release();
					detachedTimers[index] = nullptr;
				}
			}
		}
		detachedTimers.reset();
		detachedActiveTimers.reset();
		if (detachedQueue != nullptr) {
			vQueueDelete(detachedQueue);
		}

		{
			PulseLock lock(mutex);
			if (lock) {
				finalStackHighWaterMarkBytes = finalStack;
				lifecycle = PulseLifecycleState::Stopped;
			}
		}
		xEventGroupSetBits(completionEvent, kTaskStoppedBit);

		release();
		pulse_task_support::deleteCurrentTask(withCaps);
	}

	static void taskEntry(void *arg) {
		PulseImpl *impl = static_cast<PulseImpl *>(arg);
		if (impl == nullptr) {
			vTaskDelete(nullptr);
			return;
		}
		impl->taskLoop();
		impl->cleanupAndPublishStopped();
	}
};

namespace {
PulseImpl *loadImpl(const std::atomic<PulseImpl *> &storage) {
	return storage.load(std::memory_order_acquire);
}

PulseResult waitForEnd(PulseImpl *impl, uint32_t timeoutMs, bool waitForever) {
	if (impl == nullptr) {
		return PulseResult::success("pulse not initialized");
	}

	uint64_t generation = 0;
	{
		PulseLock lock(impl->mutex);
		if (!lock) {
			return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
		}
		if (impl->lifecycle == PulseLifecycleState::Uninitialized) {
			return PulseResult::success("pulse not initialized");
		}
		if (pulse_task_support::isCurrentTask(impl->taskHandle)) {
			return PulseResult::failure(PulseStatus::Busy, "end cannot wait from the pulse task");
		}
		generation = impl->lifecycleGeneration;
		if (impl->lifecycle == PulseLifecycleState::Stopped) {
			impl->lifecycle = PulseLifecycleState::Uninitialized;
			return PulseResult::success("pulse ended");
		}
		if (impl->lifecycle == PulseLifecycleState::Running) {
			impl->lifecycle = PulseLifecycleState::Stopping;
			pulse_task_support::wakeTask(impl->taskHandle);
		}
		impl->endWaiterCount++;
	}

	const uint64_t startMs = pulseNowMs();
	while (true) {
		{
			PulseLock lock(impl->mutex);
			if (!lock) {
				return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
			}
			if (impl->lifecycleGeneration != generation ||
			    impl->lifecycle == PulseLifecycleState::Uninitialized) {
				impl->endWaiterCount--;
				return PulseResult::success("pulse ended");
			}
			if (impl->lifecycle == PulseLifecycleState::Stopped) {
				impl->lifecycle = PulseLifecycleState::Uninitialized;
				impl->endWaiterCount--;
				return PulseResult::success("pulse ended");
			}
		}

		TickType_t waitTicks = portMAX_DELAY;
		if (!waitForever) {
			const uint64_t elapsedMs = pulseNowMs() - startMs;
			if (elapsedMs >= timeoutMs) {
				PulseLock lock(impl->mutex);
				if (lock && impl->endWaiterCount > 0) {
					impl->endWaiterCount--;
				}
				return PulseResult::failure(PulseStatus::Timeout, "pulse end timed out");
			}
			waitTicks = timeoutTicks(timeoutMs - elapsedMs);
		}

		xEventGroupWaitBits(
		    impl->completionEvent,
		    kTaskStoppedBit,
		    pdFALSE,
		    pdTRUE,
		    waitTicks
		);
	}
}

PulseResult controlTimer(
    PulseImpl *impl,
    PulseTimerId id,
    PulseCommandType command,
    bool validateType,
    PulseTimerType expectedType,
    const char *wrongTypeMessage
) {
	if (impl == nullptr) {
		return PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}
	PulseLock lock(impl->mutex);
	if (!lock) {
		return PulseResult::failure(PulseStatus::InternalError, "failed to lock pulse");
	}
	PulseResult lifecycleResult = impl->validateControlLocked();
	if (!lifecycleResult) {
		return lifecycleResult;
	}
	PulseTimerRecord *timer = impl->findTimerLocked(id);
	if (timer == nullptr) {
		return PulseResult::failure(PulseStatus::TimerNotFound, "timer not found");
	}
	if (validateType && timer->type != expectedType) {
		return PulseResult::failure(PulseStatus::InvalidArgument, wrongTypeMessage);
	}
	return impl->enqueueCommandLocked(command, id);
}

PulseTimerResult scheduleTimer(
    PulseImpl *impl,
    PulseTimerRecord *timer,
    PulseTimerType type,
    const char *limitMessage,
    uint32_t firstDelayMs,
    const char *successMessage
) {
	if (timer == nullptr) {
		return PulseTimerResult::failure(PulseStatus::OutOfMemory, "failed to allocate timer");
	}
	if (impl == nullptr) {
		timer->release();
		return PulseTimerResult::failure(PulseStatus::NotInitialized, "pulse is not initialized");
	}

	PulseTimerResult result;
	PulseTimerRecord *releaseAfterUnlock = nullptr;
	{
		PulseLock lock(impl->mutex);
		if (!lock) {
			result = PulseTimerResult::failure(PulseStatus::InternalError, "failed to lock pulse");
			releaseAfterUnlock = timer;
		} else {
