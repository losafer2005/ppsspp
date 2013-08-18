// Copyright (c) 2012- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include <algorithm>

#include "Core/Reporting.h"
#include "Core/CoreTiming.h"
#include "Core/HLE/HLE.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/sceKernelMsgPipe.h"
#include "Core/HLE/sceKernelMemory.h"
#include "Core/HLE/sceKernelThread.h"
#include "Common/ChunkFile.h"

#define SCE_KERNEL_MPA_THFIFO_S 0x0000
#define SCE_KERNEL_MPA_THPRI_S  0x0100
#define SCE_KERNEL_MPA_THFIFO_R 0x0000
#define SCE_KERNEL_MPA_THPRI_R  0x1000
#define SCE_KERNEL_MPA_HIGHMEM  0x4000
#define SCE_KERNEL_MPA_KNOWN    (SCE_KERNEL_MPA_THPRI_S | SCE_KERNEL_MPA_THPRI_R | SCE_KERNEL_MPA_HIGHMEM)

#define SCE_KERNEL_MPW_FULL 0
#define SCE_KERNEL_MPW_ASAP 1

// State: the timer for MsgPipe timeouts.
static int waitTimer = -1;

struct NativeMsgPipe
{
	SceSize_le size;
	char name[32];
	SceUInt_le attr;
	s32_le bufSize;
	s32_le freeSize;
	s32_le numSendWaitThreads;
	s32_le numReceiveWaitThreads;
};

struct MsgPipeWaitingThread
{
	SceUID id;
	u32 bufAddr;
	u32 bufSize;
	u32 freeSize;
	s32 waitMode;
	PSPPointer<u32_le> transferredBytes;

	bool IsStillWaiting(SceUID waitID) const
	{
		u32 error;
		int actualWaitID = __KernelGetWaitID(id, WAITTYPE_MSGPIPE, error);
		return actualWaitID == waitID;
	}

	void WriteCurrentTimeout(SceUID waitID) const
	{
		u32 error;
		if (IsStillWaiting(waitID))
		{
			u32 timeoutPtr = __KernelGetWaitTimeoutPtr(id, error);
			if (timeoutPtr != 0 && waitTimer != -1)
			{
				// Remove any event for this thread.
				s64 cyclesLeft = CoreTiming::UnscheduleEvent(waitTimer, id);
				Memory::Write_U32((u32) cyclesToUs(cyclesLeft), timeoutPtr);
			}
		}
	}

	void Complete(SceUID waitID, int result, u32 transferred = (u32)-1) const
	{
		if (IsStillWaiting(waitID))
		{
			WriteCurrentTimeout(waitID);
			if (transferred != (u32)-1 && transferredBytes.IsValid())
				*transferredBytes = transferred;
			__KernelResumeThreadFromWait(id, result);
		}
	}

	void Cancel(SceUID waitID, int result) const
	{
		Complete(waitID, result, 0);
	}

	void ReadBuffer(u8 *dest, u32 len)
	{
		Memory::Memcpy(dest, bufAddr, len);
		if (transferredBytes.IsValid())
			*transferredBytes += len;
	}

	void WriteBuffer(const u8 *src, u32 len)
	{
		Memory::Memcpy(bufAddr + (bufSize - freeSize), src, len);
		freeSize -= len;
		if (transferredBytes.IsValid())
			*transferredBytes += len;
	}
};

bool __KernelMsgPipeThreadSortPriority(MsgPipeWaitingThread thread1, MsgPipeWaitingThread thread2)
{
	return __KernelThreadSortPriority(thread1.id, thread2.id);
}

struct MsgPipe : public KernelObject
{
	const char *GetName() {return nmp.name;}
	const char *GetTypeName() {return "MsgPipe";}
	static u32 GetMissingErrorCode() { return SCE_KERNEL_ERROR_UNKNOWN_MPPID; }
	static int GetStaticIDType() { return SCE_KERNEL_TMID_Mpipe; }
	int GetIDType() const { return SCE_KERNEL_TMID_Mpipe; }

