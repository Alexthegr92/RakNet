#include "NativeFeatureIncludes.h"
#if _RAKNET_SUPPORT_RDMAInterface==1

#include "RDMAInterface.h"
#include "RakSleep.h"
#include "RakAssert.h"
#include "StringCompressor.h"
#include "StringTable.h"
#include "Itoa.h"
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
#include <WinSock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#endif

using namespace RakNet;

STATIC_FACTORY_DEFINITIONS(RDMAInterface, RDMAInterface);

// Buffer Pool Implementation
RDMABufferPool::RDMABuffer* RDMABufferPool::AllocateBuffer()
{
	poolMutex.Lock();
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		if (!buffers[i].inUse)
		{
			buffers[i].inUse = true;
			poolMutex.Unlock();
			return &buffers[i];
		}
	}
	poolMutex.Unlock();
	return nullptr; // Pool exhausted
}

void RDMABufferPool::FreeBuffer(RDMABuffer* buffer)
{
	if (buffer)
	{
		poolMutex.Lock();
		buffer->inUse = false;
		poolMutex.Unlock();
	}
}

void RDMABufferPool::RegisterBuffers(struct fid_domain* domain)
{
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		buffers[i].data = (char*)rakMalloc_Ex(BUFFER_SIZE, _FILE_AND_LINE_);
		
		// Register memory region with RDMA
		int ret = fi_mr_reg(domain, buffers[i].data, BUFFER_SIZE,
		                    FI_SEND | FI_RECV, 0, 0, 0, &buffers[i].mr, nullptr);
		
		if (ret != 0)
		{
			// Handle registration failure
			printf("RDMA: Failed to register buffer %u: %s\n", i, fi_strerror(-ret));
		}
	}
}

void RDMABufferPool::DeregisterBuffers()
{
	for (unsigned int i = 0; i < POOL_SIZE; i++)
	{
		if (buffers[i].mr)
		{
			fi_close(&buffers[i].mr->fid);
			buffers[i].mr = nullptr;
		}
		if (buffers[i].data)
		{
			rakFree_Ex(buffers[i].data, _FILE_AND_LINE_);
			buffers[i].data = nullptr;
		}
	}
}

// RDMAInterface Implementation
RDMAInterface::RDMAInterface()
{
	hints = nullptr;
	fabric_info = nullptr;
	fabric = nullptr;
	domain = nullptr;
	listener = nullptr;
	event_queue = nullptr;
	completion_queue = nullptr;
	remoteClients = nullptr;
	remoteClientsLength = 0;
	// isStarted and threadRunning are LocklessUint32_t, initialized to 0 by default constructor
	threadPriority = 0;
	connectionMode = RDMA_MODE_RC;
	maxConnections = 0;
}

RDMAInterface::~RDMAInterface()
{
	Stop();
}

