#pragma once

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <list>
#include <mutex>
#include <string>

#include "macro.h"
#include "udp_socket.h"

BEGIN_UTIL_NAMESPACE

class IUdpProxy
{
public:
	virtual ~IUdpProxy() {}
	virtual std::string GetSendSessionId(const PackDataInfo_t *pPackData) = 0;
	virtual std::string GetRecvSessionId(const PackDataInfo_t *pPackData) = 0;
	virtual void OnPassiveRecv() {}
};

/**
 * \brief Udp proxy
 */
class CUdpProxy
{
public:
	CUdpProxy()
		: _enablePassiveRecv(false),
	      _func(NULL) {
		_socket = new CUdpSocket();
		_socket->SetCallback(std::bind(&CUdpProxy::OnRecvPack, this,
			std::placeholders::_1));
	}

	~CUdpProxy() {
		_enablePassiveRecv = false;
	
		delete _socket;
		_socket = NULL;

		//clear the callback to avoid segment fault
		_func = NULL;

		std::lock_guard<std::mutex> guard(_mutex);

		for (std::list<pthread_cond_t *>::iterator it = _totalCondList.begin();
			it != _totalCondList.end(); it++)
		{
			if (*it != NULL)
			{
				pthread_cond_destroy(*it);
				delete *it;
				*it = NULL;
			}
		}
	}

	void SetListener(IUdpProxy *pFunc) {
		_func = pFunc;
	}

	void SetEventBase(struct event_base *event) {
		_socket->SetEventBase(event);
	}
	
	void EnablePassiveRecv(bool bEnable) {
		_enablePassiveRecv = bEnable;
	}
	/**
	 * \brief  get passive pack
	 * \param  pPackDataInfo -- out param
	 */
	bool PopPassivePack(PackDataInfo_t *pPackDataInfo) {
		if (NULL == pPackDataInfo)
		{
			return false;
		}

		std::lock_guard<std::mutex> guard(_mutex);
		if (_recvPackList.empty())
		{
			return false;
		}

		*pPackDataInfo = *_recvPackList.begin();
		_recvPackList.pop_front();

		return true;
	}

	bool SendPacket(const PackDataInfo_t *pSendPackData,
				   int nSendTimes = 1) {
		if (NULL == pSendPackData)
		{
			return false;
		}

		bool ret = _socket->Send(pSendPackData, nSendTimes);
		if (!ret)
		{
			LOG_ERROR("SendData Failed!");
			return false;
		}

		return true;
	}

	/**
	 * \brief  lock the call thread until package receive
	 */
	bool ExecuteQuery(const PackDataInfo_t *pSendPackData,
					 PackDataInfo_t *pRecvPackData,
					 int nTimeOut) {
		if ((NULL == pSendPackData) ||
			(NULL == pRecvPackData))
		{
			return false;
		}

		std::lock_guard<std::mutex> guard(_mutex);

		pthread_cond_t *pCond = NULL;
		if (!AddWaitPacket(pSendPackData, pRecvPackData, &pCond))
		{
			return false;
		}

		bool ret = true;
		if (!_socket->Send(pSendPackData, 1)
			|| !Wait(pCond, nTimeOut))
		{
			ClearWaitPacket(pRecvPackData, &pCond);
			ret = false;
		}

		_freeCondList.push_back(pCond);
		return ret;
	}

	bool Bind(int nBindPort) {
		if ((nBindPort < 1024)	||
				(nBindPort > 65535))
		{
			return false;
		}

		_socket->Bind(nBindPort);
		return true;
	}
private:
	enum
	{
		MAX_PACKAGE_QUEUE_LEN = 1024
	};

	struct PACK_THREAD_COND
	{
		std::string strSessionId;
		PackDataInfo_t *pRecvPackData;
		pthread_cond_t *pCond;

		PACK_THREAD_COND()
			: pRecvPackData(NULL),
			  pCond(NULL) {
		}
	};

	void OnRecvPack(const PackDataInfo_t *pRecvPackData) {
		std::unique_lock<std::mutex> lock(_mutex);
		pthread_cond_t *pCond = NULL;
		int nRet = FindWaitPacket(pRecvPackData, &pCond);
		if (nRet < 0)
		{
			return;
		}
		else if (nRet == 1)
		{
			if (pCond != NULL)
			{
				pthread_cond_broadcast(pCond);
			}
			return;
		}

		//not found, try passive receive
		if (!_enablePassiveRecv)
		{
			return;	
		}

		if (_recvPackList.size() >= MAX_PACKAGE_QUEUE_LEN)
		{
			LOG_WARN("exceed threshold, drop packet!");
			return;
		}
		_recvPackList.insert(_recvPackList.begin(), *pRecvPackData);

		lock.unlock();

		IUdpProxy *pFunc = _func;
		if (pFunc)
		{
			pFunc->OnPassiveRecv();
		}

		return;
	}

