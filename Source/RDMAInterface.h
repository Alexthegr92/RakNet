/// \file
/// \brief RDMA-based transport using libfabric for ultra-low latency networking
/// Suitable for datacenter-to-datacenter communication and distributed physics simulation
///

#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_RDMAInterface==1

#ifndef __RDMA_INTERFACE_H
#define __RDMA_INTERFACE_H

#include "RakMemoryOverride.h"
#include "DS_List.h"
#include "RakNetTypes.h"
#include "Export.h"
#include "RakThread.h"
#include "DS_Queue.h"
#include "SimpleMutex.h"
#include "RakNetDefines.h"
#include "DS_ByteQueue.h"
#include "DS_ThreadsafeAllocatingQueue.h"
#include "LocklessTypes.h"
#include "PluginInterface2.h"

// libfabric includes
#include <rdma/fabric.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_eq.h>
#include <rdma/fi_errno.h>

namespace RakNet
{

/// Forward declarations
struct RDMARemoteClient;

/// RDMA connection modes
enum RDMAConnectionMode
{
	/// Reliable Connected - Similar to TCP, provides ordering and reliability
	RDMA_MODE_RC,
	
	/// Unreliable Datagram - Similar to UDP, no ordering or reliability guarantees
	RDMA_MODE_UD,
	
	/// Reliable Datagram - Datagram with reliability
	RDMA_MODE_RD
};

/// RDMA buffer pool for pre-registered memory regions
struct RDMABufferPool
{
	static const unsigned int BUFFER_SIZE = 65536;  // 64KB buffers
	static const unsigned int POOL_SIZE = 1024;     // 1024 buffers
	
	struct RDMABuffer
	{
		char* data;
		struct fid_mr* mr;              // Memory region handle
		bool inUse;
		RDMABuffer() : data(nullptr), mr(nullptr), inUse(false) {}
	};
	
	RDMABuffer buffers[POOL_SIZE];
	SimpleMutex poolMutex;
	
	RDMABuffer* AllocateBuffer();
	void FreeBuffer(RDMABuffer* buffer);
	void RegisterBuffers(struct fid_domain* domain);
	void DeregisterBuffers();
};

/// \brief RDMA-based networking interface using libfabric
/// Provides ultra-low latency communication for datacenter environments
/// Follows the same patterns as TCPInterface
class RAK_DLL_EXPORT RDMAInterface
{
public:
	// GetInstance() and DestroyInstance(instance*)
	STATIC_FACTORY_DECLARATIONS(RDMAInterface)

	RDMAInterface();
	virtual ~RDMAInterface();

	/// Starts the RDMA server on the indicated port
	/// \param[in] port Which port to listen on
	/// \param[in] maxIncomingConnections Max incoming connections we will accept
	/// \param[in] maxConnections Max total connections, which should be >= maxIncomingConnections
	/// \param[in] threadPriority Passed to the thread creation routine
	/// \param[in] connectionMode RDMA connection mode (RC, UD, or RD)
	/// \param[in] providerName Optional fabric provider name (e.g., "verbs", "sockets", "psm2")
	/// \param[in] bindAddress Optional address to bind to
	bool Start(unsigned short port, 
	           unsigned short maxIncomingConnections, 
	           unsigned short maxConnections=0, 
	           int threadPriority=-99999,
	           RDMAConnectionMode connectionMode=RDMA_MODE_RC,
	           const char* providerName=nullptr,
	           const char* bindAddress=nullptr);

	/// Stops the RDMA server
	void Stop(void);

	/// Connect to the specified host on the specified port
	/// \param[in] host Hostname or IP address
	/// \param[in] remotePort Port to connect to
	/// \param[in] block Whether to block until connection completes
	/// \return SystemAddress of the connection, or UNASSIGNED_SYSTEM_ADDRESS on failure
	SystemAddress Connect(const char* host, unsigned short remotePort, bool block=true);

	/// Sends a byte stream via RDMA
	/// \param[in] data Data to send
	/// \param[in] length Length of data
	/// \param[in] systemAddress Target address
	/// \param[in] broadcast Whether to broadcast to all connections
	virtual void Send(const char* data, unsigned int length, const SystemAddress& systemAddress, bool broadcast);

	/// Sends a concatenated list of byte streams
	virtual bool SendList(const char** data, const unsigned int* lengths, const int numParameters, 
	                      const SystemAddress& systemAddress, bool broadcast);

	/// Get how many bytes are waiting to be sent
	unsigned int GetOutgoingDataBufferSize(SystemAddress systemAddress) const;

	/// Returns if Receive() will return data
	virtual bool ReceiveHasPackets(void);

	/// Returns data received
	virtual Packet* Receive(void);

