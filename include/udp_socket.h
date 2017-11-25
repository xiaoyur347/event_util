#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <event2/event.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <functional>
#include <mutex>
#include <string>

#include "macro.h"

BEGIN_UTIL_NAMESPACE

/**
 * \brief packet data info
 */
struct PackDataInfo_t
{
	/** data size */
	int  nDataSize;
	/** IP */
	char szIP[64];
	/** port */
	int nPort;
	/** data buffer */
	char szDataBuf[4096];

	PackDataInfo_t()
		: nDataSize(0),
		  nPort(0) {
		memset(szIP, 0, sizeof(szIP));
		memset(szDataBuf, 0, sizeof(szDataBuf));
	}
};

class CUdpSocket
{
public:
	typedef std::function<void(const PackDataInfo_t *)> Callback;

	CUdpSocket()
		: _socket(-1),
		  _bindPort(-1),
		  _connPort(-1),
		  _event_base(NULL),
		  _event(NULL) {
		Open();
	}

	~CUdpSocket() {
		Close();
		SetEventBase(NULL);
	}

	void SetEventBase(struct event_base *event) {
		if (event != NULL)
		{
			_event_base = event;
			_event = event_new(_event_base, _socket, EV_READ | EV_PERSIST,
					OnRecv, this);
			event_add(_event, NULL);
		}
		else
		{
			if (_event)
			{
				event_free(_event);
				_event = NULL;
			}
		}
	}

	void SetCallback(Callback cb) {
		_cb = cb;
	}

	bool Open() {
		if (_socket >= 0)
		{
			return true;
		}

		_socket = socket(AF_INET, SOCK_DGRAM, 0);
		evutil_make_socket_nonblocking(_socket);
		InnerBind();
		return true;
	}

	bool Close() {
		if (_socket < 0)
		{
			return true;
		}

		int nSocket = _socket;
		_socket = -1;
		//let recvfrom unstuck
		shutdown(nSocket,SHUT_RD);
		close(nSocket);

		return true;
	}

	bool Bind(int nPort) {
		if ((nPort < 1024) || (nPort > 65535))
		{
			return false;
		}

		if (_bindPort > 0)
		{
			return true;
		}

		_bindPort = nPort;
		return InnerBind();
	}

	bool Connect(const char *szIp, int nPort) {
		if ((nPort < 1024) || (nPort > 65535))
		{
			return false;
		}

		_connIp = szIp;
		_connPort = nPort;

		return true;
	}

	bool Send(const PackDataInfo_t *pData, int nSendTimes = 1) {
		if (NULL == pData)
		{
			return false;
		}

		PackDataInfo_t pack = *pData;
		if (pack.szIP[0] == '\0')
		{
			strcpy(pack.szIP, _connIp.c_str());
		}
		if (pack.nPort == 0)
		{
			pack.nPort = _connPort;
		}

		std::lock_guard<std::mutex> guard(_mutex);

		int nSuccCount = 0;
		for (int i = 0; i < nSendTimes; i++)
		{
			if (SendTo(&pack))
			{
				++nSuccCount;
			}
		}

		return nSuccCount != 0;
	}

private:
	bool InnerBind() {
		if (_bindPort < 0)
		{
			return false;
		}

		struct sockaddr_in LocalAddr;
		LocalAddr.sin_family = AF_INET;
		LocalAddr.sin_port = htons(_bindPort);
		LocalAddr.sin_addr.s_addr = INADDR_ANY;
		if (bind(_socket, (struct sockaddr *)&LocalAddr, sizeof(LocalAddr)))
		{
			LOG_ERROR("bind socket fail:%s", strerror(errno));
			_bindPort = -1;
			return false;
		}

		return true;
	}

	bool SendTo(const PackDataInfo_t *pData) {
		int nEnableBroadcast = (strcmp(pData->szIP, "255.255.255.255") == 0) ? 1 : 0;

		if (nEnableBroadcast)
		{
			setsockopt(_socket, SOL_SOCKET, SO_BROADCAST,
				&nEnableBroadcast, sizeof(int));
		}

		struct sockaddr_in to;

		memset(&to, 0, sizeof(struct sockaddr_in));
		to.sin_family = AF_INET;
		to.sin_addr.s_addr = inet_addr(pData->szIP);
		to.sin_port = htons(pData->nPort);

		int ret = sendto(_socket, pData->szDataBuf, pData->nDataSize, 0,
						(const struct sockaddr *)&to, sizeof(struct sockaddr_in));

		if (nEnableBroadcast)
		{
			nEnableBroadcast = 0;
			setsockopt(_socket, SOL_SOCKET, SO_BROADCAST,
				&nEnableBroadcast, sizeof(int));
		}

		return ret == pData->nDataSize;
	}

	static void OnRecv(evutil_socket_t, short, void *arg) {
		CUdpSocket *pParent = (CUdpSocket *)arg;
		if (NULL == pParent)
		{
			return;
		}

		if (pParent->_socket < 0)
		{
			LOG_INFO("socket close");
			return;
		}

		PackDataInfo_t PackDataInfo;
		struct sockaddr_in SockAddr;
		socklen_t fromlen = sizeof(struct sockaddr_in);
		memset(&SockAddr, 0, sizeof(struct sockaddr_in));
		PackDataInfo.nDataSize = ::recvfrom(pParent->_socket,
			PackDataInfo.szDataBuf,
			sizeof(PackDataInfo.szDataBuf),
			0,
			(struct sockaddr *)&SockAddr,
			&fromlen);

		if (PackDataInfo.nDataSize <= 0)
		{
			return;
		}

		inet_ntop(AF_INET, &SockAddr.sin_addr, PackDataInfo.szIP,
			sizeof(PackDataInfo.szIP));
		pParent->_cb(&PackDataInfo);		
	}

	mutable std::mutex _mutex;
	int _socket;
	int _bindPort;
	std::string _connIp;
	int _connPort;

	Callback _cb;

	struct event_base *_event_base;
	struct event *_event;
};

END_UTIL_NAMESPACE
