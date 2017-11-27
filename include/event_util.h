#pragma once

#include <event2/event.h>
#include <event2/thread.h>
#include <sys/time.h>
#include <unistd.h>

#include <functional>
#include <thread>

#include "macro.h"

BEGIN_UTIL_NAMESPACE

/**
 * \brief libevent base
 */
class CEventBase
{
public:
	/**
	 * \brief  instance
	 * \return instance
	 */
	static CEventBase &GetInstance() {
		static CEventBase _instance;
		return _instance;
	}

	CEventBase()
		: _base(NULL),
		  _thread(NULL),
		  _event(NULL) {
		static int init = evthread_use_pthreads();
		(void)init;
		_base = event_base_new();
		Start();
	}

	~CEventBase() {
		Stop();
		event_base_free(_base);
		_base = NULL;
	}

	/**
	 * \brief  get event_base pointer
	 * \return event_base pointer
	 */
	struct event_base *Pointer() {
		return _base;
	}
	/**
	 * \note  autostart
	 * \return true/false
	 */
	bool Start() {
		if (_thread != NULL)
		{
			return true;
		}

		/* add a timer event to make it auto run */
		struct timeval tv;
		evutil_timerclear(&tv);
		tv.tv_sec = 3600;
		_event = event_new(_base, -1,
			EV_TIMEOUT | EV_PERSIST,
			TimeProc, this);
		event_add(_event, &tv);

		_thread = new std::thread(std::bind(&CEventBase::Run, this));
		return true;
	}
	/**
	 * \note  autostop
	 * \return true/false
	 */
	bool Stop() {
		if (_thread == NULL)
		{
			return true;
		}
		event_base_loopexit(_base, NULL);
		if (_event)
		{
			event_free(_event);
			_event = NULL;
		}
		_thread->join();
		delete _thread;
		_thread = NULL;
		return true;
	}

private:
	void Run() {
		event_base_dispatch(_base);
	}
	static void TimeProc(evutil_socket_t, short, void *) {
	}
	struct event_base *_base;
	std::thread *_thread;
	struct event *_event;
};

/**
 * \brief libevent timer
 */
class CEventTimer
{
public:
	CEventTimer()
		: _millisecond(0),
		  _singleShot(false),
		  _active(false),
		  _deleteLater(false),
		  _inCallback(false),
		  _event(NULL),
		  _base(NULL) {
		_tv.tv_sec = 0;
		_tv.tv_usec = 0;
	}

	explicit CEventTimer(int millisecond)
		: _millisecond(millisecond),
		  _singleShot(false),
		  _active(false),
		  _deleteLater(false),
		  _inCallback(false),
		  _event(NULL),
	 	  _base(NULL) {
		_tv.tv_sec = 0;
		_tv.tv_usec = 0;
	}

	~CEventTimer() {
		Stop();
	}

	void DeleteLater() {
		_deleteLater = true;
		if (!_inCallback)
		{
			delete this;
		}
	}

	/**
	 * \brief  set event_base, or else we will call CEventBase::GetInstance() later
	 * \param  base -- libevent event_base
	 */
	void SetEventBase(struct event_base *base) {
		_base = base;
	}

	/**
	 * \brief  start or restart
	 */
	bool Start(int millisecond) {
		if (millisecond < 0)
		{
			return false;
		}

		/* start */
		if (!SetElapsed(millisecond))
		{
			return false;
		}

		_millisecond = millisecond;
		gettimeofday(&_tv, NULL);
		return true;
	}

	/**
	 * \brief  start or restart
	 */
	bool Start() {
		if (_millisecond < 0)
		{
			return false;
		}

		/* start */
		if (!SetElapsed(_millisecond))
		{
			return false;
		}

		gettimeofday(&_tv, NULL);
		return true;
	}

	bool Stop() {
		if (!_active)
		{
			return true;
		}

		return SetElapsed(0);
	}

	/**
	 * \note  not restart the timer
	 */
	void SetInterval(int millisecond) {
		_millisecond = millisecond;
	}

	/**
	 * \note not restart the timer
	 * \param  singleShot -- true run once, false loop forever
	 */
	void SetSingleShot(bool singleShot) {
		_singleShot = singleShot;
	}

	void SetCallback(std::function<void()> cb) {
		_cb = cb;
	}

	bool IsActive() const {
		return _active;
	}

	bool IsSingleShot() const {
		return _singleShot;
	}

	int GetInterval() const {
		return _millisecond;
	}

private:
	static void TimeProc(evutil_socket_t, short, void *arg) {
		CEventTimer *pTimer = (CEventTimer *)arg;
		pTimer->TimeOut();
		if (pTimer->_deleteLater)
		{
			delete pTimer;
		}
	}

	void TimeOut() {
		if (!_active)
		{
			return;
		}

		_inCallback = true;
		struct timeval tv = _tv;
		_cb();
		_inCallback = false;
		if (_singleShot
			&& tv.tv_sec == _tv.tv_sec && tv.tv_usec == _tv.tv_usec)
		{
			Stop();
		}
	}

	bool CreateTimer() {
		if (_event)
		{
			return false;
		}

		struct timeval tv;
		evutil_timerclear(&tv);
		tv.tv_sec = _millisecond / 1000;
		tv.tv_usec = _millisecond % 1000 * 1000;
		if (_event != NULL)
		{
			event_free(_event);
		}
		short events = EV_TIMEOUT;
		if (!_singleShot)
		{
			events |= EV_PERSIST;
		}
		struct event_base *base = _base;
		if (base == NULL)
		{
			base = CEventBase::GetInstance().Pointer();
		}
		_event = event_new(base, -1,
			events,
			TimeProc, this);
		event_add(_event, &tv);

		return true;
	}

	/**
	 * \param  millisecond(>=0) =0 stop, >0 restart timer
	 */
	bool SetElapsed(int millisecond) {
		if (millisecond < 0)
		{
			return false;
		}

		if (_event == NULL && millisecond > 0 && !CreateTimer())
		{
			return false;
		}

		if (millisecond > 0)
		{
			_active = true;
		}
		else
		{
			if (_event)
			{
				event_free(_event);
				_event = NULL;
			}
			_active = false;
		}
		return true;
	}

private:
	int _millisecond;
	bool _singleShot;
	bool _active;
	bool _deleteLater;
	bool _inCallback;
	struct event *_event;
	struct event_base *_base;
	struct timeval _tv;
	std::function<void()> _cb;
};

END_UTIL_NAMESPACE
