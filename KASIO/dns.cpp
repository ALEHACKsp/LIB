#include "StdAfx.h"

_NT_BEGIN

#include "tdi.h"
//#define DbgPrint /##/
ULONG HashString(PCSTR lpsz, ULONG hash = 0)
{
	while (char c = *lpsz++) hash = hash * 33 ^ c;
	return hash;
}

struct DnsCache 
{
	struct CRC_IP_TIME { ULONG crc, ip, time, n; };

	inline static CRC_IP_TIME g_ip_cache[64] = {};
	inline static EX_PUSH_LOCK g_icl = {};

	static ULONG get(PCSTR name, ULONG& rcrc);
	static void set(ULONG crc, ULONG ip);
};

ULONG DnsCache::get(PCSTR name, ULONG& rcrc)
{
	ULONG crc = HashString(name);

	rcrc = crc;

	CRC_IP_TIME* p = &g_ip_cache[crc & (RTL_NUMBER_OF(g_ip_cache) - 1)];

	ULONG time = (ULONG)(KeQueryInterruptTime() / 10000000);
	DbgPrint("get<%p>%08x %u ? %u\n", p, crc, time, p->time);

	ULONG ip = 0;
	KeEnterCriticalRegion();
	ExfAcquirePushLockShared(&g_icl);
	if (p->crc == crc && time < p->time) ip = p->ip;
	ExfReleasePushLockShared(&g_icl);
	KeLeaveCriticalRegion();

	return ip;
}

void DnsCache::set(ULONG crc, ULONG ip)
{
	CRC_IP_TIME* p = &g_ip_cache[crc & (RTL_NUMBER_OF(g_ip_cache) - 1)];

	ULONG time = (ULONG)(KeQueryInterruptTime() / 10000000) + 1000;// 100 sec

	DbgPrint("set<%p>%08x %u\n", p, crc, time);

	KeEnterCriticalRegion();
	ExfAcquirePushLockExclusive(&g_icl);
	p->crc = crc, p->time = time, p->ip = ip, p->n++;
	ExfReleasePushLockExclusive(&g_icl);
	KeLeaveCriticalRegion();
}

class CDnsSocket;

class CDnsTask : public IO_OBJECT_TIMEOUT
{
	CTdiObject* _pEndp;
	CDnsSocket** _ppSocks;
	ULONG _n;
	LONG _nRecvCount;
	LONG _bFirstIp;

	virtual void IOCompletionRoutine(CDataPacket* , ULONG , NTSTATUS , ULONG_PTR , PVOID )
	{
		__debugbreak();
	}

	virtual void OnTimeout()
	{
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
		Cleanup();
	}

	ULONG Create(ULONG n);

	void Cleanup();

	~CDnsTask()
	{
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
		Cleanup();
		if (_bFirstIp)
		{
			_pEndp->OnIp(0);
		}
		_pEndp->Release();
	}

public:

	void DecRecvCount();

	void OnIp(ULONG ip)
	{
		if (InterlockedExchangeNoFence(&_bFirstIp, FALSE))
		{
			DbgPrint("%s<%p> (%08x)\n", __FUNCTION__, this, ip);
			Cleanup();
			_pEndp->OnIp(ip);
		}
	}

	CDnsTask(CTdiObject* pEndp) : _ppSocks(0), _n(0), _bFirstIp(TRUE), _pEndp(pEndp)
	{
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
		_pEndp->AddRef();
	}

	void DnsToIp(PCSTR Dns, ULONG crc, ULONG dwMilliseconds = 3000);
};

class CDnsSocket : public CUdpEndpoint
{
	CDnsTask* _pTask;
	ULONG _crc;

	virtual void OnRecv(PSTR Buffer, ULONG cbTransferred);

	virtual void OnRecv(PSTR Buffer, ULONG cbTransferred, CDataPacket* , TA_IP_ADDRESS*  )
	{
		if (Buffer) OnRecv(Buffer, cbTransferred);
		_pTask->DecRecvCount();
	}

	~CDnsSocket()
	{
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
		_pTask->Release();
	}

public:

	BOOL Start(PCSTR Dns, ULONG ip, ULONG crc);

	CDnsSocket(CDnsTask* pTask) : _pTask(pTask)
	{
		pTask->AddRef();
		DbgPrint("%s<%p>\n", __FUNCTION__, this);
	}
};

//////////////////////////////////////////////////////////////////////////

void CDnsTask::Cleanup()
{
	DbgPrint("%s<%p> (%p)\n", __FUNCTION__, this, _ppSocks);

	StopTimeout();

	if (PVOID pv = InterlockedExchangePointerAcquire((void**)&_ppSocks, 0))
	{
		CDnsSocket** ppSocks = (CDnsSocket**)pv;

		if (ULONG n = _n)
		{
			do 
			{
				CDnsSocket* pSocks = *ppSocks++;
				pSocks->Close();
				pSocks->Release();
			} while (--n);
		}

		delete [] pv;
	}
}

void CDnsTask::DecRecvCount()
{
	if (!InterlockedDecrementNoFence(&_nRecvCount))
	{
		DbgPrint("%s<%p> (%p)\n", __FUNCTION__, this, _ppSocks);
		Cleanup();
	}
}

