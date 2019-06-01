/*
 * This file is part of the Eccoin project
 * Copyright (c) 2012-2013 The PPCoin developers
 * Copyright (c) 2014-2018 The Eccoin developers
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <algorithm>

#include "args.h"
#include "blockstorage/blockstorage.h"
#include "chain/chain.h"
#include "consensus/consensus.h"
#include "crypto/scrypt.h"
#include "init.h"
#include "kernel.h"
#include "main.h"
#include "net/net.h"
#include "networks/netman.h"
#include "networks/networktemplate.h"
#include "script/stakescript.h"
#include "timedata.h"
#include "txdb.h"
#include "util/logger.h"
#include "util/utiltime.h"

// The stake modifier used to hash for a stake kernel is chosen as the stake
// modifier about a selection interval later than the coin generating the kernel
static bool GetKernelStakeModifier(uint256 hashBlockFrom, uint256 &nStakeModifier)
{
    nStakeModifier.SetNull();
    const CBlockIndex *pindex = pnetMan->getChainActive()->LookupBlockIndex(hashBlockFrom);
    if (!pindex)
    {
        return error("GetKernelStakeModifier() : block not indexed");
    }
    int blocksToGo = 5;
    if (pnetMan->getChainActive()->chainActive.Tip()->nHeight >= 1504350)
    {
        blocksToGo = 180;
    }
    while (pnetMan->getChainActive()->chainActive.Next(pindex) && blocksToGo > 0)
    {
        pindex = pnetMan->getChainActive()->chainActive.Next(pindex);
        blocksToGo = blocksToGo - 1;
    }
    if (blocksToGo > 0)
    {
        LogPrint("kernel", "blocks to go was %i and it should be 0 but we ran out of indexes \n", blocksToGo);
        return false;
    }

    CDataStream ss(SER_GETHASH, 0);
    ss << pindex->nStakeModifier;
    ss << pindex->hashProofOfStake;
    ss << pindex->pprev->nStakeModifier;
    ss << pindex->pprev->hashProofOfStake;
    ss << pindex->pprev->pprev->nStakeModifier;
    ss << pindex->pprev->pprev->hashProofOfStake;
    uint256 nStakeModifierNew = Hash(ss.begin(), ss.end());
    nStakeModifier = nStakeModifierNew;
    return true;
}

// Stake Modifier (hash modifier of proof-of-stake):
// The purpose of stake modifier is to prevent a txout (coin) owner from
// computing future proof-of-stake generated by this txout at the time
// of transaction confirmation. To meet kernel protocol, the txout
// must hash with a future stake modifier to generate the proof.
// Stake modifier consists of bits each of which is contributed from a
// selected block of a given block group in the past.
// The selection of a block is based on a hash of the block's proof-hash and
// the previous stake modifier.
// Stake modifier is recomputed at a fixed time interval instead of every
// block. This is to make it difficult for an attacker to gain control of
// additional bits in the stake modifier, even after generating a chain of
// blocks.
bool ComputeNextStakeModifier(const CBlockIndex *pindexPrev, const CTransaction &tx, uint256 &nStakeModifier)
{
    nStakeModifier.SetNull();
    if (tx.IsNull())
    {
        if (!pindexPrev)
        {
            return true; // genesis block's modifier is 0
        }
        return false;
    }
    if (tx.IsCoinBase())
    {
        /// if it isnt one of first 3 blocks run this calc then return. otherwise just return
        if (pindexPrev->pprev && pindexPrev->pprev->pprev)
        {
            CDataStream ss(SER_GETHASH, 0);
            ss << pindexPrev->nStakeModifier;
            ss << pindexPrev->hashProofOfStake;
            ss << pindexPrev->pprev->nStakeModifier;
            ss << pindexPrev->pprev->hashProofOfStake;
            ss << pindexPrev->pprev->pprev->nStakeModifier;
            ss << pindexPrev->pprev->pprev->hashProofOfStake;
            uint256 nStakeModifierNew = Hash(ss.begin(), ss.end());
            nStakeModifier = nStakeModifierNew;
        }
        return true;
    }

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn &txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTransaction txPrev;
    uint256 blockHashOfTx;
    if (!GetTransaction(txin.prevout.hash, txPrev, pnetMan->getActivePaymentNetwork()->GetConsensus(), blockHashOfTx))
        // previous transaction not in main chain, may occur during initial download
        return error("ComputeNextStakeModifier() : INFO: read txPrev failed");

    // Read block header
    CBlock block;
    CBlockIndex *index = pnetMan->getChainActive()->LookupBlockIndex(blockHashOfTx);

    {
        LOCK(cs_blockstorage);
        if (!ReadBlockFromDisk(block, index, pnetMan->getActivePaymentNetwork()->GetConsensus()))
        {
            // unable to read block of previous transaction
            LogPrint("kernel", "ComputeNextStakeModifier() : read block failed");
            return false;
        }
    }

    if (!GetKernelStakeModifier(block.GetHash(), nStakeModifier))
    {
        LogPrint("kernel", "ComputeNextStakeModifier(): GetKernelStakeModifier return false\n");
        return false;
    }
    return true;
}

// ppcoin kernel protocol
// coinstake must meet hash target according to the protocol:
// kernel (input 0) must meet the formula
//     hash(nStakeModifier + txPrev.block.nTime + txPrev.offset + txPrev.nTime + txPrev.vout.n + nTime) < bnTarget *
//     nCoinDayWeight
// this ensures that the chance of getting a coinstake is proportional to the
// amount of coin age one owns.
// The reason this hash is chosen is the following:
//   nStakeModifier:
//       (v0.3) scrambles computation to make it very difficult to precompute
//              future proof-of-stake at the time of the coin's confirmation
//       (v0.2) nBits (deprecated): encodes all past block timestamps
//   txPrev.block.nTime: prevent nodes from guessing a good timestamp to
//                       generate transaction for future advantage
//   txPrev.offset: offset of txPrev inside block, to reduce the chance of
//                  nodes generating coinstake at the same time
//   txPrev.nTime: reduce the chance of nodes generating coinstake at the same
//                 time
//   txPrev.vout.n: output number of txPrev, to reduce the chance of nodes
//                  generating coinstake at the same time
//   block/tx hash should not be used here as they can be generated in vast
//   quantities so as to generate blocks faster, degrading the system back into
//   a proof-of-work situation.
//
bool CheckStakeKernelHash(int nHeight,
    const CBlock &blockFrom,
    unsigned int nTxPrevOffset,
    const CTransaction &txPrev,
    const COutPoint &prevout,
    unsigned int nTimeTx,
    uint256 &hashProofOfStake)
{
    if (nTimeTx < txPrev.nTime) // Transaction timestamp violation
        return error("CheckStakeKernelHash() : nTime violation");

    unsigned int nTimeBlockFrom = blockFrom.GetBlockTime();
    if (nTimeBlockFrom + pnetMan->getActivePaymentNetwork()->getStakeMinAge() > nTimeTx) // Min age requirement
        return error("CheckStakeKernelHash() : min age violation");

    int64_t nValueIn = txPrev.vout[prevout.n].nValue;

    // v0.3 protocol kernel hash weight starts from 0 at the min age
    // this change increases active coins participating the hash and helps
    // to secure the network when proof-of-stake difficulty is low
    int64_t nTimeWeight = ((int64_t)nTimeTx - txPrev.nTime) - pnetMan->getActivePaymentNetwork()->getStakeMinAge();

    if (nTimeWeight <= 0)
    {
        LogPrint("kernel", "CheckStakeKernelHash(): ERROR: time weight was somehow <= 0 \n");
        return false;
    }

    // LogPrintf(">>> CheckStakeKernelHash: nTimeWeight = %"PRI64d"\n", nTimeWeight);
    // Calculate hash
    CDataStream ss(SER_GETHASH, 0);
    uint256 nStakeModifier;
    nStakeModifier.SetNull();

    if (!GetKernelStakeModifier(blockFrom.GetHash(), nStakeModifier))
    {
        LogPrint("kernel", ">>> CheckStakeKernelHash: GetKernelStakeModifier return false\n");
        return false;
    }
    // LogPrintf(">>> CheckStakeKernelHash: passed GetKernelStakeModifier\n");
    ss << nStakeModifier;

    ss << nTimeBlockFrom << nTxPrevOffset << txPrev.nTime << prevout.n << nTimeTx;
    hashProofOfStake = Hash(ss.begin(), ss.end());

    if (nHeight > 1504350)
    {
        arith_uint256 arith_hashProofOfStake = UintToArith256(hashProofOfStake);

        // the older the coins are, the higher the day weight. this means with a higher dayWeight you get a bigger
        // reduction in your hashProofOfStake
        // this should lead to older and older coins needing to be selected as the difficulty rises due to fast
        // block minting. larger inputs will also help this
        // but not nearly as much as older coins will because seconds in age are easier to earn compared to coin
        // amount. RNG with the result of the hash is also always a factor
        // nTimeWeight is the number of seconds old the coins are past the min stake age
        // nValueIn is the number of satoshis being staked so we divide by COIN to get the number of coins

        // This basically works out to: amount of satoshi * seconds old
        arith_uint256 reduction = arith_uint256(nTimeWeight) * arith_uint256(nValueIn);
        arith_uint256 hashTarget;
        bool fNegative;
        bool fOverflow;
        hashTarget.SetCompact(
            GetNextTargetRequired(pnetMan->getChainActive()->chainActive.Tip(), true), &fNegative, &fOverflow);
        if (fNegative || hashTarget == 0 || fOverflow ||
            hashTarget > UintToArith256(pnetMan->getActivePaymentNetwork()->GetConsensus().posLimit))
            return error("CheckStakeKernelHash(): nBits below minimum work for proof of stake");

        std::string reductionHex = reduction.GetHex();
        unsigned int n = std::count(reductionHex.begin(), reductionHex.end(), '0');
        unsigned int redux = 64 - n; // 64 is max 0's in a 256 bit hex string
        LogPrint("kernel", "reduction = %u \n", redux);
        LogPrint("kernel", "pre reduction hashProofOfStake = %s \n", arith_hashProofOfStake.GetHex().c_str());
        // before we apply reduction, we want to shift the hash 20 bits to the right. the PoS limit is lead by 20 0's so
        // we want our reduction to apply to a hashproofofstake that is also lead by 20 0's
        arith_hashProofOfStake = arith_hashProofOfStake >> 20;
        LogPrint("kernel", "mid reduction hashProofOfStake = %s \n", arith_hashProofOfStake.GetHex().c_str());
        arith_hashProofOfStake = arith_hashProofOfStake >> redux;
        LogPrint("kernel", "post reduction hashProofOfStake = %s \n", arith_hashProofOfStake.GetHex().c_str());
        // Now check if proof-of-stake hash meets target protocol
        if (arith_hashProofOfStake > hashTarget)
        {
            LogPrint("kernel", "CheckStakeKernelHash(): ERROR: hashProofOfStake %s > %s hashTarget\n",
                arith_hashProofOfStake.GetHex().c_str(), hashTarget.GetHex().c_str());
            return false;
        }
        LogPrint("kernel", "CheckStakeKernelHash(): SUCCESS: hashProofOfStake %s < %s hashTarget\n",
            arith_hashProofOfStake.GetHex().c_str(), hashTarget.GetHex().c_str());
    }

    return true;
}

// Check kernel hash target and coinstake signature
bool CheckProofOfStake(int nHeight, const CTransaction &tx, uint256 &hashProofOfStake)
{
    if (!tx.IsCoinStake())
        return error("CheckProofOfStake() : called on non-coinstake %s", tx.GetHash().ToString().c_str());

    // Kernel (input 0) must match the stake hash target per coin age (nBits)
    const CTxIn &txin = tx.vin[0];

    // First try finding the previous transaction in database
    CTransaction txPrev;
    uint256 blockHashOfTx;
    if (!GetTransaction(txin.prevout.hash, txPrev, pnetMan->getActivePaymentNetwork()->GetConsensus(), blockHashOfTx))
        // previous transaction not in main chain, may occur during initial download
        return error("CheckProofOfStake() : INFO: read txPrev failed");
    // Verify signature
    if (!VerifySignature(txPrev, tx, 0, true))
        return error("CheckProofOfStake() : VerifySignature failed on coinstake %s", tx.GetHash().ToString().c_str());

    // Read block header
    CBlock block;
    CBlockIndex *index = pnetMan->getChainActive()->LookupBlockIndex(blockHashOfTx);

    {
        LOCK(cs_blockstorage);
        if (!ReadBlockFromDisk(block, index, pnetMan->getActivePaymentNetwork()->GetConsensus()))
        {
            LogPrint("kernel", "CheckProofOfStake() : read block failed");
            return false;
        }
    }

    CDiskTxPos txindex;
    pnetMan->getChainActive()->pblocktree->ReadTxIndex(txPrev.GetHash(), txindex);
    if (nHeight < 1505775)
    {
        if (!CheckStakeKernelHash(
                nHeight, block, txindex.nTxOffset + 80, txPrev, txin.prevout, tx.nTime, hashProofOfStake))
        {
            // may occur during initial download or if behind on block chain sync
            return error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s",
                tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str());
        }
    }
    else
    {
        if (!CheckStakeKernelHash(nHeight, block, txindex.nTxOffset, txPrev, txin.prevout, tx.nTime, hashProofOfStake))
        {
            // may occur during initial download or if behind on block chain sync
            return error("CheckProofOfStake() : INFO: check kernel failed on coinstake %s, hashProof=%s",
                tx.GetHash().ToString().c_str(), hashProofOfStake.ToString().c_str());
        }
    }
    return true;
}