	MsgPipe() : buffer(0) {}
	~MsgPipe()
	{
		if (buffer != 0)
			userMemory.Free(buffer);
	}

	u32 GetUsedSize()
	{
		return (u32)(nmp.bufSize - nmp.freeSize);
	}

	void AddWaitingThread(std::vector<MsgPipeWaitingThread> &list, SceUID id, u32 addr, u32 size, int waitMode, u32 transferredBytesAddr)
	{
		MsgPipeWaitingThread thread = { id, addr, size, size, waitMode, transferredBytesAddr };
		// Start out with 0 transferred bytes while waiting.
		// TODO: for receive, it might be a different (partial) number.
		if (thread.transferredBytes.IsValid())
			*thread.transferredBytes = 0;

		list.push_back(thread);
	}

	void AddSendWaitingThread(SceUID id, u32 addr, u32 size, int waitMode, u32 transferredBytesAddr)
	{
		AddWaitingThread(sendWaitingThreads, id, addr, size, waitMode, transferredBytesAddr);
	}

	void AddReceiveWaitingThread(SceUID id, u32 addr, u32 size, int waitMode, u32 transferredBytesAddr)
	{
		AddWaitingThread(receiveWaitingThreads, id, addr, size, waitMode, transferredBytesAddr);
	}

	bool CheckSendThreads()
	{
		SortSendThreads();

		bool wokeThreads = false;
		bool filledSpace = false;
		while (!sendWaitingThreads.empty() && nmp.freeSize > 0)
		{
			MsgPipeWaitingThread *thread = &sendWaitingThreads.front();
			u32 bytesToSend = 0;
			if (thread->bufSize <= (u32) nmp.freeSize)
				bytesToSend = thread->bufSize;
			else if (thread->waitMode == SCE_KERNEL_MPW_ASAP && nmp.freeSize != 0)
				bytesToSend = nmp.freeSize;

			if (bytesToSend != 0)
			{
				thread->ReadBuffer(Memory::GetPointer(buffer + GetUsedSize()), bytesToSend);
				thread->Complete(GetUID(), 0);
				sendWaitingThreads.erase(sendWaitingThreads.begin());
				wokeThreads = true;
				filledSpace = true;
			}
			// Unlike receives, we don't do partial sends.  Stop at first blocked thread.
			else
				break;
		}

		if (filledSpace)
			wokeThreads |= CheckReceiveThreads();

		return wokeThreads;
	}

	// This function should be only ran when the temporary buffer size is not 0 (otherwise, data is copied directly to the threads)
	bool CheckReceiveThreads()
	{
		SortReceiveThreads();

		bool wokeThreads = false;
		bool freedSpace = false;
		while (!receiveWaitingThreads.empty() && GetUsedSize() > 0)
		{
			MsgPipeWaitingThread *thread = &receiveWaitingThreads.front();
			// Receive as much as possible, even if it's not enough to wake up.
			u32 bytesToSend = std::min(thread->freeSize, GetUsedSize());

			thread->WriteBuffer(Memory::GetPointer(buffer), bytesToSend);
			// Put the unused data at the start of the buffer.
			nmp.freeSize += bytesToSend;
			memmove(Memory::GetPointer(buffer), Memory::GetPointer(buffer) + bytesToSend, GetUsedSize());
			freedSpace = true;

			if (thread->waitMode == SCE_KERNEL_MPW_ASAP || thread->freeSize == 0)
			{
				thread->Complete(GetUID(), 0, thread->bufSize);
				receiveWaitingThreads.erase(receiveWaitingThreads.begin());
				wokeThreads = true;
				thread = NULL;
			}
			// Stop at the first that can't wake up.
			else
				break;
		}

		if (freedSpace)
			wokeThreads |= CheckSendThreads();

		return wokeThreads;
	}

