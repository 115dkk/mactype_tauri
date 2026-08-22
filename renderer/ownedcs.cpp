#include "ownedcs.h"
#include "windows.h"

void WINAPI InitializeOwnedCritialSection(POWNED_CRITIAL_SECTION cs)
{
	cs->nOwner = -1;
	cs->nRecursiveCount = 0;
	cs->nRequests = -1;
	cs->hEvent = CreateEvent(nullptr, false, false, nullptr);
	InitializeCriticalSection(&cs->threadLock);
}

void WINAPI DeleteOwnedCritialSection(POWNED_CRITIAL_SECTION cs)
{
	CloseHandle(cs->hEvent);
	DeleteCriticalSection(&cs->threadLock);
}


void WINAPI EnterOwnedCritialSection(POWNED_CRITIAL_SECTION cs, WORD Owner)
{
	EnterCriticalSection(&cs->threadLock);
	if (cs->nOwner == Owner)
	{
		InterlockedIncrement(&cs->nRecursiveCount);
		LeaveCriticalSection(&cs->threadLock);
	}
	else
	{
		if (InterlockedIncrement(&cs->nRequests)>0)  //等待获取所有权
		{
			LeaveCriticalSection(&cs->threadLock);
			WaitForSingleObject(cs->hEvent, INFINITE);
		}
		else
			LeaveCriticalSection(&cs->threadLock);
		InterlockedExchange(&cs->nOwner, Owner);//更改所有者
		InterlockedExchange(&cs->nRecursiveCount, 1);//增加占用计数
	}
}

void WINAPI LeaveOwnedCritialSection(POWNED_CRITIAL_SECTION cs, WORD Owner)
{
	EnterCriticalSection(&cs->threadLock);
	if (cs->nOwner == Owner)
	{
		if (InterlockedDecrement(&cs->nRecursiveCount)<=0)
		{
			InterlockedExchange(&cs->nOwner, -1);//归还所有权
			if (InterlockedDecrement(&cs->nRequests)>=0)
				SetEvent(cs->hEvent);
		}
	}
	else
		InterlockedDecrement(&cs->nRecursiveCount);
	LeaveCriticalSection(&cs->threadLock);
}
