#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

class CurierMutex {
  public:
	CurierMutex() : _handle(xSemaphoreCreateRecursiveMutex()) {
	}

	~CurierMutex() {
		if (_handle != nullptr) {
			vSemaphoreDelete(_handle);
			_handle = nullptr;
		}
	}

	CurierMutex(const CurierMutex &) = delete;
	CurierMutex &operator=(const CurierMutex &) = delete;

	bool valid() const {
		return _handle != nullptr;
	}

	bool lock() {
		return _handle != nullptr && xSemaphoreTakeRecursive(_handle, portMAX_DELAY) == pdTRUE;
	}

	void unlock() {
		if (_handle != nullptr) {
			(void)xSemaphoreGiveRecursive(_handle);
		}
	}

  private:
	SemaphoreHandle_t _handle = nullptr;
};

class CurierLock {
  public:
	explicit CurierLock(CurierMutex &mutex) : _mutex(mutex), _locked(mutex.lock()) {
	}

	~CurierLock() {
		if (_locked) {
			_mutex.unlock();
		}
	}

	CurierLock(const CurierLock &) = delete;
	CurierLock &operator=(const CurierLock &) = delete;

	explicit operator bool() const {
		return _locked;
	}

  private:
	CurierMutex &_mutex;
	bool _locked = false;
};