	void SortReceiveThreads()
	{
		// Clean up any not waiting at the same time.
		size_t size = receiveWaitingThreads.size();
		for (size_t i = 0; i < size; ++i)
		{
			if (!receiveWaitingThreads[i].IsStillWaiting(GetUID()))
			{
				// Decrement size and swap what was there with i.
				std::swap(receiveWaitingThreads[i], receiveWaitingThreads[--size]);
				// Now we haven't checked the new i, so go back and do i again.
				--i;
			}
		}
		receiveWaitingThreads.resize(size);

		bool usePrio = (nmp.attr & SCE_KERNEL_MPA_THPRI_R) != 0;
		if (usePrio)
			std::stable_sort(receiveWaitingThreads.begin(), receiveWaitingThreads.end(), __KernelMsgPipeThreadSortPriority);
	}

	void SortSendThreads()
	{
		// Clean up any not waiting at the same time.
		size_t size = sendWaitingThreads.size();
		for (size_t i = 0; i < size; ++i)
		{
			if (!sendWaitingThreads[i].IsStillWaiting(GetUID()))
			{
				// Decrement size and swap what was there with i.
				std::swap(sendWaitingThreads[i], sendWaitingThreads[--size]);
				// Now we haven't checked the new i, so go back and do i again.
				--i;
			}
		}
		sendWaitingThreads.resize(size);

		bool usePrio = (nmp.attr & SCE_KERNEL_MPA_THPRI_S) != 0;
		if (usePrio)
			std::stable_sort(sendWaitingThreads.begin(), sendWaitingThreads.end(), __KernelMsgPipeThreadSortPriority);
	}

	virtual void DoState(PointerWrap &p)
	{
		p.Do(nmp);
		MsgPipeWaitingThread mpwt1 = {0}, mpwt2 = {0};
		p.Do(sendWaitingThreads, mpwt1);
		p.Do(receiveWaitingThreads, mpwt2);
		p.Do(buffer);
		p.DoMarker("MsgPipe");
	}

	NativeMsgPipe nmp;

	std::vector<MsgPipeWaitingThread> sendWaitingThreads;
	std::vector<MsgPipeWaitingThread> receiveWaitingThreads;

	u32 buffer;
};

KernelObject *__KernelMsgPipeObject()
{
	return new MsgPipe;
}

void __KernelMsgPipeTimeout(u64 userdata, int cyclesLate)
{
	SceUID threadID = (SceUID) (userdata & 0xFFFFFFFF);

	u32 error;
	u32 timeoutPtr = __KernelGetWaitTimeoutPtr(threadID, error);
	if (timeoutPtr != 0)
		Memory::Write_U32(0, timeoutPtr);

	SceUID uid = __KernelGetWaitID(threadID, WAITTYPE_MSGPIPE, error);
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (m)
	{
		// This thread isn't waiting anymore, but we'll remove it from waitingThreads later.
		// The reason is, if it times out, but whhile it was waiting on is DELETED prior to it
		// actually running, it will get a DELETE result instead of a TIMEOUT.
		// So, we need to remember it or we won't be able to mark it DELETE instead later.
		__KernelResumeThreadFromWait(threadID, SCE_KERNEL_ERROR_WAIT_TIMEOUT);
	}
}

bool __KernelSetMsgPipeTimeout(u32 timeoutPtr)
{
	if (timeoutPtr == 0 || waitTimer == -1)
		return true;

	int micro = (int) Memory::Read_U32(timeoutPtr);
	if (micro <= 2)
	{
		// Don't wait or reschedule, just timeout immediately.
		return false;
	}

	if (micro <= 210)
		micro = 250;
	CoreTiming::ScheduleEvent(usToCycles(micro), waitTimer, __KernelGetCurThread());
	return true;
}

void __KernelMsgPipeInit()
{
	waitTimer = CoreTiming::RegisterEvent("MsgPipeTimeout", __KernelMsgPipeTimeout);
}

void __KernelMsgPipeDoState(PointerWrap &p)
{
	p.Do(waitTimer);
	CoreTiming::RestoreRegisterEvent(waitTimer, "MsgPipeTimeout", __KernelMsgPipeTimeout);
	p.DoMarker("sceKernelMsgPipe");
}

