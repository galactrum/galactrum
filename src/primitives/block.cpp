// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2017 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <primitives/block.h>

#include <hash.h>
#include <tinyformat.h>
#include <utilstrencodings.h>
#include <crypto/common.h>


uint256 CBlockHeader::GetHash() const
{
    return lyra2re2_hash(BEGIN(nVersion), END(nNonce));
}

uint256 CBlock::GetTPoSHash()
{
    return lyra2re2_hash(BEGIN(nVersion), END(hashTPoSContractTx));
}

bool CBlock::IsProofOfStake() const
{
    return (vtx.size() > 1 && vtx[1]->IsCoinStake());
}

bool CBlock::IsTPoSBlock() const
{
    return IsProofOfStake() && !hashTPoSContractTx.IsNull();
}

bool CBlock::IsProofOfWork() const
{
    return !IsProofOfStake();
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (const auto& tx : vtx) {
        s << "  " << tx->ToString() << "\n";
    }
    return s.str();
}
