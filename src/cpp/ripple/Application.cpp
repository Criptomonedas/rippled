
#include "Application.h"
#include "Config.h"
#include "PeerDoor.h"
#include "RPCDoor.h"
#include "BitcoinUtil.h"
#include "key.h"
#include "utils.h"
#include "TaggedCache.h"
#include "Log.h"

#include "../database/SqliteDatabase.h"

#include <iostream>

#include <boost/bind.hpp>
#include <boost/filesystem.hpp>
#include <boost/thread.hpp>

SETUP_LOG();

LogPartition TaggedCachePartition("TaggedCache");
LogPartition AutoSocketPartition("AutoSocket");
Application* theApp = NULL;

DatabaseCon::DatabaseCon(const std::string& strName, const char *initStrings[], int initCount)
{
	boost::filesystem::path	pPath	= theConfig.RUN_STANDALONE
			? ""
			: theConfig.DATA_DIR / strName;

	mDatabase = new SqliteDatabase(pPath.string().c_str());
	mDatabase->connect();
	for(int i = 0; i < initCount; ++i)
		mDatabase->executeSQL(initStrings[i], true);
}

DatabaseCon::~DatabaseCon()
{
	mDatabase->disconnect();
	delete mDatabase;
}

Application::Application() :
	mIOWork(mIOService), mAuxWork(mAuxService), mUNL(mIOService), mNetOps(mIOService, &mLedgerMaster),
	mTempNodeCache("NodeCache", 16384, 90), mHashedObjectStore(16384, 300),
	mSNTPClient(mAuxService), mRPCHandler(&mNetOps), mFeeTrack(),
	mRpcDB(NULL), mTxnDB(NULL), mLedgerDB(NULL), mWalletDB(NULL), mHashNodeDB(NULL), mNetNodeDB(NULL),
	mConnectionPool(mIOService), mPeerDoor(NULL), mRPCDoor(NULL), mWSPublicDoor(NULL), mWSPrivateDoor(NULL),
	mSweepTimer(mAuxService)
{
	getRand(mNonce256.begin(), mNonce256.size());
	getRand(reinterpret_cast<unsigned char *>(&mNonceST), sizeof(mNonceST));
}

extern const char *RpcDBInit[], *TxnDBInit[], *LedgerDBInit[], *WalletDBInit[], *HashNodeDBInit[], *NetNodeDBInit[];
extern int RpcDBCount, TxnDBCount, LedgerDBCount, WalletDBCount, HashNodeDBCount, NetNodeDBCount;
bool Instance::running = true;

void Application::stop()
{
	cLog(lsINFO) << "Received shutdown request";
	mIOService.stop();
	mHashedObjectStore.bulkWrite();
	mValidations.flush();
	mAuxService.stop();
	mJobQueue.shutdown();

	cLog(lsINFO) << "Stopped: " << mIOService.stopped();
	Instance::shutdown();
}

static void InitDB(DatabaseCon** dbCon, const char *fileName, const char *dbInit[], int dbCount)
{
	*dbCon = new DatabaseCon(fileName, dbInit, dbCount);
}

volatile bool doShutdown = false;

#ifdef SIGINT
void sigIntHandler(int)
{
	doShutdown = true;
}
#endif

