#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketizedRDMA==1 && _RAKNET_SUPPORT_RDMAInterface==1

#include "PacketizedRDMA.h"
#include "NativeTypes.h"
#include "BitStream.h"
#include "MessageIdentifiers.h"
#include "RakAlloca.h"
#include "GetTime.h"
#include "RakSleep.h"

using namespace RakNet;

typedef uint32_t PRDMAHeader;

STATIC_FACTORY_DEFINITIONS(PacketizedRDMA, PacketizedRDMA);

PacketizedRDMA::PacketizedRDMA()
{
}

PacketizedRDMA::~PacketizedRDMA()
{
	ClearAllConnections();
}

void PacketizedRDMA::Stop(void)
{
	unsigned int i;
	RDMAInterface::Stop();
	for (i = 0; i < waitingPackets.Size(); i++)
		DeallocatePacket(waitingPackets[i]);
	ClearAllConnections();
}

void PacketizedRDMA::Send(const char* data, unsigned length, const SystemAddress& systemAddress, bool broadcast)
{
	// Prefix with 4-byte length header
	PRDMAHeader dataLength = length;
	char stack_data[256];
	char* allocatedData = nullptr;
	char* sendData;

	if (length + sizeof(PRDMAHeader) <= sizeof(stack_data))
	{
		sendData = stack_data;
	}
	else
	{
		allocatedData = (char*)rakMalloc_Ex(length + sizeof(PRDMAHeader), _FILE_AND_LINE_);
		sendData = allocatedData;
	}

	memcpy(sendData, &dataLength, sizeof(PRDMAHeader));
	memcpy(sendData + sizeof(PRDMAHeader), data, length);

	// Send through base RDMA interface
	RDMAInterface::Send(sendData, length + sizeof(PRDMAHeader), systemAddress, broadcast);

	if (allocatedData)
		rakFree_Ex(allocatedData, _FILE_AND_LINE_);
}

bool PacketizedRDMA::SendList(const char** data, const unsigned int* lengths, const int numParameters,
                              const SystemAddress& systemAddress, bool broadcast)
{
	if (numParameters == 0)
		return false;

	unsigned int totalLength = 0;
	unsigned int lengthOffset;
	int i;
	for (i = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
			totalLength += lengths[i];
	}

	if (totalLength == 0)
		return false;

	// Allocate space for header + all data
	char* dataAggregate = (char*)rakMalloc_Ex(totalLength + sizeof(PRDMAHeader), _FILE_AND_LINE_);
	
	// Write length header
	PRDMAHeader dataLength = totalLength;
	memcpy(dataAggregate, &dataLength, sizeof(PRDMAHeader));

	// Concatenate all data after header
	lengthOffset = sizeof(PRDMAHeader);
	for (i = 0; i < numParameters; i++)
	{
		if (lengths[i] > 0)
		{
			memcpy(dataAggregate + lengthOffset, data[i], lengths[i]);
			lengthOffset += lengths[i];
		}
	}

	// Send through base RDMA interface
	RDMAInterface::Send(dataAggregate, totalLength + sizeof(PRDMAHeader), systemAddress, broadcast);

	rakFree_Ex(dataAggregate, _FILE_AND_LINE_);
	return true;
}

void PacketizedRDMA::PushNotificationsToQueues(void)
{
	SystemAddress sa;
	
	// Process new connections
	sa = RDMAInterface::HasNewIncomingConnection();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		AddToConnectionList(sa);
		_newIncomingConnections.Push(sa, _FILE_AND_LINE_);
		sa = RDMAInterface::HasNewIncomingConnection();
	}

	// Process completed connection attempts
	sa = RDMAInterface::HasCompletedConnectionAttempt();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		AddToConnectionList(sa);
		_completedConnectionAttempts.Push(sa, _FILE_AND_LINE_);
		sa = RDMAInterface::HasCompletedConnectionAttempt();
	}

	// Process failed connection attempts
	sa = RDMAInterface::HasFailedConnectionAttempt();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		_failedConnectionAttempts.Push(sa, _FILE_AND_LINE_);
		sa = RDMAInterface::HasFailedConnectionAttempt();
	}

	// Process lost connections
	sa = RDMAInterface::HasLostConnection();
	while (sa != UNASSIGNED_SYSTEM_ADDRESS)
	{
		RemoveFromConnectionList(sa);
		_lostConnections.Push(sa, _FILE_AND_LINE_);
		sa = RDMAInterface::HasLostConnection();
	}
}

Packet* PacketizedRDMA::Receive(void)
{
	PushNotificationsToQueues();

	unsigned int i;
	Packet* packet;
	
	// Return any waiting complete packets first
	if (waitingPackets.Size() > 0)
	{
		packet = waitingPackets[0];
		waitingPackets.RemoveAtIndex(0);
		return packet;
	}

	// Process incoming data from base interface
	packet = RDMAInterface::Receive();
	if (packet == nullptr)
		return nullptr;

	// Get or create byte queue for this connection
	SystemAddress sa = packet->systemAddress;
	DataStructures::ByteQueue* bq;
	
	if (!connections.Has(sa))
	{
		AddToConnectionList(sa);
	}
	
	bq = connections.Get(sa);
	if (bq == nullptr)
	{
		DeallocatePacket(packet);
		return nullptr;
	}

	// Add received data to byte queue
	bq->WriteBytes((const char*)packet->data, packet->length, _FILE_AND_LINE_);
	DeallocatePacket(packet);

	// Try to extract complete packets
	return ReturnOutgoingPacket();
}

