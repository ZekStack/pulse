#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class PulseMutex {
  public:
	PulseMutex() {
		_handle = xSemaphoreCreateRecursiveMutex();
	}

	~PulseMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
		}
	}

	PulseMutex(const PulseMutex &) = delete;
	PulseMutex &operator=(const PulseMutex &) = delete;

	bool isValid() const {
		return _handle != nullptr;
	}

	bool lock(TickType_t timeout = portMAX_DELAY) {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, timeout) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class PulseLock {
  public:
	explicit PulseLock(PulseMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
	}

	~PulseLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	PulseLock(const PulseLock &) = delete;
	PulseLock &operator=(const PulseLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	PulseMutex &_mutex;
	bool _locked = false;
};
