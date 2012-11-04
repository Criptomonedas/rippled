#ifndef __WSHANDLER__
#define __WSHANDLER__

class WSConnection;

// A single instance of this object is made.
// This instance dispatches all events.  There is no per connection persistence.
template <typename endpoint_type>
class WSServerHandler : public endpoint_type::handler
{
public:
	typedef typename endpoint_type::handler::connection_ptr connection_ptr;
	typedef typename endpoint_type::handler::message_ptr message_ptr;

	// Private reasons to close.
	enum {
		crTooSlow	= 4000,		// Client is too slow.
	};

private:
	boost::shared_ptr<boost::asio::ssl::context>		mCtx;

protected:
	boost::mutex										mMapLock;
	// For each connection maintain an associated object to track subscriptions.
	boost::unordered_map<connection_ptr, boost::shared_ptr<WSConnection> >	mMap;

public:
	WSServerHandler(boost::shared_ptr<boost::asio::ssl::context> spCtx) : mCtx(spCtx) {}

	boost::shared_ptr<boost::asio::ssl::context> on_tls_init()
	{
		return mCtx;
	}

	void send(connection_ptr cpClient, message_ptr mpMessage)
	{
		try
		{
			cpClient->send(mpMessage->get_payload(), mpMessage->get_opcode());
		}
		catch (...)
		{
			cpClient->close(websocketpp::close::status::value(crTooSlow), std::string("Client is too slow."));
		}
	}

	void send(connection_ptr cpClient, const std::string& strMessage)
	{
		try
		{
			cLog(lsDEBUG) << "Ws:: Sending '" << strMessage << "'";

			cpClient->send(strMessage);
		}
		catch (...)
		{
			cpClient->close(websocketpp::close::status::value(crTooSlow), std::string("Client is too slow."));
		}
	}

	void send(connection_ptr cpClient, const Json::Value& jvObj)
	{
		Json::FastWriter	jfwWriter;

		// cLog(lsDEBUG) << "Ws:: Object '" << jfwWriter.write(jvObj) << "'";

		send(cpClient, jfwWriter.write(jvObj));
	}

	void on_open(connection_ptr cpClient)
	{
		boost::mutex::scoped_lock	sl(mMapLock);

		mMap[cpClient]	= boost::make_shared<WSConnection>(this, cpClient);
	}

	void on_close(connection_ptr cpClient)
	{
		boost::mutex::scoped_lock	sl(mMapLock);

		mMap.erase(cpClient);
	}

	void on_message(connection_ptr cpClient, message_ptr mpMessage)
	{
		Json::Value		jvRequest;
		Json::Reader	jrReader;

		if (mpMessage->get_opcode() != websocketpp::frame::opcode::TEXT)
		{
			Json::Value	jvResult(Json::objectValue);

			jvResult["type"]	= "error";
			jvResult["error"]	= "wsTextRequired";	// We only accept text messages.

			send(cpClient, jvResult);
		}
		else if (!jrReader.parse(mpMessage->get_payload(), jvRequest) || jvRequest.isNull() || !jvRequest.isObject())
		{
			Json::Value	jvResult(Json::objectValue);

			jvResult["type"]	= "error";
			jvResult["error"]	= "jsonInvalid";	// Received invalid json.
			jvResult["value"]	= mpMessage->get_payload();

			send(cpClient, jvResult);
		}
		else
		{
			send(cpClient, mMap[cpClient]->invokeCommand(jvRequest));
		}
	}

	// Respond to http requests.
	void http(connection_ptr cpClient)
	{
		cpClient->set_body(
			"<!DOCTYPE html><html><head><title>" SYSTEM_NAME " Test</title></head>"
			"<body><h1>" SYSTEM_NAME " Test</h1><p>This page shows http(s) connectivity is working.</p></body></html>");
	}
};

#endif