	/// Disconnects a client
	void CloseConnection(SystemAddress systemAddress);

	/// Deallocates a packet returned by Receive
	void DeallocatePacket(Packet* packet);

	/// Get list of connected systems
	void GetConnectionList(SystemAddress* remoteSystems, unsigned short* numberOfSystems) const;

	/// Returns just the number of connections
	unsigned short GetConnectionCount(void) const;

	/// Has a previous call to connect succeeded?
	SystemAddress HasCompletedConnectionAttempt(void);

	/// Has a previous call to connect failed?
	SystemAddress HasFailedConnectionAttempt(void);

	/// Queued events of new incoming connections
	SystemAddress HasNewIncomingConnection(void);

	/// Queued events of lost connections
	SystemAddress HasLostConnection(void);

	/// Return an allocated but empty packet
	Packet* AllocatePacket(unsigned dataSize);

	/// Push a packet back to the queue
	virtual void PushBackPacket(Packet* packet, bool pushAtHead);

	/// Returns if Start() was called successfully
	bool WasStarted(void) const;

	/// Get the current connection mode
	RDMAConnectionMode GetConnectionMode(void) const { return connectionMode; }

	/// Attach a plugin for message processing
	void AttachPlugin(PluginInterface2* plugin);
	
	/// Detach a plugin
	void DetachPlugin(PluginInterface2* plugin);

protected:
	Packet* ReceiveInt(void);
	
	bool CreateListenEndpoint(unsigned short port, const char* hostAddress);
	bool InitializeFabric(const char* providerName);
	void CleanupFabric();
	
	// Plugins
	DataStructures::List<PluginInterface2*> messageHandlerList;
	
	// Thread safety
	RakNet::LocklessUint32_t isStarted, threadRunning;
	SimpleMutex completedConnectionAttemptMutex, failedConnectionAttemptMutex;
	
	// libfabric handles
	struct fi_info* hints;
	struct fi_info* fabric_info;
	struct fid_fabric* fabric;
	struct fid_domain* domain;
	struct fid_pep* listener;          // Passive endpoint for listening
	struct fid_eq* event_queue;        // Event queue
	struct fid_cq* completion_queue;   // Completion queue
	
	// Connection management
	RDMARemoteClient* remoteClients;
	int remoteClientsLength;
	
	// Message queues
	DataStructures::Queue<Packet*> headPush, tailPush;
	DataStructures::ThreadsafeAllocatingQueue<Packet> incomingMessages;
	DataStructures::ThreadsafeAllocatingQueue<SystemAddress> newIncomingConnections, lostConnections, requestedCloseConnections;
	DataStructures::ThreadsafeAllocatingQueue<RDMARemoteClient*> newRemoteClients;
	DataStructures::Queue<SystemAddress> completedConnectionAttempts, failedConnectionAttempts;
	
	// Configuration
	int threadPriority;
	RDMAConnectionMode connectionMode;
	unsigned short maxConnections;
	
	// Buffer pool for zero-copy transfers
	RDMABufferPool bufferPool;
	
	// Thread function declarations
	friend RAK_THREAD_DECLARATION(UpdateRDMAInterfaceLoop);
	friend RAK_THREAD_DECLARATION(RDMAConnectionAttemptLoop);
	
	struct ThisPtrPlusSysAddr
	{
		RDMAInterface* rdmaInterface;
		SystemAddress systemAddress;
		char bindAddress[64];
	};
};

/// Stores information about a remote RDMA client
struct RDMARemoteClient
{
	RDMARemoteClient() 
	{
		endpoint = nullptr;
		isActive = false;
		sendBuffer = nullptr;
		recvBuffer = nullptr;
	}
	
	struct fid_ep* endpoint;           // RDMA endpoint
	SystemAddress systemAddress;
	DataStructures::ByteQueue outgoingData;
	bool isActive;
	SimpleMutex outgoingDataMutex;
	SimpleMutex isActiveMutex;
	
	// Pre-registered RDMA buffers
	RDMABufferPool::RDMABuffer* sendBuffer;
	RDMABufferPool::RDMABuffer* recvBuffer;
	
	int Send(const char* data, unsigned int length);
	int Recv(char* data, const int dataSize);
	
	void Reset(void)
	{
		outgoingDataMutex.Lock();
		outgoingData.Clear(_FILE_AND_LINE_);
		outgoingDataMutex.Unlock();
	}
	
	void SetActive(bool a);
	void SendOrBuffer(const char** data, const unsigned int* lengths, const int numParameters);
};

} // namespace RakNet

#endif // __RDMA_INTERFACE_H

#endif // _RAKNET_SUPPORT_RDMAInterface==1
