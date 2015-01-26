/*
	This file is part of cpp-ethereum.

	cpp-ethereum is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	cpp-ethereum is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with cpp-ethereum.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file Host.cpp
 * @author Alex Leverington <nessence@gmail.com>
 * @author Gav Wood <i@gavwood.com>
 * @date 2014
 */

#include <set>
#include <chrono>
#include <thread>
#include <mutex>
#include <boost/algorithm/string.hpp>
#include <libdevcore/Common.h>
#include <libdevcore/CommonIO.h>
#include <libethcore/Exceptions.h>
#include <libdevcrypto/FileSystem.h>
#include "Session.h"
#include "Common.h"
#include "Capability.h"
#include "UPnP.h"
#include "Host.h"
using namespace std;
using namespace dev;
using namespace dev::p2p;

HostNodeTableHandler::HostNodeTableHandler(Host& _host): m_host(_host) {}

void HostNodeTableHandler::processEvent(NodeId const& _n, NodeTableEventType const& _e)
{
	m_host.onNodeTableEvent(_n, _e);
}

Host::Host(std::string const& _clientVersion, NetworkPreferences const& _n):
	Worker("p2p", 0),
	m_clientVersion(_clientVersion),
	m_netPrefs(_n),
	m_ifAddresses(Network::getInterfaceAddresses()),
	m_ioService(2),
	m_tcp4Acceptor(m_ioService),
	m_alias(move(getHostIdentifier())),
	m_lastPing(chrono::time_point<chrono::steady_clock>::min())
{
	for (auto address: m_ifAddresses)
		if (address.is_v4())
			clog(NetNote) << "IP Address: " << address << " = " << (isPrivateAddress(address) ? "[LOCAL]" : "[PEER]");
	
	clog(NetNote) << "Id:" << id();
}

Host::~Host()
{
	stop();
}

void Host::start()
{
	startWorking();
}

void Host::stop()
{
	// called to force io_service to kill any remaining tasks it might have -
	// such tasks may involve socket reads from Capabilities that maintain references
	// to resources we're about to free.

	{
		// Although m_run is set by stop() or start(), it effects m_runTimer so x_runTimer is used instead of a mutex for m_run.
		// when m_run == false, run() will cause this::run() to stop() ioservice
		Guard l(x_runTimer);
		// ignore if already stopped/stopping
		if (!m_run)
			return;
		m_run = false;
	}
	
	// wait for m_timer to reset (indicating network scheduler has stopped)
	while (!!m_timer)
		this_thread::sleep_for(chrono::milliseconds(50));
	
	// stop worker thread
	stopWorking();
}

void Host::doneWorking()
{
	// reset ioservice (allows manually polling network, below)
	m_ioService.reset();
	
	// shutdown acceptor
	m_tcp4Acceptor.cancel();
	if (m_tcp4Acceptor.is_open())
		m_tcp4Acceptor.close();
	
	// There maybe an incoming connection which started but hasn't finished.
	// Wait for acceptor to end itself instead of assuming it's complete.
	// This helps ensure a peer isn't stopped at the same time it's starting
	// and that socket for pending connection is closed.
	while (m_accepting)
		m_ioService.poll();

	// stop capabilities (eth: stops syncing or block/tx broadcast)
	for (auto const& h: m_capabilities)
		h.second->onStopping();

	// disconnect peers
	for (unsigned n = 0;; n = 0)
	{
		{
			RecursiveGuard l(x_sessions);
			for (auto i: m_sessions)
				if (auto p = i.second.lock())
					if (p->isOpen())
					{
						p->disconnect(ClientQuit);
						n++;
					}
		}
		if (!n)
			break;
		
		// poll so that peers send out disconnect packets
		m_ioService.poll();
	}
	
	// stop network (again; helpful to call before subsequent reset())
	m_ioService.stop();
	
	// reset network (allows reusing ioservice in future)
	m_ioService.reset();
	
	// finally, clear out peers (in case they're lingering)
	RecursiveGuard l(x_sessions);
	m_sessions.clear();
}

