// Copyright (c) 2022 The Myce Developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef POS_POS_H
#define POS_POS_H

#include <primitives/transaction.h>
#include <validation.h>

static const int MODIFIER_INTERVAL_RATIO = 3;

class CStakeInput;

// Logging defaults
static const bool DEFAULT_PRINTSTAKEMODIFIER = false;
static const bool DEFAULT_PRINTHASHPROOF = false;
static const bool DEFAULT_PRINTCOINAGE = false;

bool ComputeNextStakeModifier(Chainstate& chainstate, const CBlockIndex* pindexCurrent, uint64_t& nStakeModifier, bool& fGeneratedStakeModifier);
bool GetKernelStakeModifier(Chainstate& chainstate, CBlockIndex* pindexPrev, uint256 hashBlockFrom, uint64_t& nStakeModifier, int& nStakeModifierHeight, int64_t& nStakeModifierTime, bool fPrintProofOfStake = true);

bool CheckProofOfStake(const CBlock& block, uint256& hashProofOfStake, std::unique_ptr<CStakeInput>& stake, Chainstate& chainstate);
bool stakeTargetHit(const uint256& hashProofOfStake, const int64_t& nValueIn, const uint256& bnTargetPerCoinDay);
bool checkStake(const CDataStream& ssUniqueID, CAmount nValueIn, const uint64_t nStakeModifier, arith_uint256& bnTarget, unsigned int nTimeBlockFrom, unsigned int& nTimeTx, uint256& hashProofOfStake);
bool buildStake(CStakeInput* stakeInput, unsigned int nBits, unsigned int nTimeBlockFrom, unsigned int& nTimeTx, uint256& hashProofOfStake, Chainstate& chainstate);

#endif // POS_POS_H
