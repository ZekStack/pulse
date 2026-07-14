		if (lifecycle != PulseLifecycleState::Running) {
			return PulseResult::failure(PulseStatus::Busy, "pulse is stopping");
		}
		return PulseResult::success();
	}

	bool allocateTimerIdLocked(PulseTimerId &out) {
		for (uint32_t attempts = 0; attempts < UINT32_MAX; attempts++) {
			const PulseTimerId id = nextTimerId++;
			if (id == kInvalidTimerId) {
				continue;
			}
			if (!hasTimerLocked(id)) {
				out = id;
				return true;
			}
		}
		return false;
	}

	bool addRegistryLocked(PulseTimerRecord *timer) {
		if (timer == nullptr || timerCount >= timerCapacity) {
			return false;
		}
		timers[timerCount++] = timer;
		return true;
	}

	void removeActiveLocked(PulseTimerId id) {
		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < activeTimerCount; readIndex++) {
			PulseTimerRecord *timer = activeTimers[readIndex];
			if (timer != nullptr && timer->id != id) {
				activeTimers[writeIndex++] = timer;
			}
		}
		for (uint32_t index = writeIndex; index < activeTimerCount; index++) {
			activeTimers[index] = nullptr;
		}
		activeTimerCount = writeIndex;
	}

	PulseTimerRecord *removeRegistryLocked(PulseTimerId id) {
		removeActiveLocked(id);
		PulseTimerRecord *removed = nullptr;
		uint32_t writeIndex = 0;
		for (uint32_t readIndex = 0; readIndex < timerCount; readIndex++) {
			PulseTimerRecord *timer = timers[readIndex];
			if (timer != nullptr && timer->id == id) {
				removed = timer;
				continue;
			}
			if (timer != nullptr) {
				timers[writeIndex++] = timer;
			}
		}
		for (uint32_t index = writeIndex; index < timerCount; index++) {
			timers[index] = nullptr;
		}
		timerCount = writeIndex;
		return removed;
	}

	void insertActiveLocked(PulseTimerRecord *timer) {
		if (timer == nullptr || timer->state != PulseTimerState::Running) {
			return;
		}
		removeActiveLocked(timer->id);
		if (activeTimerCount >= timerCapacity) {
			return;
		}
		activeTimers[activeTimerCount++] = timer;
		std::sort(
		    activeTimers.get(),
		    activeTimers.get() + activeTimerCount,
		    [](const PulseTimerRecord *left, const PulseTimerRecord *right) {
			    if (left == nullptr) {
				    return false;
			    }
			    if (right == nullptr) {
				    return true;
			    }
			    if (left->nextDueMs == right->nextDueMs) {
				    return left->id < right->id;
			    }
			    return left->nextDueMs < right->nextDueMs;
		    }
		);
	}

	PulseResult enqueueCommandLocked(PulseCommandType type, PulseTimerId id) {
		if (lifecycle != PulseLifecycleState::Running || commandQueue == nullptr) {
			return lifecycle == PulseLifecycleState::Uninitialized ?
			           PulseResult::failure(PulseStatus::NotInitialized, "pulse is not initialized") :
			           PulseResult::failure(PulseStatus::Busy, "pulse is stopping");
		}
		PulseCommand command;
		command.type = type;
		command.id = id;
		if (xQueueSend(commandQueue, &command, 0) != pdTRUE) {
			droppedCommandCount++;
			return PulseResult::failure(PulseStatus::QueueFull, "pulse command queue is full");
		}
		pulse_task_support::wakeTask(taskHandle);
		return PulseResult::success("command queued");
	}

	PulseTimerRecord *processCommand(const PulseCommand &command) {
		PulseTimerRecord *removed = nullptr;
		const uint64_t nowMs = pulseNowMs();
		PulseLock lock(mutex);
		if (!lock || lifecycle != PulseLifecycleState::Running) {
			return nullptr;
		}

		PulseTimerRecord *timer = findTimerLocked(command.id);
		if (timer == nullptr) {
			return nullptr;
		}

		if (command.type == PulseCommandType::Add) {
			if (timer->state == PulseTimerState::Running) {
				insertActiveLocked(timer);
			}
			return nullptr;
		}

		if (command.type == PulseCommandType::Clear) {
			timer->mutationGeneration++;
			return removeRegistryLocked(command.id);
		}

		if (command.type == PulseCommandType::Pause) {
			if (timer->state == PulseTimerState::Running) {
				if (timer->executingCallback) {
					timer->remainingDelayMs =
					    timer->callbackDefaultDelayMs > 0 ? timer->callbackDefaultDelayMs : 1;
				} else {
					timer->remainingDelayMs = timer->nextDueMs > nowMs ?
					                              static_cast<uint32_t>(timer->nextDueMs - nowMs) :
					                              1;
				}
				timer->state = PulseTimerState::Paused;
				timer->mutationGeneration++;
				removeActiveLocked(timer->id);
			}
			return nullptr;
		}

		if (command.type == PulseCommandType::Resume) {
			if (timer->state == PulseTimerState::Paused) {
				const uint32_t delayMs = timer->remainingDelayMs > 0 ? timer->remainingDelayMs : 1;
				timer->nextDueMs = nowMs + delayMs;
				timer->remainingDelayMs = 0;
				timer->state = PulseTimerState::Running;
				timer->mutationGeneration++;
				insertActiveLocked(timer);
			}
			return nullptr;
		}

		if (command.type == PulseCommandType::Restart) {
			timer->state = PulseTimerState::Running;
			timer->remainingDelayMs = 0;
			timer->mutationGeneration++;
			if (timer->type == PulseTimerType::Countdown) {
				timer->elapsedMs = 0;
				timer->nextDueMs = nowMs + timer->tickMs;
			} else if (timer->type == PulseTimerType::Interval) {
				timer->nextDueMs = nowMs + timer->intervalMs;
			} else {
				timer->nextDueMs = nowMs + timer->delayMs;
			}
			insertActiveLocked(timer);
		}
		return removed;
	}

	bool isRunning() {
		PulseLock lock(mutex);
		return lock && lifecycle == PulseLifecycleState::Running;
	}

	uint32_t commandQueueUsageLocked() const {
		return commandQueue != nullptr ?
