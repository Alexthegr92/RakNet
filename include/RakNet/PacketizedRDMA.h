#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_PacketizedRDMA==1 && _RAKNET_SUPPORT_RDMAInterface==1

#ifndef __PACKETIZED_RDMA_H
#define __PACKETIZED_RDMA_H

#include "RDMAInterface.h"
#include "DS_ByteQueue.h"
#include "DS_Map.h"

namespace RakNet
{

/// \brief Adds packet framing to RDMAInterface
/// Automatically prefixes each message with a 4-byte length header
/// Handles message boundaries when using RDMA datagram modes
class RAK_DLL_EXPORT PacketizedRDMA : public RDMAInterface
{
public:
	// GetInstance() and DestroyInstance(instance*)
	STATIC_FACTORY_DECLARATIONS(PacketizedRDMA)

	PacketizedRDMA();
	virtual ~PacketizedRDMA();

	/// Stops the RDMA server
	void Stop(void);

	/// Sends a byte stream with packet framing
	void Send(const char* data, unsigned length, const SystemAddress& systemAddress, bool broadcast);

	/// Sends a concatenated list of byte streams with packet framing
	bool SendList(const char** data, const unsigned int* lengths, const int numParameters, 
	              const SystemAddress& systemAddress, bool broadcast);

	/// Returns data received (handles deframing)
	Packet* Receive(void);

	/// Disconnects a player/address
	void CloseConnection(SystemAddress systemAddress);

	/// Has a previous call to connect succeeded?
	SystemAddress HasCompletedConnectionAttempt(void);

	/// Has a previous call to connect failed?
	SystemAddress HasFailedConnectionAttempt(void);

	/// Queued events of new incoming connections
	SystemAddress HasNewIncomingConnection(void);

	/// Queued events of lost connections
	SystemAddress HasLostConnection(void);

protected:
	void PushNotificationsToQueues(void);
	Packet* ReturnOutgoingPacket(void);
	void RemoveFromConnectionList(const SystemAddress& sa);
	void AddToConnectionList(const SystemAddress& sa);
	void ClearAllConnections(void);

	DataStructures::List<Packet*> waitingPackets;
	DataStructures::Map<SystemAddress, DataStructures::ByteQueue*> connections;

	DataStructures::Queue<SystemAddress> _completedConnectionAttempts;
	DataStructures::Queue<SystemAddress> _failedConnectionAttempts;
	DataStructures::Queue<SystemAddress> _newIncomingConnections;
	DataStructures::Queue<SystemAddress> _lostConnections;
};

} // namespace RakNet

#endif // __PACKETIZED_RDMA_H

#endif // _RAKNET_SUPPORT_PacketizedRDMA==1 && _RAKNET_SUPPORT_RDMAInterface==1
