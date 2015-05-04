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
/** @file JSConsole.h
 * @author Marek Kotewicz <marek@ethdev.com>
 * @date 2015
 * Ethereum client.
 */

#pragma once

#include <libjsengine/JSV8Engine.h>
#include <libjsengine/JSV8Printer.h>
#include <libweb3jsonrpc/WebThreeStubServer.h>

namespace dev
{
namespace eth
{

class JSConsole
{
public:
	JSConsole(WebThreeDirect& _web3, std::vector<dev::KeyPair> const& _accounts);
	void repl() const;

private:
	std::string promptForIndentionLevel(int _i) const;

	JSV8Engine m_engine;
	JSV8Printer m_printer;
	std::unique_ptr<WebThreeStubServer> m_jsonrpcServer;
	std::unique_ptr<jsonrpc::AbstractServerConnector> m_jsonrpcConnector;
};

}
}