	bool AddWaitPacket(const PackDataInfo_t *pSendPack, 
		PackDataInfo_t *pRecvPack,
		pthread_cond_t **ppCond) {
		std::string sessionId = _func->GetSendSessionId(pSendPack);
		if (sessionId.empty())
		{
			return false;
		}
		
		pthread_cond_t *pCond = NULL;
		
		if (_freeCondList.empty())
		{
			pCond = new pthread_cond_t();
			pthread_cond_init(pCond, NULL);
			_freeCondList.push_back(pCond);
			_totalCondList.push_back(pCond);
		}

		pCond = *_freeCondList.begin();
		_freeCondList.pop_front();

		PACK_THREAD_COND waitPack;
		waitPack.strSessionId = sessionId;
		waitPack.pRecvPackData = pRecvPack;
		waitPack.pCond = pCond;
		_waitPackList.push_front(waitPack);

		*ppCond = pCond;

		return true;
	}

	int FindWaitPacket(const PackDataInfo_t *pRecvPack, 
		pthread_cond_t **ppCond) {
		std::string sessionId = _func->GetRecvSessionId(pRecvPack);
		if (sessionId.empty())
		{
			return -1;
		}

		for (auto it = _waitPackList.begin();
			it != _waitPackList.end(); it++)
		{
			if (sessionId == it->strSessionId)
			{
				if (it->pRecvPackData == NULL)
				{
					{
						char szBuf[256];
						memset(szBuf, 0, sizeof(szBuf));
						char szHex[4];
						for (int i = 0; i < pRecvPack->nDataSize; i++)
						{
							if (i > 80)
							{
								break;
							}
							sprintf(szHex, "%02x ", (unsigned char)pRecvPack->szDataBuf[i]);
							strcat(szBuf, szHex);
						}
						LOG_WARN("OverTime Hex=%s", szBuf);
					}
					*ppCond = NULL;
				}
				else
				{
					memcpy(it->pRecvPackData, pRecvPack, sizeof(PackDataInfo_t));
					*ppCond = it->pCond;
				}

				_waitPackList.erase(it);
				return 1;
			}
		}

		return 0;
	}

	bool ClearWaitPacket(const PackDataInfo_t *pRecvPack, 
		pthread_cond_t **ppCond) {
		for (std::list<PACK_THREAD_COND>::iterator it = _waitPackList.begin();
			it != _waitPackList.end(); it++)
		{
			if (pRecvPack == it->pRecvPackData && *ppCond == it->pCond)
			{
				it->pRecvPackData = NULL;
				it->pCond = NULL;

				//_waitPackList.erase(it);
				return true;
			}
		}

		return false;
	}

	bool Wait(pthread_cond_t *cond, int millisecond) {
		struct timespec ts;
#ifndef __APPLE__
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_sec += millisecond / 1000;
		ts.tv_nsec += (millisecond % 1000) * 1000000;
#else
		struct timeval tv;
		gettimeofday(&tv, NULL);
		ts.tv_sec = tv.tv_sec + millisecond / 1000;
		ts.tv_nsec = tv.tv_usec * 1000 + (millisecond % 1000) * 1000000;
#endif

		if (ts.tv_nsec >= 1000000000)
		{
			ts.tv_nsec -= 1000000000;
			++ts.tv_sec;
		}

#if defined(__ANDROID__) && HAVE_PTHREAD_COND_TIMEDWAIT_MONOTONIC
		return pthread_cond_timedwait_monotonic_np(cond, &_mutex.native_handle(), &ts) == 0;
#else
		return pthread_cond_timedwait(cond, _mutex.native_handle(), &ts) == 0;
#endif
	}

	CUdpSocket *_socket;
	bool _enablePassiveRecv;
	mutable std::mutex _mutex;
	IUdpProxy *_func;
	
	std::list<PackDataInfo_t> _recvPackList;
	std::list<PACK_THREAD_COND> _waitPackList;
	std::list<pthread_cond_t *> _freeCondList;
	std::list<pthread_cond_t *> _totalCondList;
};

END_UTIL_NAMESPACE