int sceKernelCreateMsgPipe(const char *name, int partition, u32 attr, u32 size, u32 optionsPtr)
{
	if (!name)
	{
		WARN_LOG_REPORT(HLE, "%08x=sceKernelCreateMsgPipe(): invalid name", SCE_KERNEL_ERROR_NO_MEMORY);
		return SCE_KERNEL_ERROR_NO_MEMORY;
	}
	if (partition < 1 || partition > 9 || partition == 7)
	{
		WARN_LOG_REPORT(HLE, "%08x=sceKernelCreateMsgPipe(): invalid partition %d", SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT, partition);
		return SCE_KERNEL_ERROR_ILLEGAL_ARGUMENT;
	}
	// We only support user right now.
	if (partition != 2 && partition != 6)
	{
		WARN_LOG_REPORT(HLE, "%08x=sceKernelCreateMsgPipe(): invalid partition %d", SCE_KERNEL_ERROR_ILLEGAL_PERM, partition);
		return SCE_KERNEL_ERROR_ILLEGAL_PERM;
	}
	if ((attr & ~SCE_KERNEL_MPA_KNOWN) >= 0x100)
	{
		WARN_LOG_REPORT(HLE, "%08x=sceKernelCreateEventFlag(%s): invalid attr parameter: %08x", SCE_KERNEL_ERROR_ILLEGAL_ATTR, name, attr);
		return SCE_KERNEL_ERROR_ILLEGAL_ATTR;
	}

	u32 memBlockPtr = 0;
	if (size != 0)
	{
		// We ignore the upalign to 256.
		u32 allocSize = size;
		memBlockPtr = userMemory.Alloc(allocSize, (attr & SCE_KERNEL_MPA_HIGHMEM) != 0, "MsgPipe");
		if (memBlockPtr == (u32)-1)
		{
			ERROR_LOG(HLE, "%08x=sceKernelCreateEventFlag(%s): Failed to allocate %i bytes for buffer", SCE_KERNEL_ERROR_NO_MEMORY, name, size);
			return SCE_KERNEL_ERROR_NO_MEMORY;
		}
	}

	MsgPipe *m = new MsgPipe();
	SceUID id = kernelObjects.Create(m);

	m->nmp.size = sizeof(NativeMsgPipe);
	strncpy(m->nmp.name, name, KERNELOBJECT_MAX_NAME_LENGTH);
	m->nmp.name[KERNELOBJECT_MAX_NAME_LENGTH] = 0;
	m->nmp.attr = attr;
	m->nmp.bufSize = size;
	m->nmp.freeSize = size;
	m->nmp.numSendWaitThreads = 0;
	m->nmp.numReceiveWaitThreads = 0;

	m->buffer = memBlockPtr;
	
	DEBUG_LOG(HLE, "%d=sceKernelCreateMsgPipe(%s, part=%d, attr=%08x, size=%d, opt=%08x)", id, name, partition, attr, size, optionsPtr);

	if (optionsPtr != 0)
		WARN_LOG_REPORT(HLE, "sceKernelCreateMsgPipe(%s) unsupported options parameter: %08x", name, optionsPtr);

	return id;
}

int sceKernelDeleteMsgPipe(SceUID uid)
{
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m)
	{
		ERROR_LOG(HLE, "sceKernelDeleteMsgPipe(%i) - ERROR %08x", uid, error);
		return error;
	}

	for (size_t i = 0; i < m->sendWaitingThreads.size(); i++)
		m->sendWaitingThreads[i].Cancel(uid, SCE_KERNEL_ERROR_WAIT_DELETE);
	for (size_t i = 0; i < m->receiveWaitingThreads.size(); i++)
		m->receiveWaitingThreads[i].Cancel(uid, SCE_KERNEL_ERROR_WAIT_DELETE);

	DEBUG_LOG(HLE, "sceKernelDeleteMsgPipe(%i)", uid);
	return kernelObjects.Destroy<MsgPipe>(uid);
}