unsigned Host::protocolVersion() const
{
	return 3;
}

void Host::registerPeer(std::shared_ptr<Session> _s, CapDescs const& _caps)
{
	{
		RecursiveGuard l(x_sessions);
		// TODO: temporary loose-coupling; if m_peers already has peer,
		//       it is same as _s->m_peer. (fixing next PR)
		if (!m_peers.count(_s->m_peer->id))
			m_peers[_s->m_peer->id] = _s->m_peer;
		m_sessions[_s->m_peer->id] = _s;
	}
	
	unsigned o = (unsigned)UserPacket;
	for (auto const& i: _caps)
		if (haveCapability(i))
		{
			_s->m_capabilities[i] = shared_ptr<Capability>(m_capabilities[i]->newPeerCapability(_s.get(), o));
			o += m_capabilities[i]->messageCount();
		}
}

void Host::onNodeTableEvent(NodeId const& _n, NodeTableEventType const& _e)
{

	if (_e == NodeEntryAdded)
	{
		clog(NetNote) << "p2p.host.nodeTable.events.nodeEntryAdded " << _n;
		
		auto n = (*m_nodeTable)[_n];
		if (n)
		{
			RecursiveGuard l(x_sessions);
			auto p = m_peers[_n];
			if (!p)
			{
				m_peers[_n] = make_shared<Peer>();
				p = m_peers[_n];
				p->id = _n;
			}
			p->endpoint.tcp = n.endpoint.tcp;
			
			// TODO: Implement similar to doFindNode. Attempt connecting to nodes
			//       until ideal peer count is reached; if all nodes are tried,
			//       repeat. Notably, this is an integrated process such that
			//       when onNodeTableEvent occurs we should also update +/-
			//       the list of nodes to be tried. Thus:
			//       1) externalize connection attempts
			//       2) attempt copies potentialPeer list
			//       3) replace this logic w/maintenance of potentialPeers
			if (peerCount() < m_idealPeerCount)
				connect(p);
		}
	}
	else if (_e == NodeEntryRemoved)
	{
		clog(NetNote) << "p2p.host.nodeTable.events.nodeEntryRemoved " << _n;
		
		RecursiveGuard l(x_sessions);
		m_peers.erase(_n);
	}
}

void Host::seal(bytes& _b)
{
	_b[0] = 0x22;
	_b[1] = 0x40;
	_b[2] = 0x08;
	_b[3] = 0x91;
	uint32_t len = (uint32_t)_b.size() - 8;
	_b[4] = (len >> 24) & 0xff;
	_b[5] = (len >> 16) & 0xff;
	_b[6] = (len >> 8) & 0xff;
	_b[7] = len & 0xff;
}

