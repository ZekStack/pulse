		           static_cast<uint32_t>(uxQueueMessagesWaiting(commandQueue)) :
		           0;
	}

	void processQueuedSnapshot(uint32_t count) {
		for (uint32_t index = 0; index < count; index++) {
			QueueHandle_t queue = nullptr;
			{
				PulseLock lock(mutex);
				if (!lock || lifecycle != PulseLifecycleState::Running) {
					return;
				}
				queue = commandQueue;
			}
			if (queue == nullptr) {
				return;
			}
			PulseCommand command;
			if (xQueueReceive(queue, &command, 0) != pdTRUE) {
				return;
			}
			PulseTimerRecord *removed = processCommand(command);
			if (removed != nullptr) {
				removed->release();
			}
		}
	}

	TickType_t nextWaitTicks() {
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running) {
			return 0;
		}
		if (activeTimerCount == 0) {
			return portMAX_DELAY;
		}
		const uint64_t nowMs = pulseNowMs();
		PulseTimerRecord *timer = activeTimers[0];
		if (timer == nullptr || timer->nextDueMs <= nowMs) {
			return 0;
		}
		return timeoutTicks(timer->nextDueMs - nowMs);
	}

	bool takeDueTimer(PulseTimerRecord *&out, uint64_t &dueMs) {
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running) {
			return false;
		}
		const uint64_t nowMs = pulseNowMs();
		while (activeTimerCount > 0) {
			PulseTimerRecord *timer = activeTimers[0];
			if (timer == nullptr) {
				removeActiveLocked(kInvalidTimerId);
				continue;
			}
			if (timer->nextDueMs > nowMs) {
				return false;
			}
			removeActiveLocked(timer->id);
			if (timer->state != PulseTimerState::Running || !hasTimerLocked(timer->id)) {
				continue;
			}
			dueMs = timer->nextDueMs;
			if (nowMs > timer->nextDueMs) {
				lateCallbackCount++;
			}
			timer->retain();
			out = timer;
			return true;
		}
		return false;
	}

	void executeTimer(PulseTimerRecord *timer, uint64_t dueMs) {
		if (timer == nullptr) {
			return;
		}

		PulseCountdownTick tick;
		PulseTimerRecord *removedRegistryOwner = nullptr;
		uint32_t generationBeforeCallback = 0;
		bool terminal = false;
		bool invokeCallback = false;

		{
			PulseLock lock(mutex);
			if (!lock || lifecycle != PulseLifecycleState::Running ||
			    timer->state != PulseTimerState::Running || !hasTimerLocked(timer->id)) {
				return;
			}

			(void)dueMs;
			generationBeforeCallback = timer->mutationGeneration;

			if (timer->type == PulseTimerType::Timeout) {
				terminal = true;
				removedRegistryOwner = removeRegistryLocked(timer->id);
				invokeCallback = static_cast<bool>(timer->callback);
			} else if (timer->type == PulseTimerType::Interval) {
				timer->executingCallback = true;
				timer->callbackDefaultDelayMs = timer->intervalMs;
				invokeCallback = static_cast<bool>(timer->callback);
			} else {
				const uint32_t remainingBefore = timer->durationMs > timer->elapsedMs ?
				                                     timer->durationMs - timer->elapsedMs :
				                                     0;
				const uint32_t stepMs = std::min(timer->tickMs, remainingBefore);
				timer->elapsedMs = std::min(timer->durationMs, timer->elapsedMs + stepMs);
				tick.elapsedMs = timer->elapsedMs;
				tick.remainingMs = timer->durationMs > timer->elapsedMs ?
				                       timer->durationMs - timer->elapsedMs :
				                       0;
				tick.elapsedSeconds = tick.elapsedMs / 1000;
				tick.remainingSeconds = tick.remainingMs / 1000;
				tick.isFinished = timer->elapsedMs >= timer->durationMs;
				terminal = tick.isFinished;
				if (terminal) {
					removedRegistryOwner = removeRegistryLocked(timer->id);
				} else {
					timer->executingCallback = true;
					timer->callbackDefaultDelayMs = std::min(timer->tickMs, tick.remainingMs);
				}
				invokeCallback = static_cast<bool>(timer->countdownCallback);
			}
		}

		if (removedRegistryOwner != nullptr) {
			removedRegistryOwner->release();
		}

		if (timer->type == PulseTimerType::Countdown) {
			if (timer->countdownCallback) {
				timer->countdownCallback(tick);
			}
		} else if (timer->callback) {
			timer->callback();
		}

		if (isRunning()) {
			uint32_t queuedAtCallbackCompletion = 0;
			{
				PulseLock lock(mutex);
				if (lock && lifecycle == PulseLifecycleState::Running) {
					queuedAtCallbackCompletion =
					    std::min(commandQueueUsageLocked(), config.commandQueueSize);
				}
			}
			processQueuedSnapshot(queuedAtCallbackCompletion);
		}

		{
			PulseLock lock(mutex);
			if (!lock) {
				return;
			}
			if (invokeCallback) {
				executedCallbackCount++;
			}
			if (terminal) {
				return;
			}
			timer->executingCallback = false;
			if (lifecycle != PulseLifecycleState::Running || !hasTimerLocked(timer->id)) {
				return;
			}
			if (timer->mutationGeneration == generationBeforeCallback &&
			    timer->state == PulseTimerState::Running) {
				timer->nextDueMs = pulseNowMs() + timer->callbackDefaultDelayMs;
				insertActiveLocked(timer);
			}
		}
	}

	void taskLoop() {
		while (true) {
			const TickType_t waitTicks = nextWaitTicks();
			ulTaskNotifyTake(pdTRUE, waitTicks);

			if (!isRunning()) {
				break;
			}

			uint32_t snapshot = 0;
			{
				PulseLock lock(mutex);
				if (lock && lifecycle == PulseLifecycleState::Running) {
					snapshot = std::min(commandQueueUsageLocked(), config.commandQueueSize);
				}
			}
			processQueuedSnapshot(snapshot);

			if (!isRunning()) {
				break;
			}

			PulseTimerRecord *timer = nullptr;
			uint64_t dueMs = 0;
			if (takeDueTimer(timer, dueMs)) {
				executeTimer(timer, dueMs);
				timer->release();
			}
		}
	}

