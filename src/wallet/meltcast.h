// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_MELTCAST_H
#define BITCOIN_WALLET_MELTCAST_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <serialize.h>
#include <uint256.h>

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/**
 * zSide: Melt/Cast (Paul Sztorc, https://www.truthcoin.info/blog/zside-meltcast/), in the wallet.
 *
 * The shielded pool hides who pays whom; what goes in and what comes out stays public, and can link
 * the two: 13.47 shielded and 13.47 unshielded an hour later. Melt and cast leave nothing to link.
 *
 * - Melt: each transparent coin goes into the pool alone (it is the only input of its transaction,
 *   whole, with no change), at a random height over the next blocks. No two coins are ever seen together.
 * - Cast: coins come out only as standard bills, 0.01 CHN times a power of two, each to a new
 *   address of the wallet, and only at the heights every wallet uses for that bill: a cast of
 *   0.64 looks like every other cast of 0.64 that day.
 *
 * The plan is kept in the wallet, and carried out by the wallet as the blocks come (it has to be
 * running, and unlocked, at those heights). Nothing of it is a rule of the chain.
 */
namespace wallet {
class CWallet;

namespace meltcast {
//! The smallest bill: 0.01 CHN. Bill k is BILL_UNIT << k.
static constexpr CAmount BILL_UNIT{1'000'000};
//! Bills 0.01 to 10485.76 CHN.
static constexpr int NUM_BILLS{21};
//! Blocks a bill's window stays open, from its height.
static constexpr int WINDOW{3};
//! Coins smaller than this, or than four times their fee, are not melted.
static constexpr CAmount MIN_MELT{100'000};

/** Blocks between the windows of a bill: a day on the main and test networks, 12 blocks on regtest. */
int CastPeriod();
/** Whether a cast of bill k may go out with the tip at `height`. */
bool InCastWindow(int bill, int height);
/** The first height at or after `from` in a window of bill k. */
int NextCastWindow(int bill, int from);
/** The bills an amount comes out as: its binary digits in units of BILL_UNIT, the rest stays in the pool. */
std::vector<int> Bills(CAmount amount);
inline CAmount BillAmount(int bill) { return BILL_UNIT << bill; }
} // namespace meltcast

/** A step of the plan: one coin to melt, or one bill to cast. */
struct MeltCastJob {
    static constexpr uint8_t MELT{0};
    static constexpr uint8_t CAST{1};

    uint64_t id{0};
    uint8_t kind{MELT};
    //! Melt: the coin.
    COutPoint coin;
    //! Melt: the coin's value; cast: the bill's.
    CAmount amount{0};
    //! Cast: the bill.
    int32_t bill{-1};
    //! The tip height from which it goes (a cast then waits for its window).
    int32_t due{0};
    int64_t created{0};
    //! The transaction, once made.
    uint256 txid;
    //! Why it could not go yet, if it tried.
    std::string error;
    //! The height it last tried at (not kept): once per block is enough.
    int tried{-1};

    bool Sent() const { return !txid.IsNull(); }

    SERIALIZE_METHODS(MeltCastJob, obj) { READWRITE(obj.id, obj.kind, obj.coin, obj.amount, obj.bill, obj.due, obj.created, obj.txid, obj.error); }
};

/** Plan a melt of every transparent coin of the wallet (in blocks), spread over the next `spread` blocks. Locks the coins. */
std::variant<std::vector<MeltCastJob>, std::string> ScheduleMelt(CWallet& wallet, int spread);
/** Plan a cast of `amount` out of the shielded account, each bill at one of its next `spread` + 1 windows. */
std::variant<std::vector<MeltCastJob>, std::string> ScheduleCast(CWallet& wallet, CAmount amount, int spread);
/** Drop a step that has not gone yet; true if it was there. */
bool CancelMeltCast(CWallet& wallet, uint64_t id);
/** Carry out what is due at the wallet's height. Returns how many transactions it made. */
int RunMeltCast(CWallet& wallet);
/** What the plan still holds back of the shielded account: the bills not cast yet. */
CAmount CastReserved(const CWallet& wallet);

} // namespace wallet

#endif // BITCOIN_WALLET_MELTCAST_H
