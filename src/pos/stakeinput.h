// Copyright (c) 2022 The Myce developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef POS_STAKEINPUT_H
#define POS_STAKEINPUT_H

class CDataStream;
class CKeyStore;
class CStakeWallet;
class CWallet;
class CWalletTx;

typedef std::vector<unsigned char> valtype;

// TODO: move this
const CAmount nStakeSplitThreshold = 10000;

class CStakeInput
{
protected:
    CBlockIndex* pindexFrom;

public:
    virtual ~CStakeInput(){};
    virtual CBlockIndex* GetIndexFrom(Chainstate& chainstate) = 0;
    virtual bool CreateTxIn(CStakeWallet* wallet, CTxIn& txIn, uint256 hashTxOut = uint256()) = 0;
    virtual bool GetTxFrom(CTransactionRef& tx) = 0;
    virtual CAmount GetValue() = 0;
    virtual bool CreateTxOuts(CStakeWallet* wallet, std::vector<CTxOut>& vout, CAmount nTotal) = 0;
    virtual bool GetModifier(uint64_t& nStakeModifier, Chainstate& chainstate) = 0;
    virtual CDataStream GetUniqueness() = 0;
};

class CMyceStake : public CStakeInput
{
private:
    CTransactionRef txFrom;
    unsigned int nPosition;

public:
    CMyceStake()
    {
        this->pindexFrom = nullptr;
    }

    bool SetInput(CTransactionRef txPrev, unsigned int n);

    CBlockIndex* GetIndexFrom(Chainstate& chainstate) override;
    bool GetTxFrom(CTransactionRef& tx) override;
    CAmount GetValue() override;
    bool GetModifier(uint64_t& nStakeModifier, Chainstate& chainstate) override;
    CDataStream GetUniqueness() override;
    bool CreateTxIn(CStakeWallet* wallet, CTxIn& txIn, uint256 hashTxOut = uint256()) override;
    bool CreateTxOuts(CStakeWallet* wallet, std::vector<CTxOut>& vout, CAmount nTotal) override;
};

#endif // POS_STAKEINPUT_H