LONGLONG GetBootTime()
{
	static LONGLONG BootTime;
	if (!BootTime)
	{
		SYSTEM_TIMEOFDAY_INFORMATION sti;
		if (0 <= NtQuerySystemInformation(SystemTimeOfDayInformation, &sti, sizeof(sti), 0))
		{
			BootTime = sti.BootTime.QuadPart;
		}
		else
		{
			BootTime = 1;
		}
	}
	return BootTime;
}

volatile UCHAR guz;

ULONG FillDnsServerList(ULONG MaxCount, ULONG IPs[])
{
	HANDLE hKey;
	STATIC_OBJECT_ATTRIBUTES(soa, "\\registry\\MACHINE\\SYSTEM\\CurrentControlSet\\Services\\Tcpip\\Parameters\\Interfaces");
	UNICODE_STRING ObjectName;
	OBJECT_ATTRIBUTES oa = { sizeof(oa), 0, &ObjectName, OBJ_CASE_INSENSITIVE };

	ULONG m = 0;

	NTSTATUS status;

	if (0 <= (status = ZwOpenKey(&oa.RootDirectory, KEY_READ, &soa)))
	{
		PVOID stack = alloca(guz);
		union {
			PVOID buf;
			PKEY_BASIC_INFORMATION pkni;
			PKEY_VALUE_PARTIAL_INFORMATION pkvpi;
		};
		ULONG cb = 0, rcb = 0x100, Index = 0;

		LONGLONG BootTime = GetBootTime();

		do 
		{
			do 
			{
				if (cb < rcb) cb = RtlPointerToOffset(buf = alloca(rcb - cb), stack);

				if (0 <= (status = ZwEnumerateKey(oa.RootDirectory, Index, KeyBasicInformation, buf, cb, &rcb)))
				{
					if (pkni->LastWriteTime.QuadPart < BootTime)
					{
						goto __nextIndex;
					}

					ObjectName.Buffer = pkni->Name;
					ObjectName.MaximumLength = ObjectName.Length = (USHORT)pkni->NameLength;

					if (0 <= ZwOpenKey(&hKey, KEY_READ, &oa))
					{
						STATIC_UNICODE_STRING_(NameServer);
						STATIC_UNICODE_STRING_(DhcpNameServer);
						NTSTATUS ss;
						PCUNICODE_STRING aa[] = { &DhcpNameServer, &NameServer }, ValueName;
						ULONG n = RTL_NUMBER_OF(aa);

						do 
						{
							ValueName = aa[--n];

							do 
							{
								if (cb < rcb) cb = RtlPointerToOffset(buf = alloca(rcb - cb), stack);

								if (0 <= (ss = ZwQueryValueKey(hKey, ValueName, KeyValuePartialInformation, buf, cb, &rcb)))
								{
									ULONG DataLength = pkvpi->DataLength;
									union {
										PWSTR psz;
										PBYTE Data;
									};
									Data = pkvpi->Data;
									if (pkvpi->Type == REG_SZ && 
										DataLength > sizeof(WCHAR) && 
										!(DataLength & (sizeof(WCHAR) - 1)) &&
										!*(WCHAR*)(Data + DataLength - sizeof(WCHAR)))
									{
										ULONG ip;

										for (;;)
										{
											while (*psz == ' ') psz++;

											if (!*psz || 0 > RtlIpv4StringToAddressW(psz, TRUE, psz, ip))
											{
												break;
											}

											n = 0;// not look for DhcpNameServer if found NameServer
											*IPs++ = ip;
											if (++m, !--MaxCount)
											{
												NtClose(hKey);
												goto __exit;
											}
										}
									}
								}

							} while (ss == STATUS_BUFFER_OVERFLOW);

						} while (n);

						NtClose(hKey);
					}
				}

			} while (status == STATUS_BUFFER_OVERFLOW);

__nextIndex:
			Index++;

		} while (status != STATUS_NO_MORE_ENTRIES);
__exit:
		NtClose(oa.RootDirectory);
	}

	return m;
}