void Host::determinePublic(string const& _publicAddress, bool _upnp)
{
	m_peerAddresses.clear();
	
	// no point continuing if there are no interface addresses or valid listen port
	if (!m_ifAddresses.size() || m_listenPort < 1)
		return;

	// populate interfaces we'll listen on (eth listens on all interfaces); ignores local
	for (auto addr: m_ifAddresses)
		if ((m_netPrefs.localNetworking || !isPrivateAddress(addr)) && !isLocalHostAddress(addr))
			m_peerAddresses.insert(addr);
	
	// if user supplied address is a public address then we use it
	// if user supplied address is private, and localnetworking is enabled, we use it
	bi::address reqPublicAddr(bi::address(_publicAddress.empty() ? bi::address() : bi::address::from_string(_publicAddress)));
	bi::tcp::endpoint reqPublic(reqPublicAddr, m_listenPort);
	bool isprivate = isPrivateAddress(reqPublicAddr);
	bool ispublic = !isprivate && !isLocalHostAddress(reqPublicAddr);
	if (!reqPublicAddr.is_unspecified() && (ispublic || (isprivate && m_netPrefs.localNetworking)))
	{
		if (!m_peerAddresses.count(reqPublicAddr))
			m_peerAddresses.insert(reqPublicAddr);
		m_tcpPublic = reqPublic;
		return;
	}
	
	// if address wasn't provided, then use first public ipv4 address found
	for (auto addr: m_peerAddresses)
		if (addr.is_v4() && !isPrivateAddress(addr))
		{
			m_tcpPublic = bi::tcp::endpoint(*m_peerAddresses.begin(), m_listenPort);
			return;
		}
	
	// or find address via upnp
	if (_upnp)
	{
		bi::address upnpifaddr;
		bi::tcp::endpoint upnpep = Network::traverseNAT(m_ifAddresses, m_listenPort, upnpifaddr);
		if (!upnpep.address().is_unspecified() && !upnpifaddr.is_unspecified())
		{
			if (!m_peerAddresses.count(upnpep.address()))
				m_peerAddresses.insert(upnpep.address());
			m_tcpPublic = upnpep;
			return;
		}
	}

	// or if no address provided, use private ipv4 address if local networking is enabled
	if (reqPublicAddr.is_unspecified())
		if (m_netPrefs.localNetworking)
			for (auto addr: m_peerAddresses)
				if (addr.is_v4() && isPrivateAddress(addr))
				{
					m_tcpPublic = bi::tcp::endpoint(addr, m_listenPort);
					return;
				}
	
	// otherwise address is unspecified
	m_tcpPublic = bi::tcp::endpoint(bi::address(), m_listenPort);
}

void Host::runAcceptor()
{
	assert(m_listenPort > 0);
	
	if (m_run && !m_accepting)
	{
		clog(NetConnect) << "Listening on local port " << m_listenPort << " (public: " << m_tcpPublic << ")";
		m_accepting = true;

		// socket is created outside of acceptor-callback
		// An allocated socket is necessary as asio can use the socket
		// until the callback succeeds or fails.
		//
		// Until callback succeeds or fails, we can't dealloc it.
		//
		// Callback is guaranteed to be called via asio or when
		// m_tcp4Acceptor->stop() is called by Host.
		//
		// All exceptions are caught so they don't halt asio and so the
		// socket is deleted.
		//
		// It's possible for an accepted connection to return an error in which
		// case the socket may be open and must be closed to prevent asio from
		// processing socket events after socket is deallocated.
		
		bi::tcp::socket *s = new bi::tcp::socket(m_ioService);
		m_tcp4Acceptor.async_accept(*s, [=](boost::system::error_code ec)
		{
			// if no error code, doHandshake takes ownership
			bool success = false;
			if (!ec)
			{
				try
				{
					// doHandshake takes ownersihp of *s via std::move
					// incoming connection; we don't yet know nodeid
					doHandshake(s, NodeId());
					success = true;
				}
				catch (Exception const& _e)
				{
					clog(NetWarn) << "ERROR: " << diagnostic_information(_e);
				}
				catch (std::exception const& _e)
				{
					clog(NetWarn) << "ERROR: " << _e.what();
				}
			}
			
			// asio doesn't close socket on error
			if (!success && s->is_open())
			{
				boost::system::error_code ec;
				s->shutdown(boost::asio::ip::tcp::socket::shutdown_both, ec);
				s->close();
			}

			m_accepting = false;
			delete s;
			
			if (ec.value() < 1)
				runAcceptor();
		});
	}
}

void Host::doHandshake(bi::tcp::socket* _socket, NodeId _nodeId)
{
	try {
		clog(NetConnect) << "Accepting connection for " << _socket->remote_endpoint();
	} catch (...){}

	shared_ptr<Peer> p;
	if (_nodeId)
		p = m_peers[_nodeId];
	
	if (!p)
	{
		p = make_shared<Peer>();
		p->endpoint.tcp.address(_socket->remote_endpoint().address());
	}

	auto ps = std::make_shared<Session>(this, std::move(*_socket), p);
	ps->start();
}