Packet* PacketizedRDMA::ReturnOutgoingPacket(void)
{
	// Iterate through all connections looking for complete packets
	for (unsigned int connectionIndex = 0; connectionIndex < connections.Size(); connectionIndex++)
	{
		DataStructures::ByteQueue* bq = connections[connectionIndex];
		SystemAddress sa = connections.GetKeyAtIndex(connectionIndex);

		if (bq->GetBytesWritten() < sizeof(PRDMAHeader))
			continue;

		// Peek at the length header
		PRDMAHeader dataLength;
		bq->ReadBytes((char*)&dataLength, sizeof(PRDMAHeader), true);

		// Check if we have the complete packet
		if (bq->GetBytesWritten() >= (unsigned)(dataLength + sizeof(PRDMAHeader)))
		{
			// Remove the header
			bq->IncrementReadOffset(sizeof(PRDMAHeader));

			// Allocate packet
			Packet* packet = AllocatePacket(dataLength);
			
			// Read the data
			bq->ReadBytes((char*)packet->data, dataLength, false);
			
			packet->systemAddress = sa;
			packet->guid = UNASSIGNED_RAKNET_GUID;
			packet->length = dataLength;
			packet->bitSize = BYTES_TO_BITS(dataLength);

			return packet;
		}
	}

	return nullptr;
}

void PacketizedRDMA::CloseConnection(SystemAddress systemAddress)
{
	RemoveFromConnectionList(systemAddress);
	RDMAInterface::CloseConnection(systemAddress);
}

void PacketizedRDMA::RemoveFromConnectionList(const SystemAddress& sa)
{
	if (sa == UNASSIGNED_SYSTEM_ADDRESS)
		return;
		
	if (connections.Has(sa))
	{
		unsigned int index = connections.GetIndexAtKey(sa);
		if (index != (unsigned int)-1)
		{
			RakNet::OP_DELETE(connections[index], _FILE_AND_LINE_);
			connections.RemoveAtIndex(index);
		}
	}
}

void PacketizedRDMA::AddToConnectionList(const SystemAddress& sa)
{
	if (sa == UNASSIGNED_SYSTEM_ADDRESS)
		return;
		
	connections.SetNew(sa, RakNet::OP_NEW<DataStructures::ByteQueue>(_FILE_AND_LINE_));
}

void PacketizedRDMA::ClearAllConnections(void)
{
	unsigned int i;
	for (i = 0; i < connections.Size(); i++)
		RakNet::OP_DELETE(connections[i], _FILE_AND_LINE_);
	connections.Clear();
}

SystemAddress PacketizedRDMA::HasCompletedConnectionAttempt(void)
{
	PushNotificationsToQueues();

	if (_completedConnectionAttempts.IsEmpty() == false)
		return _completedConnectionAttempts.Pop();
	return UNASSIGNED_SYSTEM_ADDRESS;
}

SystemAddress PacketizedRDMA::HasFailedConnectionAttempt(void)
{
	PushNotificationsToQueues();

	if (_failedConnectionAttempts.IsEmpty() == false)
		return _failedConnectionAttempts.Pop();
	return UNASSIGNED_SYSTEM_ADDRESS;
}

SystemAddress PacketizedRDMA::HasNewIncomingConnection(void)
{
	PushNotificationsToQueues();

	if (_newIncomingConnections.IsEmpty() == false)
		return _newIncomingConnections.Pop();
	return UNASSIGNED_SYSTEM_ADDRESS;
}

SystemAddress PacketizedRDMA::HasLostConnection(void)
{
	PushNotificationsToQueues();

	if (_lostConnections.IsEmpty() == false)
		return _lostConnections.Pop();
	return UNASSIGNED_SYSTEM_ADDRESS;
}

SystemAddress PacketizedRDMA::WaitForConnectionAttempt(unsigned int timeoutMS)
{
	TimeMS startTime = GetTimeMS();
	
	while (GetTimeMS() - startTime < timeoutMS)
	{
		SystemAddress completedConn = HasCompletedConnectionAttempt();
		if (completedConn != UNASSIGNED_SYSTEM_ADDRESS)
			return completedConn;
		
		SystemAddress failedConn = HasFailedConnectionAttempt();
		if (failedConn != UNASSIGNED_SYSTEM_ADDRESS)
			return UNASSIGNED_SYSTEM_ADDRESS;
		
		RakSleep(100);
	}
	
	return UNASSIGNED_SYSTEM_ADDRESS; // Timeout
}

#endif // _RAKNET_SUPPORT_PacketizedRDMA==1 && _RAKNET_SUPPORT_RDMAInterface==1