bool RDMAInterface::InitializeFabric(const char* providerName)
{
	// Allocate hints structure
	hints = fi_allocinfo();
	if (!hints)
		return false;

	// Configure hints based on connection mode
	hints->ep_attr->type = (connectionMode == RDMA_MODE_UD) ? FI_EP_DGRAM : FI_EP_MSG;
	hints->caps = FI_MSG | FI_RMA | FI_SEND | FI_RECV;
	hints->mode = FI_CONTEXT;
	hints->domain_attr->mr_mode = FI_MR_LOCAL | FI_MR_ALLOCATED | FI_MR_PROV_KEY | FI_MR_VIRT_ADDR;
	hints->addr_format = FI_SOCKADDR;

	if (providerName)
	{
		hints->fabric_attr->prov_name = strdup(providerName);
	}

	// Get fabric info
	int ret = fi_getinfo(FI_VERSION(1, 9), nullptr, nullptr, 0, hints, &fabric_info);
	if (ret != 0)
	{
		printf("RDMA: fi_getinfo failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Open fabric
	ret = fi_fabric(fabric_info->fabric_attr, &fabric, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_fabric failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Open domain
	ret = fi_domain(fabric, fabric_info, &domain, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_domain failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Create event queue
	struct fi_eq_attr eq_attr = {0};
	eq_attr.size = 1024;
	eq_attr.wait_obj = FI_WAIT_UNSPEC;
	ret = fi_eq_open(fabric, &eq_attr, &event_queue, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_eq_open failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Create completion queue
	struct fi_cq_attr cq_attr = {0};
	cq_attr.size = 4096;
	cq_attr.format = FI_CQ_FORMAT_CONTEXT;
	ret = fi_cq_open(domain, &cq_attr, &completion_queue, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_cq_open failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Register buffer pool
	bufferPool.RegisterBuffers(domain);

	return true;
}

void RDMAInterface::CleanupFabric()
{
	bufferPool.DeregisterBuffers();

	if (completion_queue)
	{
		fi_close(&completion_queue->fid);
		completion_queue = nullptr;
	}

	if (event_queue)
	{
		fi_close(&event_queue->fid);
		event_queue = nullptr;
	}

	if (listener)
	{
		fi_close(&listener->fid);
		listener = nullptr;
	}

	if (domain)
	{
		fi_close(&domain->fid);
		domain = nullptr;
	}

	if (fabric)
	{
		fi_close(&fabric->fid);
		fabric = nullptr;
	}

	if (fabric_info)
	{
		fi_freeinfo(fabric_info);
		fabric_info = nullptr;
	}

	if (hints)
	{
		fi_freeinfo(hints);
		hints = nullptr;
	}
}

bool RDMAInterface::CreateListenEndpoint(unsigned short port, const char* hostAddress)
{
	// Create passive endpoint for listening
	int ret = fi_passive_ep(fabric, fabric_info, &listener, nullptr);
	if (ret != 0)
	{
		printf("RDMA: fi_passive_ep failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Bind event queue to listener
	ret = fi_pep_bind(listener, &event_queue->fid, 0);
	if (ret != 0)
	{
		printf("RDMA: fi_pep_bind failed: %s\n", fi_strerror(-ret));
		return false;
	}

	// Setup address
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	
	if (hostAddress && strlen(hostAddress) > 0)
	{
		inet_pton(AF_INET, hostAddress, &addr.sin_addr);
	}
	else
	{
		addr.sin_addr.s_addr = INADDR_ANY;
	}

	// Bind event queue to passive endpoint
	ret = fi_pep_bind(listener, &event_queue->fid, 0);
	if (ret != 0)
	{
		printf("RDMA: Failed to bind event queue: %s\n", fi_strerror(-ret));
		return false;
	}

	// Start listening
	ret = fi_listen(listener);
	if (ret != 0)
	{
		printf("RDMA: fi_listen failed: %s\n", fi_strerror(-ret));
		return false;
	}

	return true;
}

bool RDMAInterface::Start(unsigned short port, 
                          unsigned short maxIncomingConnections, 
                          unsigned short maxConn,
                          int _threadPriority,
                          RDMAConnectionMode connMode,
                          const char* providerName,
                          const char* bindAddress)
{
	if (isStarted.GetValue() != 0)
		return false;

	connectionMode = connMode;
	threadPriority = _threadPriority;
	maxConnections = (maxConn == 0) ? maxIncomingConnections : maxConn;

	if (!InitializeFabric(providerName))
	{
		CleanupFabric();
		return false;
	}

	if (!CreateListenEndpoint(port, bindAddress))
	{
		CleanupFabric();
		return false;
	}

	// Allocate remote clients array
	remoteClientsLength = maxConnections;
	remoteClients = RakNet::OP_NEW_ARRAY<RDMARemoteClient>(remoteClientsLength, _FILE_AND_LINE_);

	isStarted.Increment();
	threadRunning.Increment();

	// TODO: Start worker threads (UpdateRDMAInterfaceLoop)
	// This would be similar to TCPInterface's thread management

	return true;
}

void RDMAInterface::Stop(void)
{
	if (isStarted.GetValue() == 0)
		return;

	isStarted.Decrement();
	
	// Wait for thread to finish
	while (threadRunning.GetValue() > 0)
		RakSleep(10);

	// Wait for threads to complete
	RakSleep(100);

	// Close all connections
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
		{
			CloseConnection(remoteClients[i].systemAddress);
		}
	}

	// Cleanup
	if (remoteClients)
	{
		RakNet::OP_DELETE_ARRAY(remoteClients, _FILE_AND_LINE_);
		remoteClients = nullptr;
	}

	CleanupFabric();

	// Clear queues
	Packet* packet;
	while ((packet = headPush.Pop()))
		DeallocatePacket(packet);
	while ((packet = tailPush.Pop()))
		DeallocatePacket(packet);
}

SystemAddress RDMAInterface::Connect(const char* host, unsigned short remotePort, bool block)
{
	// TODO: Implement active connection establishment
	// This would use fi_connect() for connection-oriented mode
	// or setup the endpoint for datagram mode
	
	(void)host;
	(void)remotePort;
	(void)block;
	
	return UNASSIGNED_SYSTEM_ADDRESS;
}

void RDMAInterface::Send(const char* data, unsigned int length, const SystemAddress& systemAddress, bool broadcast)
{
	if (broadcast)
	{
		for (int i = 0; i < remoteClientsLength; i++)
		{
			if (remoteClients[i].isActive)
			{
				remoteClients[i].Send(data, length);
			}
		}
	}
	else
	{
		// Find the specific client
		for (int i = 0; i < remoteClientsLength; i++)
		{
			if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
			{
				remoteClients[i].Send(data, length);
				break;
			}
		}
	}
}

bool RDMAInterface::SendList(const char** data, const unsigned int* lengths, const int numParameters,
                             const SystemAddress& systemAddress, bool broadcast)
{
	if (numParameters == 0)
		return false;

	// Calculate total length
	unsigned int totalLength = 0;
	for (int i = 0; i < numParameters; i++)
		totalLength += lengths[i];

	// Allocate concatenated buffer
	char* buffer = (char*)rakMalloc_Ex(totalLength, _FILE_AND_LINE_);
	unsigned int offset = 0;
	
	for (int i = 0; i < numParameters; i++)
	{
		memcpy(buffer + offset, data[i], lengths[i]);
		offset += lengths[i];
	}

	Send(buffer, totalLength, systemAddress, broadcast);
	
	rakFree_Ex(buffer, _FILE_AND_LINE_);
	return true;
}

unsigned int RDMAInterface::GetOutgoingDataBufferSize(SystemAddress systemAddress) const
{
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
		{
			return remoteClients[i].outgoingData.GetBytesWritten();
		}
	}
	return 0;
}

bool RDMAInterface::ReceiveHasPackets(void)
{
	return headPush.Size() > 0 || tailPush.Size() > 0 || incomingMessages.Size() > 0;
}

Packet* RDMAInterface::Receive(void)
{
	Packet* packet = headPush.Pop();
	if (packet)
		return packet;

	packet = incomingMessages.Pop();
	if (packet)
		return packet;

	return tailPush.Pop();
}

Packet* RDMAInterface::ReceiveInt(void)
{
	return Receive();
}

void RDMAInterface::CloseConnection(SystemAddress systemAddress)
{
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive && remoteClients[i].systemAddress == systemAddress)
		{
			if (remoteClients[i].endpoint)
			{
				fi_close(&remoteClients[i].endpoint->fid);
				remoteClients[i].endpoint = nullptr;
			}
			
			if (remoteClients[i].sendBuffer)
			{
				bufferPool.FreeBuffer(remoteClients[i].sendBuffer);
				remoteClients[i].sendBuffer = nullptr;
			}
			
			if (remoteClients[i].recvBuffer)
			{
				bufferPool.FreeBuffer(remoteClients[i].recvBuffer);
				remoteClients[i].recvBuffer = nullptr;
			}
			
			remoteClients[i].SetActive(false);
			SystemAddress* sa = lostConnections.Allocate(_FILE_AND_LINE_);
			*sa = systemAddress;
			lostConnections.Push(sa);
			break;
		}
	}
}

void RDMAInterface::DeallocatePacket(Packet* packet)
{
	if (packet)
	{
		rakFree_Ex(packet->data, _FILE_AND_LINE_);
		RakNet::OP_DELETE(packet, _FILE_AND_LINE_);
	}
}

void RDMAInterface::GetConnectionList(SystemAddress* remoteSystems, unsigned short* numberOfSystems) const
{
	unsigned short count = 0;
	
	for (int i = 0; i < remoteClientsLength && count < *numberOfSystems; i++)
	{
		if (remoteClients[i].isActive && remoteSystems)
		{
			remoteSystems[count] = remoteClients[i].systemAddress;
			count++;
		}
	}
	
	*numberOfSystems = count;
}

unsigned short RDMAInterface::GetConnectionCount(void) const
{
	unsigned short count = 0;
	for (int i = 0; i < remoteClientsLength; i++)
	{
		if (remoteClients[i].isActive)
			count++;
	}
	return count;
}

SystemAddress RDMAInterface::HasCompletedConnectionAttempt(void)
{
	completedConnectionAttemptMutex.Lock();
	SystemAddress sa = completedConnectionAttempts.IsEmpty() ? UNASSIGNED_SYSTEM_ADDRESS : completedConnectionAttempts.Pop();
	completedConnectionAttemptMutex.Unlock();
	return sa;
}

SystemAddress RDMAInterface::HasFailedConnectionAttempt(void)
{
	failedConnectionAttemptMutex.Lock();
	SystemAddress sa = failedConnectionAttempts.IsEmpty() ? UNASSIGNED_SYSTEM_ADDRESS : failedConnectionAttempts.Pop();
	failedConnectionAttemptMutex.Unlock();
	return sa;
}

SystemAddress RDMAInterface::HasNewIncomingConnection(void)
{
	if (newIncomingConnections.IsEmpty())
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress* sa = newIncomingConnections.Pop();
	if (!sa)
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress ret = *sa;
	newIncomingConnections.Deallocate(sa, _FILE_AND_LINE_);
	return ret;
}

SystemAddress RDMAInterface::HasLostConnection(void)
{
	if (lostConnections.IsEmpty())
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress* sa = lostConnections.Pop();
	if (!sa)
		return UNASSIGNED_SYSTEM_ADDRESS;
	SystemAddress ret = *sa;
	lostConnections.Deallocate(sa, _FILE_AND_LINE_);
	return ret;
}

Packet* RDMAInterface::AllocatePacket(unsigned dataSize)
{
	Packet* packet = RakNet::OP_NEW<Packet>(_FILE_AND_LINE_);
	packet->data = (unsigned char*)rakMalloc_Ex(dataSize, _FILE_AND_LINE_);
	packet->length = dataSize;
	packet->bitSize = BYTES_TO_BITS(dataSize);
	packet->deleteData = false;
	return packet;
}

void RDMAInterface::PushBackPacket(Packet* packet, bool pushAtHead)
{
	if (pushAtHead)
		headPush.Push(packet, _FILE_AND_LINE_);
	else
		tailPush.Push(packet, _FILE_AND_LINE_);
}

bool RDMAInterface::WasStarted(void) const
{
	return isStarted.GetValue() != 0;
}

void RDMAInterface::AttachPlugin(PluginInterface2* plugin)
{
	if (messageHandlerList.GetIndexOf(plugin) == (unsigned)-1)
	{
		messageHandlerList.Insert(plugin, _FILE_AND_LINE_);
		plugin->SetRDMAInterface(this);
		plugin->OnAttach();
	}
}

void RDMAInterface::DetachPlugin(PluginInterface2* plugin)
{
	unsigned int index = messageHandlerList.GetIndexOf(plugin);
	if (index != (unsigned)-1)
	{
		messageHandlerList.RemoveAtIndex(index);
		plugin->OnDetach();
	}
}

// RDMARemoteClient Implementation
int RDMARemoteClient::Send(const char* data, unsigned int length)
{
	if (!isActive || !endpoint)
		return -1;

	// Post RDMA send
	// In a real implementation, this would use fi_send()
	// For now, buffer the data
	outgoingDataMutex.Lock();
	outgoingData.WriteBytes(data, length, _FILE_AND_LINE_);
	outgoingDataMutex.Unlock();

	return (int)length;
}

int RDMARemoteClient::Recv(char* data, const int dataSize)
{
	if (!isActive || !endpoint)
		return -1;

	// In a real implementation, this would process completed receives
	// from the completion queue
	
	return 0;
}

void RDMARemoteClient::SetActive(bool a)
{
	isActiveMutex.Lock();
	isActive = a;
	isActiveMutex.Unlock();
}

void RDMARemoteClient::SendOrBuffer(const char** data, const unsigned int* lengths, const int numParameters)
{
	for (int i = 0; i < numParameters; i++)
	{
		Send(data[i], lengths[i]);
	}
}

// Thread functions would go here (UpdateRDMAInterfaceLoop, RDMAConnectionAttemptLoop)
// Similar to TCPInterface's implementation

#endif // _RAKNET_SUPPORT_RDMAInterface==1