string Host::pocHost()
{
	vector<string> strs;
	boost::split(strs, dev::Version, boost::is_any_of("."));
	return "poc-" + strs[1] + ".ethdev.com";
}

void Host::addNode(NodeId const& _node, std::string const& _addr, unsigned short _tcpPeerPort, unsigned short _udpNodePort)
{
	
	
	if (_tcpPeerPort < 30300 || _tcpPeerPort > 30305)
		cwarn << "Non-standard port being recorded: " << _tcpPeerPort;

	if (_tcpPeerPort >= /*49152*/32768)
	{
		cwarn << "Private port being recorded - setting to 0";
		_tcpPeerPort = 0;
	}
	
	boost::system::error_code ec;
	bi::address addr = bi::address::from_string(_addr, ec);
	if (ec)
	{
		bi::tcp::resolver r(m_ioService);
		r.async_resolve({_addr, toString(_tcpPeerPort)}, [=](boost::system::error_code const& _ec, bi::tcp::resolver::iterator _epIt)
		{
			if (_ec)
				return;
			bi::tcp::endpoint tcp = *_epIt;
			addNode(Node(_node, NodeIPEndpoint(bi::udp::endpoint(tcp.address(), _udpNodePort), tcp)));
		});
	}
	else
		addNode(Node(_node, NodeIPEndpoint(bi::udp::endpoint(addr, _udpNodePort), bi::tcp::endpoint(addr, _tcpPeerPort))));
}

void Host::connect(std::shared_ptr<Peer> const& _p)
{
	if (!m_run)
		return;
	
	if (havePeerSession(_p->id))
	{
		clog(NetWarn) << "Aborted connect. Node already connected.";
		return;
	}
	
	if (!m_nodeTable->haveNode(_p->id))
	{
		clog(NetWarn) << "Aborted connect. Node not in node table.";
		return;
	}
	
	// prevent concurrently connecting to a node
	Peer *nptr = _p.get();
	{
		Guard l(x_pendingNodeConns);
		if (m_pendingNodeConns.count(nptr))
			return;
		m_pendingNodeConns.insert(nptr);
	}
	
	clog(NetConnect) << "Attempting connection to node" << _p->id.abridged() << "@" << _p->peerEndpoint() << "from" << id().abridged();
	bi::tcp::socket* s = new bi::tcp::socket(m_ioService);
	s->async_connect(_p->peerEndpoint(), [=](boost::system::error_code const& ec)
	{
		if (ec)
		{
			clog(NetConnect) << "Connection refused to node" << _p->id.abridged() << "@" << _p->peerEndpoint() << "(" << ec.message() << ")";
			_p->lastDisconnect = TCPError;
			_p->lastAttempted = std::chrono::system_clock::now();
		}
		else
		{
			clog(NetConnect) << "Connected to" << _p->id.abridged() << "@" << _p->peerEndpoint();
			
			_p->lastConnected = std::chrono::system_clock::now();
			auto ps = make_shared<Session>(this, std::move(*s), _p);
			ps->start();
			
		}
		delete s;
		Guard l(x_pendingNodeConns);
		m_pendingNodeConns.erase(nptr);
	});
}

PeerSessionInfos Host::peers() const
{
	if (!m_run)
		return PeerSessionInfos();

	std::vector<PeerSessionInfo> ret;
	RecursiveGuard l(x_sessions);
	for (auto& i: m_sessions)
		if (auto j = i.second.lock())
			if (j->m_socket.is_open())
				ret.push_back(j->m_info);
	return ret;
}