void Application::setup()
{
	mJobQueue.setThreadCount();
	mSweepTimer.expires_from_now(boost::posix_time::seconds(10));
	mSweepTimer.async_wait(boost::bind(&Application::sweep, this));
	mLoadMgr.init();

#ifndef WIN32
#ifdef SIGINT
	if (!theConfig.RUN_STANDALONE)
	{
		struct sigaction sa;
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = sigIntHandler;
		sigaction(SIGINT, &sa, NULL);
	}
#endif
#endif

	assert(mTxnDB == NULL);
	if (!theConfig.DEBUG_LOGFILE.empty())
	{ // Let DEBUG messages go to the file but only WARNING or higher to regular output (unless verbose)
		Log::setLogFile(theConfig.DEBUG_LOGFILE);
		if (Log::getMinSeverity() > lsDEBUG)
			LogPartition::setSeverity(lsDEBUG);
	}

	boost::thread(boost::bind(&boost::asio::io_service::run, &mAuxService)).detach();

	if (!theConfig.RUN_STANDALONE)
		mSNTPClient.init(theConfig.SNTP_SERVERS);

	//
	// Construct databases.
	//
	boost::thread t1(boost::bind(&InitDB, &mRpcDB, "rpc.db", RpcDBInit, RpcDBCount));
	boost::thread t2(boost::bind(&InitDB, &mTxnDB, "transaction.db", TxnDBInit, TxnDBCount));
	boost::thread t3(boost::bind(&InitDB, &mLedgerDB, "ledger.db", LedgerDBInit, LedgerDBCount));
	boost::thread t4(boost::bind(&InitDB, &mWalletDB, "wallet.db", WalletDBInit, WalletDBCount));
	boost::thread t5(boost::bind(&InitDB, &mHashNodeDB, "hashnode.db", HashNodeDBInit, HashNodeDBCount));
	boost::thread t6(boost::bind(&InitDB, &mNetNodeDB, "netnode.db", NetNodeDBInit, NetNodeDBCount));
	t1.join(); t2.join(); t3.join(); t4.join(); t5.join(); t6.join();
	mTxnDB->getDB()->setupCheckpointing(&mJobQueue);
	mLedgerDB->getDB()->setupCheckpointing(&mJobQueue);
	mHashNodeDB->getDB()->setupCheckpointing(&mJobQueue);

	if (theConfig.START_UP == Config::FRESH)
	{
		cLog(lsINFO) << "Starting new Ledger";

		startNewLedger();
	}
	else if (theConfig.START_UP == Config::LOAD)
	{
		cLog(lsINFO) << "Loading specified Ledger";

		loadOldLedger(theConfig.START_LEDGER);
	}
	else if (theConfig.START_UP == Config::NETWORK)
	{ // This should probably become the default once we have a stable network
		if (!theConfig.RUN_STANDALONE)
			mNetOps.needNetworkLedger();
		startNewLedger();
	}
	else
		startNewLedger();

	mOrderBookDB.setup(theApp->getLedgerMaster().getCurrentLedger()); // TODO: We need to update this if the ledger jumps

	//
	// Begin validation and ip maintenance.
	// - Wallet maintains local information: including identity and network connection persistence information.
	//
	if (theConfig.RUN_STANDALONE)
		mWallet.startStandalone();
	else
		mWallet.start();

	//
	// Set up UNL.
	//
	if (!theConfig.RUN_STANDALONE)
		getUNL().nodeBootstrap();

	mValidations.tune(theConfig.getSize(siValidationsSize), theConfig.getSize(siValidationsAge));
	mHashedObjectStore.tune(theConfig.getSize(siNodeCacheSize), theConfig.getSize(siNodeCacheAge));
	mLedgerMaster.tune(theConfig.getSize(siLedgerSize), theConfig.getSize(siLedgerAge));

	//
	// Allow peer connections.
	//
	if (!theConfig.RUN_STANDALONE)
	{
		try
		{
			mPeerDoor = new PeerDoor(mIOService);
		}
		catch (const std::exception& e)
		{
			// Must run as directed or exit.
			cLog(lsFATAL) << boost::str(boost::format("Can not open peer service: %s") % e.what());

			exit(3);
		}
	}
	else
	{
		cLog(lsINFO) << "Peer interface: disabled";
	}

	//
	// Allow RPC connections.
	//
	if (!theConfig.RPC_IP.empty() && theConfig.RPC_PORT)
	{
		try
		{
			mRPCDoor = new RPCDoor(mIOService);
		}
		catch (const std::exception& e)
		{
			// Must run as directed or exit.
			cLog(lsFATAL) << boost::str(boost::format("Can not open RPC service: %s") % e.what());

			exit(3);
		}
	}
	else
	{
		cLog(lsINFO) << "RPC interface: disabled";
	}

	//
	// Allow private WS connections.
	//
	if (!theConfig.WEBSOCKET_IP.empty() && theConfig.WEBSOCKET_PORT)
	{
		try
		{
			mWSPrivateDoor	= WSDoor::createWSDoor(theConfig.WEBSOCKET_IP, theConfig.WEBSOCKET_PORT, false);
		}
		catch (const std::exception& e)
		{
			// Must run as directed or exit.
			cLog(lsFATAL) << boost::str(boost::format("Can not open private websocket service: %s") % e.what());

			exit(3);
		}
	}
	else
	{
		cLog(lsINFO) << "WS private interface: disabled";
	}

	//
	// Allow public WS connections.
	//
	if (!theConfig.WEBSOCKET_PUBLIC_IP.empty() && theConfig.WEBSOCKET_PUBLIC_PORT)
	{
		try
		{
			mWSPublicDoor	= WSDoor::createWSDoor(theConfig.WEBSOCKET_PUBLIC_IP, theConfig.WEBSOCKET_PUBLIC_PORT, true);
		}
		catch (const std::exception& e)
		{
			// Must run as directed or exit.
			cLog(lsFATAL) << boost::str(boost::format("Can not open public websocket service: %s") % e.what());

			exit(3);
		}
	}
	else
	{
		cLog(lsINFO) << "WS public interface: disabled";
	}

	//
	// Begin connecting to network.
	//
	if (!theConfig.RUN_STANDALONE)
		mConnectionPool.start();


	if (theConfig.RUN_STANDALONE)
	{
		cLog(lsWARNING) << "Running in standalone mode";

		mNetOps.setStandAlone();
	}
	else
		mNetOps.setStateTimer();
}

