#include "SerializedLedger.h"

#include <boost/format.hpp>

#include "Ledger.h"
#include "Log.h"

DECLARE_INSTANCE(SerializedLedgerEntry)
SETUP_LOG();

SerializedLedgerEntry::SerializedLedgerEntry(SerializerIterator& sit, const uint256& index)
	: STObject(sfLedgerEntry), mIndex(index)
{
	set(sit);
	uint16 type = getFieldU16(sfLedgerEntryType);
	mFormat = LedgerEntryFormat::getLgrFormat(static_cast<LedgerEntryType>(type));
	if (mFormat == NULL)
		throw std::runtime_error("invalid ledger entry type");
	mType = mFormat->t_type;
	if (!setType(mFormat->elements))
		throw std::runtime_error("ledger entry not valid for type");
}

SerializedLedgerEntry::SerializedLedgerEntry(const Serializer& s, const uint256& index)
	: STObject(sfLedgerEntry), mIndex(index)
{
	SerializerIterator sit(const_cast<Serializer&>(s)); // we know 's' isn't going away
	set(sit);

	uint16 type = getFieldU16(sfLedgerEntryType);
	mFormat = LedgerEntryFormat::getLgrFormat(static_cast<LedgerEntryType>(type));
	if (mFormat == NULL)
		throw std::runtime_error("invalid ledger entry type");
	mType = mFormat->t_type;
	if (!setType(mFormat->elements))
	{
		cLog(lsWARNING) << "Ledger entry not valid for type " << mFormat->t_name;
		cLog(lsWARNING) << getJson(0);
		throw std::runtime_error("ledger entry not valid for type");
	}
}

SerializedLedgerEntry::SerializedLedgerEntry(LedgerEntryType type, const uint256& index) :
	STObject(sfLedgerEntry), mIndex(index), mType(type)
{
	mFormat = LedgerEntryFormat::getLgrFormat(type);
	if (mFormat == NULL) throw std::runtime_error("invalid ledger entry type");
	set(mFormat->elements);
	setFieldU16(sfLedgerEntryType, static_cast<uint16>(mFormat->t_type));
}

std::string SerializedLedgerEntry::getFullText() const
{
	std::string ret = "\"";
	ret += mIndex.GetHex();
	ret += "\" = { ";
	ret += mFormat->t_name;
	ret += ", ";
	ret += STObject::getFullText();
	ret += "}";
	return ret;
}

std::string SerializedLedgerEntry::getText() const
{
	return str(boost::format("{ %s, %s }")
		% mIndex.GetHex()
		% STObject::getText());
}

Json::Value SerializedLedgerEntry::getJson(int options) const
{
	Json::Value ret(STObject::getJson(options));

	ret["index"]	= mIndex.GetHex();

	return ret;
}

bool SerializedLedgerEntry::isThreadedType()
{
	return getFieldIndex(sfPreviousTxnID) != -1;
}

bool SerializedLedgerEntry::isThreaded()
{
	return isFieldPresent(sfPreviousTxnID);
}

uint256 SerializedLedgerEntry::getThreadedTransaction()
{
	return getFieldH256(sfPreviousTxnID);
}

uint32 SerializedLedgerEntry::getThreadedLedger()
{
	return getFieldU32(sfPreviousTxnLgrSeq);
}

bool SerializedLedgerEntry::thread(const uint256& txID, uint32 ledgerSeq, uint256& prevTxID, uint32& prevLedgerID)
{
	uint256 oldPrevTxID = getFieldH256(sfPreviousTxnID);
	cLog(lsTRACE) << "Thread Tx:" << txID << " prev:" << oldPrevTxID;
	if (oldPrevTxID == txID)
	{ // this transaction is already threaded
		assert(getFieldU32(sfPreviousTxnLgrSeq) == ledgerSeq);
		return false;
	}
	prevTxID = oldPrevTxID;
	prevLedgerID = getFieldU32(sfPreviousTxnLgrSeq);
	setFieldH256(sfPreviousTxnID, txID);
	setFieldU32(sfPreviousTxnLgrSeq, ledgerSeq);
	return true;
}

bool SerializedLedgerEntry::hasOneOwner()
{
	return (mType != ltACCOUNT_ROOT) && (getFieldIndex(sfAccount) != -1);
}

bool SerializedLedgerEntry::hasTwoOwners()
{
	return mType == ltRIPPLE_STATE;
}

RippleAddress SerializedLedgerEntry::getOwner()
{
	return getFieldAccount(sfAccount);
}

RippleAddress SerializedLedgerEntry::getFirstOwner()
{
	return RippleAddress::createAccountID(getFieldAmount(sfLowLimit).getIssuer());
}

RippleAddress SerializedLedgerEntry::getSecondOwner()
{
	return RippleAddress::createAccountID(getFieldAmount(sfHighLimit).getIssuer());
}

std::vector<uint256> SerializedLedgerEntry::getOwners()
{
	std::vector<uint256> owners;
	uint160 account;

	for (int i = 0, fields = getCount(); i < fields; ++i)
	{
		SField::ref fc = getFieldSType(i);
		if ((fc == sfAccount) || (fc == sfOwner))
		{
				const STAccount* entry = dynamic_cast<const STAccount *>(peekAtPIndex(i));
				if ((entry != NULL) && entry->getValueH160(account))
					owners.push_back(Ledger::getAccountRootIndex(account));
		}
		if ((fc == sfLowLimit) || (fc == sfHighLimit))
		{
			const STAmount* entry = dynamic_cast<const STAmount *>(peekAtPIndex(i));
			if ((entry != NULL))
			{
				uint160 issuer = entry->getIssuer();
				if (issuer.isNonZero())
					owners.push_back(Ledger::getAccountRootIndex(issuer));
			}
		}
	}

	return owners;
}

// vim:ts=4