void Host::run(boost::system::error_code const&)
{
	if (!m_run)
	{
		// reset NodeTable
		m_nodeTable.reset();
		
		// stopping io service allows running manual network operations for shutdown
		// and also stops blocking worker thread, allowing worker thread to exit
		m_ioService.stop();
		
		// resetting timer signals network that nothing else can be scheduled to run
		m_timer.reset();
		return;
	}
	
	m_nodeTable->processEvents();
	
	for (auto p: m_sessions)
		if (auto pp = p.second.lock())
			pp->serviceNodesRequest();
	
	keepAlivePeers();
	disconnectLatePeers();
	
	auto runcb = [this](boost::system::error_code const& error) { run(error); };
	m_timer->expires_from_now(boost::posix_time::milliseconds(c_timerInterval));
	m_timer->async_wait(runcb);
}
			
void Host::startedWorking()
{
	asserts(!m_timer);

	{
		// prevent m_run from being set to true at same time as set to false by stop()
		// don't release mutex until m_timer is set so in case stop() is called at same
		// time, stop will wait on m_timer and graceful network shutdown.
		Guard l(x_runTimer);
		// create deadline timer
		m_timer.reset(new boost::asio::deadline_timer(m_ioService));
		m_run = true;
	}
	
	// try to open acceptor (todo: ipv6)
	m_listenPort = Network::tcp4Listen(m_tcp4Acceptor, m_netPrefs.listenPort);
	
	// start capability threads
	for (auto const& h: m_capabilities)
		h.second->onStarting();
	
	// determine public IP, but only if we're able to listen for connections
	// todo: GUI when listen is unavailable in UI
	if (m_listenPort)
	{
		determinePublic(m_netPrefs.publicIP, m_netPrefs.upnp);
		
		if (m_listenPort > 0)
			runAcceptor();
			
		if (!m_tcpPublic.address().is_unspecified())
			// TODO: add m_tcpPublic endpoint; sort out endpoint stuff for nodetable
			m_nodeTable.reset(new NodeTable(m_ioService, m_alias, m_listenPort));
		else
			m_nodeTable.reset(new NodeTable(m_ioService, m_alias, m_listenPort > 0 ? m_listenPort : 30303));
		m_nodeTable->setEventHandler(new HostNodeTableHandler(*this));
	}
	else
		clog(NetNote) << "p2p.start.notice id:" << id().abridged() << "Invalid listen-port. Node Table Disabled.";
	
	clog(NetNote) << "p2p.started id:" << id().abridged();
	
	run(boost::system::error_code());
}

void Host::doWork()
{
	if (m_run)
		m_ioService.run();
}

void Host::keepAlivePeers()
{
	if (chrono::steady_clock::now() - c_keepAliveInterval < m_lastPing)
		return;
	
	RecursiveGuard l(x_sessions);
	for (auto p: m_sessions)
		if (auto pp = p.second.lock())
				pp->ping();

	m_lastPing = chrono::steady_clock::now();
}

void Host::disconnectLatePeers()
{
	auto now = chrono::steady_clock::now();
	if (now - c_keepAliveTimeOut < m_lastPing)
		return;

	RecursiveGuard l(x_sessions);
	for (auto p: m_sessions)
		if (auto pp = p.second.lock())
			if (now - c_keepAliveTimeOut > m_lastPing && pp->m_lastReceived < m_lastPing)
				pp->disconnect(PingTimeout);
}