int __KernelSendMsgPipe(MsgPipe *m, u32 sendBufAddr, u32 sendSize, int waitMode, u32 resultAddr, u32 timeoutPtr, bool cbEnabled, bool poll)
{
	u32 curSendAddr = sendBufAddr;
	SceUID uid = m->GetUID();

	if (sendSize & 0x80000000)
	{
		ERROR_LOG(HLE, "__KernelSendMsgPipe(%d): illegal size %d", uid, sendSize);
		return SCE_KERNEL_ERROR_ILLEGAL_ADDR;
	}

	// If the buffer size is 0, nothing is buffered and all operations wait.
	if (m->nmp.bufSize == 0)
	{
		m->SortReceiveThreads();

		while (!m->receiveWaitingThreads.empty() && sendSize != 0)
		{
			MsgPipeWaitingThread *thread = &m->receiveWaitingThreads.front();

			u32 bytesToSend = std::min(thread->freeSize, sendSize);
			if (bytesToSend > 0)
			{
				thread->WriteBuffer(Memory::GetPointer(curSendAddr), bytesToSend);
				sendSize -= bytesToSend;
				curSendAddr += bytesToSend;

				if (thread->freeSize == 0 || thread->waitMode == SCE_KERNEL_MPW_ASAP)
				{
					thread->Complete(uid, 0);
					m->receiveWaitingThreads.erase(m->receiveWaitingThreads.begin());
					hleReSchedule(cbEnabled, "msgpipe data sent");
					thread = NULL;
				}
			}
		}

		// If there is still data to send and (we want to send all of it or we didn't send anything)
		if (sendSize != 0 && (waitMode != SCE_KERNEL_MPW_ASAP || curSendAddr == sendBufAddr))
		{
			if (poll)
				return SCE_KERNEL_ERROR_MPP_FULL;
			else
			{
				m->AddSendWaitingThread(__KernelGetCurThread(), curSendAddr, sendSize, waitMode, resultAddr);
				if (__KernelSetMsgPipeTimeout(timeoutPtr))
					__KernelWaitCurThread(WAITTYPE_MSGPIPE, uid, 0, timeoutPtr, cbEnabled, "msgpipe send waited");
				else
					return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
				return 0;
			}
		}
	}
	else
	{
		if (sendSize > (u32) m->nmp.bufSize)
		{
			ERROR_LOG(HLE, "__KernelSendMsgPipe(%d): size %d too large for buffer", uid, sendSize);
			return SCE_KERNEL_ERROR_ILLEGAL_SIZE;
		}

		u32 bytesToSend = 0;
		// If others are already waiting, space or not, we have to get in line.
		m->SortSendThreads();
		if (m->sendWaitingThreads.empty())
		{
			if (sendSize <= (u32) m->nmp.freeSize)
				bytesToSend = sendSize;
			else if (waitMode == SCE_KERNEL_MPW_ASAP && m->nmp.freeSize != 0)
				bytesToSend = m->nmp.freeSize;
		}

		if (bytesToSend != 0)
		{
			Memory::Memcpy(m->buffer + (m->nmp.bufSize - m->nmp.freeSize), Memory::GetPointer(sendBufAddr), bytesToSend);
			m->nmp.freeSize -= bytesToSend;
			curSendAddr += bytesToSend;
			sendSize -= bytesToSend;

			if (m->CheckReceiveThreads())
				hleReSchedule(cbEnabled, "msgpipe data sent");
		}
		else
		{
			if (poll)
				return SCE_KERNEL_ERROR_MPP_FULL;
			else
			{
				m->AddSendWaitingThread(__KernelGetCurThread(), curSendAddr, sendSize, waitMode, resultAddr);
				if (__KernelSetMsgPipeTimeout(timeoutPtr))
					__KernelWaitCurThread(WAITTYPE_MSGPIPE, uid, 0, timeoutPtr, cbEnabled, "msgpipe send waited");
				else
					return SCE_KERNEL_ERROR_WAIT_TIMEOUT;
				return 0;
			}
		}
	}

	// We didn't wait, so update the number of bytes transferred now.
	if (Memory::IsValidAddress(resultAddr))
		Memory::Write_U32(curSendAddr - sendBufAddr, resultAddr);

	return 0;
}