void Application::run()
{
	mIOService.run(); // This blocks

	if (mWSPublicDoor)
		mWSPublicDoor->stop();

	if (mWSPrivateDoor)
		mWSPrivateDoor->stop();

	cLog(lsINFO) << "Done.";
}

void Application::sweep()
{

	boost::filesystem::space_info space = boost::filesystem::space(theConfig.DATA_DIR);
	if (space.available < (512 * 1024 * 1024))
	{
		cLog(lsFATAL) << "Remaining free disk space is less than 512MB";
		theApp->stop();
	}

	mMasterTransaction.sweep();
	mHashedObjectStore.sweep();
	mLedgerMaster.sweep();
	mTempNodeCache.sweep();
	mValidations.sweep();
	getMasterLedgerAcquire().sweep();
	mSweepTimer.expires_from_now(boost::posix_time::seconds(theConfig.getSize(siSweepInterval)));
	mSweepTimer.async_wait(boost::bind(&Application::sweep, this));
}

Application::~Application()
{
	delete mTxnDB;
	delete mLedgerDB;
	delete mWalletDB;
	delete mHashNodeDB;
	delete mNetNodeDB;
}

void Application::startNewLedger()
{
	// New stuff.
	RippleAddress	rootSeedMaster		= RippleAddress::createSeedGeneric("masterpassphrase");
	RippleAddress	rootAddress			= RippleAddress::createAccountID("rHb9CJAWyB4rj91VRWn96DkukG4bwdtyTh");

	// Print enough information to be able to claim root account.
	cLog(lsINFO) << "Root master seed: " << rootSeedMaster.humanSeed();
	cLog(lsINFO) << "Root account: " << rootAddress.humanAccountID();

	{
		Ledger::pointer firstLedger = boost::make_shared<Ledger>(rootAddress, SYSTEM_CURRENCY_START);
		assert(!!firstLedger->getAccountState(rootAddress));
		firstLedger->updateHash();
		firstLedger->setClosed();
		firstLedger->setAccepted();
		mLedgerMaster.pushLedger(firstLedger);

		Ledger::pointer secondLedger = boost::make_shared<Ledger>(true, boost::ref(*firstLedger));
		secondLedger->setClosed();
		secondLedger->setAccepted();
		mLedgerMaster.pushLedger(secondLedger, boost::make_shared<Ledger>(true, boost::ref(*secondLedger)), false);
		assert(!!secondLedger->getAccountState(rootAddress));
		mNetOps.setLastCloseTime(secondLedger->getCloseTimeNC());
	}
}

void Application::loadOldLedger(const std::string& l)
{
	try
	{
		Ledger::pointer loadLedger;
		if (l.empty() || (l == "latest"))
			loadLedger = Ledger::getLastFullLedger();
		else if (l.length() == 64)
		{ // by hash
			uint256 hash;
			hash.SetHex(l);
			loadLedger = Ledger::loadByHash(hash);
		}
		else // assume by sequence
			loadLedger = Ledger::loadByIndex(boost::lexical_cast<uint32>(l));

		if (!loadLedger)
		{
			cLog(lsFATAL) << "No Ledger found?" << std::endl;
			exit(-1);
		}
		loadLedger->setClosed();

		cLog(lsINFO) << "Loading ledger " << loadLedger->getHash() << " seq:" << loadLedger->getLedgerSeq();

		if (loadLedger->getAccountHash().isZero())
		{
			cLog(lsFATAL) << "Ledger is empty.";
			assert(false);
			exit(-1);
		}

		if (!loadLedger->walkLedger())
		{
			cLog(lsFATAL) << "Ledger is missing nodes.";
			exit(-1);
		}

		if (!loadLedger->assertSane())
		{
			cLog(lsFATAL) << "Ledger is not sane.";
			exit(-1);
		}
		mLedgerMaster.setLedgerRangePresent(loadLedger->getLedgerSeq(), loadLedger->getLedgerSeq());

		Ledger::pointer openLedger = boost::make_shared<Ledger>(false, boost::ref(*loadLedger));
		mLedgerMaster.switchLedgers(loadLedger, openLedger);
		mNetOps.setLastCloseTime(loadLedger->getCloseTimeNC());
	}
	catch (SHAMapMissingNode& mn)
	{
		cLog(lsFATAL) << "Data is missing for selected ledger";
		exit(-1);
	}
	catch (boost::bad_lexical_cast& blc)
	{
		cLog(lsFATAL) << "Ledger specified '" << l << "' is not valid";
		exit(-1);
	}
}

// vim:ts=4