bytes Host::saveNodes() const
{
	RLPStream nodes;
	int count = 0;
	{
		RecursiveGuard l(x_sessions);
		for (auto const& i: m_peers)
		{
			Peer const& n = *(i.second);
			// TODO: alpha: Figure out why it ever shares these ports.//n.address.port() >= 30300 && n.address.port() <= 30305 &&
			// TODO: alpha: if/how to save private addresses
			// Only save peers which have connected within 2 days, with properly-advertised port and public IP address
			if (chrono::system_clock::now() - n.lastConnected < chrono::seconds(3600 * 48) && n.peerEndpoint().port() > 0 && n.peerEndpoint().port() < /*49152*/32768 && n.id != id() && !isPrivateAddress(n.peerEndpoint().address()))
			{
				nodes.appendList(10);
				if (n.peerEndpoint().address().is_v4())
					nodes << n.peerEndpoint().address().to_v4().to_bytes();
				else
					nodes << n.peerEndpoint().address().to_v6().to_bytes();
				// TODO: alpha: replace 0 with trust-state of node
				nodes << n.peerEndpoint().port() << n.id << 0
					<< chrono::duration_cast<chrono::seconds>(n.lastConnected.time_since_epoch()).count()
					<< chrono::duration_cast<chrono::seconds>(n.lastAttempted.time_since_epoch()).count()
					<< n.failedAttempts << (unsigned)n.lastDisconnect << n.score << n.rating;
				count++;
			}
		}
	}
	RLPStream ret(3);
	ret << 0 << m_alias.secret();
	ret.appendList(count).appendRaw(nodes.out(), count);
	return ret.out();
}

// TODO: p2p Import-ant
void Host::restoreNodes(bytesConstRef _b)
{
	RecursiveGuard l(x_sessions);
	RLP r(_b);
	if (r.itemCount() > 0 && r[0].isInt())
		switch (r[0].toInt<int>())
		{
		case 0:
		{
			m_alias = KeyPair(r[1].toHash<Secret>());
//			noteNode(id(), m_tcpPublic);

			for (auto i: r[2])
			{
				bi::tcp::endpoint ep;
				if (i[0].itemCount() == 4)
					ep = bi::tcp::endpoint(bi::address_v4(i[0].toArray<byte, 4>()), i[1].toInt<short>());
				else
					ep = bi::tcp::endpoint(bi::address_v6(i[0].toArray<byte, 16>()), i[1].toInt<short>());
				auto id = (NodeId)i[2];
				if (!m_peers.count(id))
				{
					// TODO: p2p import/export :)
//					auto n = noteNode(id, ep);
//					n->lastConnected = chrono::system_clock::time_point(chrono::seconds(i[4].toInt<unsigned>()));
//					n->lastAttempted = chrono::system_clock::time_point(chrono::seconds(i[5].toInt<unsigned>()));
//					n->failedAttempts = i[6].toInt<unsigned>();
//					n->lastDisconnect = (DisconnectReason)i[7].toInt<unsigned>();
//					n->score = (int)i[8].toInt<unsigned>();
//					n->rating = (int)i[9].toInt<unsigned>();
				}
			}
		}
		default:;
		}
	else
		for (auto i: r)
		{
			auto id = (NodeId)i[2];
			if (!m_peers.count(id))
			{
				bi::tcp::endpoint ep;
				if (i[0].itemCount() == 4)
					ep = bi::tcp::endpoint(bi::address_v4(i[0].toArray<byte, 4>()), i[1].toInt<short>());
				else
					ep = bi::tcp::endpoint(bi::address_v6(i[0].toArray<byte, 16>()), i[1].toInt<short>());
//				auto n = noteNode(id, ep);
			}
		}
}

KeyPair Host::getHostIdentifier()
{
	static string s_file(getDataDir() + "/host");
	static mutex s_x;
	lock_guard<mutex> l(s_x);
	
	h256 secret;
	bytes b = contents(s_file);
	if (b.size() == 32)
		memcpy(secret.data(), b.data(), 32);
	else
	{
		// todo: replace w/user entropy; abstract to devcrypto
		std::mt19937_64 s_eng(time(0) + chrono::high_resolution_clock::now().time_since_epoch().count());
		std::uniform_int_distribution<uint16_t> d(0, 255);
		for (unsigned i = 0; i < 32; ++i)
			secret[i] = (byte)d(s_eng);
	}
	
	if (!secret)
		BOOST_THROW_EXCEPTION(crypto::InvalidState());
	return move(KeyPair(move(secret)));
}