int sceKernelSendMsgPipe(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr)
{
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelSendMsgPipe(%i) - ERROR %08x", uid, error);
		return error;
	}

	DEBUG_LOG(HLE, "sceKernelSendMsgPipe(id=%i, addr=%08x, size=%i, mode=%i, result=%08x, timeout=%08x)", uid, sendBufAddr, sendSize, waitMode, resultAddr, timeoutPtr);
	return __KernelSendMsgPipe(m, sendBufAddr, sendSize, waitMode, resultAddr, timeoutPtr, false, false);
}

int sceKernelSendMsgPipeCB(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr, u32 timeoutPtr)
{
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelSendMsgPipeCB(%i) - ERROR %08x", uid, error);
		return error;
	}

	DEBUG_LOG(HLE, "sceKernelSendMsgPipeCB(id=%i, addr=%08x, size=%i, mode=%i, result=%08x, timeout=%08x)", uid, sendBufAddr, sendSize, waitMode, resultAddr, timeoutPtr);
	// TODO: Verify callback behavior.
	hleCheckCurrentCallbacks();
	return __KernelSendMsgPipe(m, sendBufAddr, sendSize, waitMode, resultAddr, timeoutPtr, true, false);
}

int sceKernelTrySendMsgPipe(SceUID uid, u32 sendBufAddr, u32 sendSize, u32 waitMode, u32 resultAddr)
{
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelTrySendMsgPipe(%i) - ERROR %08x", uid, error);
		return error;
	}

	DEBUG_LOG(HLE, "sceKernelTrySendMsgPipe(id=%i, addr=%08x, size=%i, mode=%i, result=%08x)", uid, sendBufAddr, sendSize, waitMode, resultAddr);
	return __KernelSendMsgPipe(m, sendBufAddr, sendSize, waitMode, resultAddr, 0, false, true);
}

