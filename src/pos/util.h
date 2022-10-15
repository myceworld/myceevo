#ifndef POS_UTIL_H
#define POS_UTIL_H

#include <chain.h>
#include <chainparams.h>
#include <index/disktxpos.h>
#include <index/txindex.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <pos/wallet.h>
#include <wallet/wallet.h>
#include <validation.h>

typedef std::vector<unsigned char> valtype;

class CTransaction;

bool GetCoinAge(const CTransaction& tx, const ChainstateManager& chainman, const CCoinsViewCache &view, uint64_t& nCoinAge, unsigned int nTimeTx, bool isTrueCoinAge);
bool SignBlock(CBlock& block, CStakeWallet& keystore);
bool CheckBlockSignature(const CBlock& block);

#endif // POS_UTIL_H
