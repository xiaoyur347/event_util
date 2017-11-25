#pragma once

#include <pthread.h>
#include <sys/time.h>
#include "macro.h"

#ifdef __ANDROID__
#include <android/api-level.h>
#endif

BEGIN_UTIL_NAMESPACE

/**
 * \brief Poco Event like
 */
class Event
{
public:
	explicit Event(bool autoReset = true)
		: _auto(autoReset),
		  _state(false) {
		pthread_mutex_init(&_mutex, NULL);
#if (defined(__ANDROID__) && (HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC || __ANDROID_API__ < 21)) || defined(__APPLE__)
		pthread_cond_init(&_cond, NULL);
#else
		pthread_condattr_t condattr;
		pthread_condattr_init(&condattr);
		pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
		pthread_cond_init(&_cond, &condattr);
		pthread_condattr_destroy(&condattr);
#endif
	}

	~Event() {
		pthread_cond_destroy(&_cond);
		pthread_mutex_destroy(&_mutex);
	}

	void Reset() {
		pthread_mutex_lock(&_mutex);
		_state = false;
		pthread_mutex_unlock(&_mutex);
	}

	void Set() {
		pthread_mutex_lock(&_mutex);
		_state = true;
		pthread_cond_broadcast(&_cond);
		pthread_mutex_unlock(&_mutex);
	}

	bool IsSet() const {
		pthread_mutex_lock(&_mutex);
		bool ret = _state;
		pthread_mutex_unlock(&_mutex);
		return ret;
	}

	void Wait() {
		pthread_mutex_lock(&_mutex);
		while (!_state)
		{
			pthread_cond_wait(&_cond, &_mutex);
		}
		if (_auto)
		{
			_state = false;
		}
		pthread_mutex_unlock(&_mutex);
	}

	bool WaitUs(int microsecond) {
		struct timespec ts;
#ifndef __APPLE__
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec += microsecond / 1000000;
		ts.tv_nsec += (microsecond % 1000000) * 1000;
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + microsecond / 1000000;
		ts.tv_nsec = (tv.tv_usec + (microsecond % 1000000)) * 1000;
#endif

		if (ts.tv_nsec >= 1000000000)
		{
			ts.tv_nsec -= 1000000000;
			++ts.tv_sec;
		}

		pthread_mutex_lock(&_mutex);
		int rc = 0;
		while (!_state)
		{
#if defined(__ANDROID__) && HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC
			if ((rc = pthread_cond_timedwait_monotonic_np(&_cond, &_mutex, &ts)))
#else
			if ((rc = pthread_cond_timedwait(&_cond, &_mutex, &ts)))
#endif
			{
				break;
			}
		}
		bool bak = _state;
		if (rc == 0 && _auto)
		{
			_state = false;
		}
		pthread_mutex_unlock(&_mutex);
		return bak;
	}

	bool WaitMs(int millisecond) {
		return WaitUs(millisecond * 1000);
	}

private:
	Event(const Event&);
	Event& operator = (const Event&);

	bool _auto;
	volatile bool _state;
	mutable pthread_mutex_t _mutex;
	pthread_cond_t _cond;
};

END_UTIL_NAMESPACE