void __KernelReceiveMsgPipe(MsgPipe *m, u32 receiveBufAddr, u32 receiveSize, int waitMode, u32 resultAddr, u32 timeoutPtr, bool cbEnabled, bool poll)
{
	u32 curReceiveAddr = receiveBufAddr;
	// MsgPipe buffer size is 0, receiving directly from waiting send threads
	if (m->nmp.bufSize == 0)
	{
		// While they're still sending waiting threads (which can send data)
		while (!m->sendWaitingThreads.empty())
		{
			MsgPipeWaitingThread *thread = &m->sendWaitingThreads.front();
			// Sending thread has more data than we have to receive: retrieve just the amount of data we want
			if (thread->bufSize - thread->freeSize > receiveSize)
			{
				Memory::Memcpy(curReceiveAddr, Memory::GetPointer(thread->bufAddr), receiveSize);
				thread->freeSize += receiveSize;
				// Move still available data at the beginning of the sending thread buffer
				Memory::Memcpy(thread->bufAddr, Memory::GetPointer(thread->bufAddr + receiveSize), thread->bufSize - thread->freeSize);
				curReceiveAddr += receiveSize;
				receiveSize = 0;
				// The sending thread mode is ASAP: we have sent some data so restart it even though its buffer isn't empty
				if (thread->waitMode == SCE_KERNEL_MPW_ASAP)
				{
					thread->Complete(m->GetUID(), 0, thread->bufSize - thread->freeSize);
					m->sendWaitingThreads.erase(m->sendWaitingThreads.begin());
				}
				break;
			}
			// Sending thread wants to send the same amount of data as we want to retrieve: get the data and resume thread
			else if (thread->bufSize - thread->freeSize == receiveSize)
			{
				Memory::Memcpy(curReceiveAddr, Memory::GetPointer(thread->bufAddr), receiveSize);
				thread->Complete(m->GetUID(), 0, thread->bufSize);
				m->sendWaitingThreads.erase(m->sendWaitingThreads.begin());
				curReceiveAddr += receiveSize;
				receiveSize = 0;
				break;
			}
			// Not enough data in the sending thread: get the data available and restart the sending thread, then loop
			else
			{
				Memory::Memcpy(curReceiveAddr, Memory::GetPointer(thread->bufAddr), thread->bufSize - thread->freeSize);
				receiveSize -= thread->bufSize - thread->freeSize;
				curReceiveAddr += thread->bufSize - thread->freeSize;
				thread->Complete(m->GetUID(), 0, thread->bufSize);
				m->sendWaitingThreads.erase(m->sendWaitingThreads.begin());
			}
		}
		// All data hasn't been received and (mode isn't ASAP or nothing was received)
		if (receiveSize != 0 && (waitMode != SCE_KERNEL_MPW_ASAP || curReceiveAddr == receiveBufAddr))
		{
			if (poll)
			{
				RETURN(SCE_KERNEL_ERROR_MPP_EMPTY);
				return;
			}
			else
			{
				m->AddReceiveWaitingThread(__KernelGetCurThread(), curReceiveAddr, receiveSize, waitMode, resultAddr);
				RETURN(0);
				if (__KernelSetMsgPipeTimeout(timeoutPtr))
					__KernelWaitCurThread(WAITTYPE_MSGPIPE, m->GetUID(), 0, timeoutPtr, cbEnabled, "msgpipe waited");
				else
					RETURN(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
				return;
			}
		}
	}
	// Getting data from the MsgPipe buffer
	else
	{
		// Enough data in the buffer: copy just the needed amount of data
		if (receiveSize <= (u32) m->nmp.bufSize - (u32) m->nmp.freeSize)
		{
			Memory::Memcpy(receiveBufAddr, Memory::GetPointer(m->buffer), receiveSize);
			m->nmp.freeSize += receiveSize;
			memmove(Memory::GetPointer(m->buffer), Memory::GetPointer(m->buffer) + receiveSize, m->nmp.bufSize - m->nmp.freeSize);
			curReceiveAddr = receiveBufAddr + receiveSize;
			receiveSize = 0;
		}
		// Else, if mode is ASAP and there's at list 1 available byte of data: copy all the available data
		else if (waitMode == SCE_KERNEL_MPW_ASAP && m->nmp.freeSize != m->nmp.bufSize)
		{
			Memory::Memcpy(receiveBufAddr, Memory::GetPointer(m->buffer), m->nmp.bufSize - m->nmp.freeSize);
			receiveSize -= m->nmp.bufSize - m->nmp.freeSize;
			curReceiveAddr = receiveBufAddr + m->nmp.bufSize - m->nmp.freeSize;
			m->nmp.freeSize = m->nmp.bufSize;
		}
		else
		{
			if (poll)
			{
				RETURN(SCE_KERNEL_ERROR_MPP_EMPTY);
				return;
			}
			else
			{
				m->AddReceiveWaitingThread(__KernelGetCurThread(), curReceiveAddr, receiveSize, waitMode, resultAddr);
				RETURN(0);
				if (__KernelSetMsgPipeTimeout(timeoutPtr))
					__KernelWaitCurThread(WAITTYPE_MSGPIPE, m->GetUID(), 0, timeoutPtr, cbEnabled, "msgpipe waited");
				else
					RETURN(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
				return;
			}
		}

		if (curReceiveAddr != receiveBufAddr)
		{
			m->CheckSendThreads();
		}
	}
	Memory::Write_U32(curReceiveAddr - receiveBufAddr, resultAddr);

	RETURN(0);
}

void sceKernelReceiveMsgPipe()
{
	SceUID uid = PARAM(0);
	u32 receiveBufAddr = PARAM(1);
	u32 receiveSize = PARAM(2);
	int waitMode = PARAM(3);
	u32 resultAddr = PARAM(4);
	u32 timeoutPtr = PARAM(5);

	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelReceiveMsgPipe(%i) - ERROR %08x", uid, error);
		RETURN(error);
		return;
	}

	DEBUG_LOG(HLE, "sceKernelReceiveMsgPipe(%i, %08x, %i, %i, %08x, %08x)", uid, receiveBufAddr, receiveSize, waitMode, resultAddr, timeoutPtr);
	__KernelReceiveMsgPipe(m, receiveBufAddr, receiveSize, waitMode, resultAddr, timeoutPtr, false, false);
}

void sceKernelReceiveMsgPipeCB()
{
	SceUID uid = PARAM(0);
	u32 receiveBufAddr = PARAM(1);
	u32 receiveSize = PARAM(2);
	int waitMode = PARAM(3);
	u32 resultAddr = PARAM(4);
	u32 timeoutPtr = PARAM(5);

	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelReceiveMsgPipeCB(%i) - ERROR %08x", uid, error);
		RETURN(error);
		return;
	}

	DEBUG_LOG(HLE, "sceKernelReceiveMsgPipeCB(%i, %08x, %i, %i, %08x, %08x)", uid, receiveBufAddr, receiveSize, waitMode, resultAddr, timeoutPtr);
	__KernelReceiveMsgPipe(m, receiveBufAddr, receiveSize, waitMode, resultAddr, timeoutPtr, true, false);
}

