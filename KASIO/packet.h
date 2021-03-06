#pragma once

#ifdef DBG_PRINT
extern LONG gnPackets;// $$$
#endif

class CDataPacket
{
private:
	LONG m_nRef;
	DWORD m_BufferSize, m_DataSize, m_Pad;
	char m_Data[];

	enum ST : size_t {};

	CDataPacket(DWORD BufferSize) : m_nRef(1), m_BufferSize(BufferSize), m_DataSize(0), m_Pad(0) { }

	void* operator new(size_t ByteSize, ST BufferSize)
	{
		return ExAllocatePool(PagedPool, ByteSize + BufferSize);
	}

	void operator delete(void* p)
	{
		ExFreePool(p);
	}

#ifdef DBG_PRINT
	~CDataPacket() { _InterlockedDecrement(&gnPackets);  }//$$$
#endif

public:

	CDataPacket() 
	{ 
#ifdef DBG_PRINT
		_InterlockedIncrement(&gnPackets); // $$$
#endif
	}

	void AddRef()
	{
		InterlockedIncrement(&m_nRef);
	}

	void Release()
	{
		if (!InterlockedDecrement(&m_nRef)) delete this;
	}

	void* operator new(size_t , DWORD BufferSize)
	{
		return new((ST)BufferSize) CDataPacket(BufferSize);
	}

	PSTR getFreeBuffer()
	{
		return m_Data + m_DataSize;
	}

	DWORD getFreeSize()
	{
		return m_BufferSize - m_DataSize;
	}

	PSTR getData()
	{
		return m_Data;
	}

	DWORD getBufferSize()
	{
		return m_BufferSize;
	}

	DWORD getDataSize()
	{
		return m_DataSize;
	}

	DWORD setDataSize(DWORD DataSize)
	{
		return m_DataSize = DataSize;
	}

	DWORD addData(DWORD DataSize)
	{
		return m_DataSize += DataSize;
	}

	DWORD decData(DWORD DataSize)
	{
		return m_DataSize -= DataSize;
	}

	DWORD addData(PVOID pvData, DWORD cbData)
	{
		PVOID to = m_Data + m_DataSize;
		if (to != pvData) memcpy(to, pvData, cbData);
		return m_DataSize += cbData;
	}

	void removeData(DWORD DataSize)
	{
		memcpy(m_Data, m_Data + DataSize, m_DataSize -= DataSize);
	}

	void reservBuffer(DWORD d)
	{
		m_BufferSize -= d;
	}

	void setPad(ULONG pad)
	{
		m_Pad = pad;
	}

	ULONG getPad()
	{
		return m_Pad;
	}
};