void CDnsTask::DnsToIp(PCSTR Dns, ULONG crc, ULONG dwMilliseconds)
{
	static ULONG seed;

	if (!seed)
	{
		LARGE_INTEGER  TickCount;
		KeQueryTickCount(&TickCount);
		seed = ~TickCount.LowPart;
	}

	ULONG i = RtlRandomEx(&seed);

	static ULONG DnsServerAddressesGroup1[] = {
		IP(8, 8, 8, 8),
		IP(8, 8, 4, 4),
	};

	static ULONG DnsServerAddressesGroup2[] = {
		IP(208, 67, 222, 222),
		IP(208, 67, 222, 220),
		IP(208, 67, 220, 220),
		IP(208, 67, 220, 222),
	};

	ULONG DnsServerAddresses[16] = { 
		DnsServerAddressesGroup1[i % RTL_NUMBER_OF(DnsServerAddressesGroup1)],
		DnsServerAddressesGroup2[i % RTL_NUMBER_OF(DnsServerAddressesGroup2)],
	};

	if (ULONG n = Create(2 + FillDnsServerList(RTL_NUMBER_OF(DnsServerAddresses) - 2, DnsServerAddresses + 2)))
	{
		if (SetTimeout(dwMilliseconds))
		{
			_nRecvCount = n;
			CDnsSocket** ppSocks = (CDnsSocket**)alloca(n * sizeof(CDnsSocket*)), *pSock;
			memcpy(ppSocks, _ppSocks, n * sizeof(CDnsSocket*));

			i = n;
			do 
			{
				(*ppSocks++)->AddRef();
			} while (--i);

			do 
			{
				--n, pSock = *--ppSocks;

				BOOL fOk = FALSE;

				if (CDataPacket* packet = new(1024) CDataPacket)
				{
					DbgPrint("-->%08x\n", DnsServerAddresses[n]);

					if (0 <= pSock->RecvFrom(packet) && pSock->Start(Dns, DnsServerAddresses[n], crc))
					{
						fOk = TRUE;
					}
					packet->Release();
				}

				if (!fOk)
				{
					DecRecvCount();
				}

				pSock->Release();

			} while (n);
		}
		else
		{
			Cleanup();
		}
	}
}

BOOL CDnsSocket::Start(PCSTR Dns, ULONG DnsServerIp, ULONG crc)
{
	_crc = crc;

	if (CDataPacket* packet = new(1024) CDataPacket)
	{
		PSTR __lpsz = packet->getData(), _lpsz, lpsz = __lpsz;
		char c, i;
		static USHORT bb1[6]={ 0x1111, 1, 0x0100 };
		static USHORT bb2[2]={ 0x0100, 0x0100 };
		memcpy(lpsz, bb1, sizeof bb1);
		lpsz += sizeof bb1;

		do 
		{
			_lpsz = lpsz++, i = 0;
mm:
			switch (c = *Dns++)
			{
			case '.':
			case 0:
				break;
			default:*lpsz++ = c, ++i;
				goto mm;
			}
			*_lpsz = i;
		} while (c);

		*lpsz++ = 0;

		memcpy(lpsz, bb2, sizeof bb2);

		packet->setDataSize(RtlPointerToOffset(__lpsz, lpsz) + sizeof(bb2));

		NTSTATUS status = SendTo(DnsServerIp, 0x3500, packet);

		packet->Release();

		return 0 <= status;
	}

	return FALSE;
}

ULONG CDnsTask::Create(ULONG n)
{
	if (!n)
	{
		return 0;
	}

	if (CDnsSocket** ppSocks = new(PagedPool) CDnsSocket*[n])
	{
		CDnsSocket** ppSocks2 = ppSocks;
		ULONG m = 0;

		do 
		{
			if (CDnsSocket* pSocks = new CDnsSocket(this))
			{
				if (0 > pSocks->Create(0))
				{
					pSocks->Release();
				}
				else
				{
					*ppSocks++ = pSocks;
					m++;
				}
			}
		} while (--n);

		_ppSocks = ppSocks2;		
		// -- memory_order_release

		if (m)
		{
			_n = m;
			return m;
		}

	}

	return 0;
}

void CDnsSocket::OnRecv(PSTR Buffer, ULONG cbTransferred)
{
	if (cbTransferred < 13) return ;

	Buffer += 12, cbTransferred -= 12;

	UCHAR c;

	while (c = *Buffer++)
	{
		if (cbTransferred < (ULONG)(2 + c)) return;
		cbTransferred -= 1 + c;
		Buffer += c;
	}

	Buffer += 4;
	if (cbTransferred < 4) return ;
	cbTransferred -= 4;

	struct DNS_RR
	{
		USHORT name, type, cls, ttl1, ttl2, len;
	} x;

	for(;;) 
	{
		if (cbTransferred < sizeof(DNS_RR)) return;
		memcpy(&x, Buffer, sizeof(x));
		cbTransferred -= sizeof (DNS_RR), Buffer += sizeof (DNS_RR);
		x.len = _byteswap_ushort(x.len);
		if (cbTransferred < x.len) return;
		cbTransferred -= x.len;
		if (x.type == 0x100 && x.cls == 0x100 && x.len == sizeof(ULONG))
		{
			ULONG ip;
			memcpy(&ip, Buffer, sizeof(ULONG));

			if (ip)
			{
				//DbgPrint("[%08x]->%08x\n", _DnsServerIp, ip);
				DnsCache::set(_crc, ip);
				_pTask->OnIp(ip);
				return;
			}
		}
		Buffer += x.len;
	}
}

void CTdiObject::DnsToIp(PCSTR Dns)
{
	ULONG crc, ip;
	PSTR c;
	if (0 <= RtlIpv4StringToAddressA(Dns, TRUE, c, ip) || (ip = DnsCache::get(Dns, crc)))
	{
		OnIp(ip);
		return;
	}

	if (strlen(Dns) > 256)
	{
		OnIp(0);
		return;
	}

	if (CDnsTask* pTask = new CDnsTask(this))
	{
		pTask->DnsToIp(Dns, crc);
		pTask->Release();
	}
	else
	{
		OnIp(0);
	}
}

_NT_END