void sceKernelTryReceiveMsgPipe()
{
	SceUID uid = PARAM(0);
	u32 receiveBufAddr = PARAM(1);
	u32 receiveSize = PARAM(2);
	int waitMode = PARAM(3);
	u32 resultAddr = PARAM(4);

	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m) {
		ERROR_LOG(HLE, "sceKernelTryReceiveMsgPipe(%i) - ERROR %08x", uid, error);
		RETURN(error);
		return;
	}

	DEBUG_LOG(HLE, "sceKernelTryReceiveMsgPipe(%i, %08x, %i, %i, %08x)", uid, receiveBufAddr, receiveSize, waitMode, resultAddr);
	__KernelReceiveMsgPipe(m, receiveBufAddr, receiveSize, waitMode, resultAddr, 0, false, true);
}

int sceKernelCancelMsgPipe(SceUID uid, u32 numSendThreadsAddr, u32 numReceiveThreadsAddr)
{
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (!m)
	{
		ERROR_LOG(HLE, "sceKernelCancelMsgPipe(%i) - ERROR %08x", uid, error);
		return error;
	}

	if (Memory::IsValidAddress(numSendThreadsAddr))
		Memory::Write_U32((u32) m->sendWaitingThreads.size(), numSendThreadsAddr);
	if (Memory::IsValidAddress(numReceiveThreadsAddr))
		Memory::Write_U32((u32) m->receiveWaitingThreads.size(), numReceiveThreadsAddr);

	for (size_t i = 0; i < m->sendWaitingThreads.size(); i++)
		m->sendWaitingThreads[i].Cancel(uid, SCE_KERNEL_ERROR_WAIT_CANCEL);
	m->sendWaitingThreads.clear();
	for (size_t i = 0; i < m->receiveWaitingThreads.size(); i++)
		m->receiveWaitingThreads[i].Cancel(uid, SCE_KERNEL_ERROR_WAIT_CANCEL);
	m->receiveWaitingThreads.clear();

	// And now the entire buffer is free.
	m->nmp.freeSize = m->nmp.bufSize;

	DEBUG_LOG(HLE, "sceKernelCancelMsgPipe(%i, %i, %i)", uid, numSendThreadsAddr, numReceiveThreadsAddr);
	return 0;
}

void sceKernelReferMsgPipeStatus()
{
	SceUID uid = PARAM(0);
	u32 msgPipeStatusAddr = PARAM(1);

	DEBUG_LOG(HLE,"sceKernelReferMsgPipeStatus(%i, %08x)", uid, msgPipeStatusAddr);
	u32 error;
	MsgPipe *m = kernelObjects.Get<MsgPipe>(uid, error);
	if (m)
	{
		m->nmp.numSendWaitThreads = (int) m->sendWaitingThreads.size();
		m->nmp.numReceiveWaitThreads = (int) m->receiveWaitingThreads.size();
		Memory::WriteStruct(msgPipeStatusAddr, &m->nmp);
		RETURN(0);
	}
	else
	{
		RETURN(error);
	}
}
