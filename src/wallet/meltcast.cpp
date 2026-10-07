// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/meltcast.h>

#include <chainparams.h>
#include <interfaces/chain.h>
#include <random.h>
#include <util/moneystr.h>
#include <util/time.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/shielded.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>

#include <algorithm>

namespace wallet {
namespace meltcast {
int CastPeriod()
{
    return Params().GetChainType() == ChainType::REGTEST ? 12 : 144;
}

namespace {
/** Where in the period bill k's window opens: the bills' windows are spread over the period. */
int Offset(int bill)
{
    return (bill * 7) % CastPeriod();
}

int Mod(int a, int m)
{
    return ((a % m) + m) % m;
}
} // namespace

bool InCastWindow(int bill, int height)
{
    return Mod(height - Offset(bill), CastPeriod()) < WINDOW;
}

int NextCastWindow(int bill, int from)
{
    const int into{Mod(from - Offset(bill), CastPeriod())};
    return into < WINDOW ? from : from + CastPeriod() - into;
}

std::vector<int> Bills(CAmount amount)
{
    std::vector<int> bills;
    const CAmount units{amount / BILL_UNIT};
    for (int k{NUM_BILLS - 1}; k >= 0; --k) {
        if (units & (CAmount{1} << k)) bills.push_back(k);
    }
    return bills;
}
} // namespace meltcast

namespace {
using namespace meltcast;

uint64_t NextId(const CWallet& wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const auto& jobs{wallet.m_shielded.jobs};
    return jobs.empty() ? 1 : jobs.rbegin()->first + 1;
}

void Keep(CWallet& wallet, const MeltCastJob& job) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    wallet.m_shielded.jobs[job.id] = job;
    WalletBatch{wallet.GetDatabase()}.WriteMeltCastJob(job);
}

/** The fee a melt pays, about: a coin in, a bundle of two actions out. */
CAmount MeltFee(const CWallet& wallet)
{
    return GetMinimumFee(GetMinimumFeeRate(wallet, CCoinControl{}), BundleSize(ActionCount(0, 1)) + 160);
}

/** The fee a cast of one bill pays, about: a bundle of two actions and a transparent output. */
CAmount CastFee(const CWallet& wallet)
{
    return GetMinimumFee(GetMinimumFeeRate(wallet, CCoinControl{}), BundleSize(ActionCount(2, 2)) + 120);
}
} // namespace

CAmount CastReserved(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    CAmount reserved{0};
    for (const auto& [id, job] : wallet.m_shielded.jobs) {
        if (job.kind == MeltCastJob::CAST && !job.Sent()) reserved += job.amount;
    }
    return reserved;
}

std::variant<std::vector<MeltCastJob>, std::string> ScheduleMelt(CWallet& wallet, int spread)
{
    LOCK(wallet.cs_wallet);
    if (spread < 0) return std::string{"spread must not be negative"};
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet is locked, or has no private keys"};
    const int height{wallet.GetLastBlockHeight()};
    CCoinControl coin_control;
    coin_control.m_min_depth = 1;
    // A coin is worth melting if its fee is a small part of it.
    const CAmount smallest{std::max(MIN_MELT, 4 * MeltFee(wallet))};
    FastRandomContext rng;
    std::vector<MeltCastJob> planned;
    // Coins locked already are left alone: those of a melt planned before among them.
    for (const COutput& out : AvailableCoins(wallet, &coin_control).All()) {
        if (out.txout.nValue < smallest) continue;
        MeltCastJob job;
        job.id = NextId(wallet);
        job.kind = MeltCastJob::MELT;
        job.coin = out.outpoint;
        job.amount = out.txout.nValue;
        job.due = height + 1 + static_cast<int>(rng.randrange(spread + 1));
        job.created = GetTime();
        wallet.LockCoin(out.outpoint, /*persist=*/true);
        Keep(wallet, job);
        planned.push_back(job);
    }
    if (planned.empty()) return std::string{strprintf("No transparent coin to melt: coins in blocks, of %s at least (four times the fee), that are not locked", FormatMoney(smallest))};
    return planned;
}

std::variant<std::vector<MeltCastJob>, std::string> ScheduleCast(CWallet& wallet, CAmount amount, int spread)
{
    LOCK(wallet.cs_wallet);
    if (spread < 0) return std::string{"spread must not be negative"};
    if (!wallet.m_shielded.fvk) return std::string{"The wallet has no shielded account yet"};
    const std::vector<int> bills{Bills(amount)};
    if (amount <= 0 || amount >= BillAmount(NUM_BILLS)) return strprintf("The amount must be between %s and %s", FormatMoney(BILL_UNIT), FormatMoney(BillAmount(NUM_BILLS) - 1));
    if (bills.empty()) return strprintf("The smallest bill is %s", FormatMoney(BILL_UNIT));
    const ShieldedBalance balance{GetShieldedBalance(wallet)};
    const CAmount needed{amount / BILL_UNIT * BILL_UNIT + static_cast<CAmount>(bills.size()) * CastFee(wallet)};
    const CAmount free{balance.available + balance.pending - CastReserved(wallet)};
    if (needed > free) return strprintf("Not enough in the shielded account: %s with the fees, %s free (casts planned already hold back %s)", FormatMoney(needed), FormatMoney(free), FormatMoney(CastReserved(wallet)));

    const int height{wallet.GetLastBlockHeight()};
    FastRandomContext rng;
    std::vector<MeltCastJob> planned;
    for (const int bill : bills) {
        MeltCastJob job;
        job.id = NextId(wallet);
        job.kind = MeltCastJob::CAST;
        job.amount = BillAmount(bill);
        job.bill = bill;
        job.due = NextCastWindow(bill, height + 1 + static_cast<int>(rng.randrange(spread + 1)) * CastPeriod());
        job.created = GetTime();
        Keep(wallet, job);
        planned.push_back(job);
    }
    return planned;
}

bool CancelMeltCast(CWallet& wallet, uint64_t id)
{
    LOCK(wallet.cs_wallet);
    const auto it{wallet.m_shielded.jobs.find(id)};
    if (it == wallet.m_shielded.jobs.end()) return false;
    if (it->second.Sent()) {
        // A step sent can be dropped only if its transaction will never be mined: abandoned, or lost to a conflict.
        const CWalletTx* wtx{wallet.GetWalletTx(Txid::FromUint256(it->second.txid))};
        if (wtx && !wtx->isAbandoned() && wallet.GetTxDepthInMainChain(*wtx) >= 0) return false;
    }
    if (it->second.kind == MeltCastJob::MELT) wallet.UnlockCoin(it->second.coin);
    WalletBatch{wallet.GetDatabase()}.EraseMeltCastJob(id);
    wallet.m_shielded.jobs.erase(it);
    return true;
}

int RunMeltCast(CWallet& wallet)
{
    std::vector<uint64_t> due;
    int height;
    {
        LOCK(wallet.cs_wallet);
        height = wallet.GetLastBlockHeight();
        for (const auto& [id, job] : wallet.m_shielded.jobs) {
            if (job.Sent() || height < job.due || job.tried == height) continue;
            if (job.kind == MeltCastJob::CAST && !InCastWindow(job.bill, height)) continue;
            due.push_back(id);
        }
    }
    if (due.empty() || wallet.chain().isInitialBlockDownload()) return 0;

    int made{0};
    for (const uint64_t id : due) {
        LOCK(wallet.cs_wallet);
        auto it{wallet.m_shielded.jobs.find(id)};
        if (it == wallet.m_shielded.jobs.end()) continue;
        MeltCastJob job{it->second};
        it->second.tried = height;
        job.tried = height;
        if (wallet.IsLocked()) {
            if (job.error.empty()) {
                job.error = "Waiting for the wallet to be unlocked";
                Keep(wallet, job);
            }
            continue;
        }
        std::variant<CTransactionRef, std::string> result{std::string{}};
        if (job.kind == MeltCastJob::MELT) {
            if (wallet.IsSpent(job.coin) || !wallet.GetWalletTx(job.coin.hash)) {
                // Spent some other way (the coin was unlocked and sent): nothing left to melt.
                wallet.WalletLogPrintf("Melt: coin %s was spent otherwise, dropped from the plan\n", job.coin.ToString());
                WalletBatch{wallet.GetDatabase()}.EraseMeltCastJob(id);
                wallet.m_shielded.jobs.erase(it);
                continue;
            }
            result = ShieldCoin(wallet, job.coin, *ShieldedAddress(wallet, 0));
        } else {
            // Each bill to a new address of the wallet: no two bills share one.
            const auto dest{wallet.GetNewDestination(OutputType::BECH32, "cast")};
            if (!dest) {
                result = util::ErrorString(dest).original;
            } else {
                ShieldedRecipient recipient;
                recipient.script = GetScriptForDestination(*dest);
                recipient.amount = job.amount;
                result = SendShielded(wallet, {recipient});
            }
        }
        if (const auto* tx{std::get_if<CTransactionRef>(&result)}) {
            job.txid = (*tx)->GetHash().ToUint256();
            // The coin is spent now: no need to keep it from other uses.
            if (job.kind == MeltCastJob::MELT) wallet.UnlockCoin(job.coin);
            job.error.clear();
            ++made;
            wallet.WalletLogPrintf("%s %s at height %d: %s\n", job.kind == MeltCastJob::MELT ? "Melted" : "Cast", FormatMoney(job.amount), height, job.txid.GetHex());
        } else {
            // It tries again: a melt at the next block, a cast at the next window of its bill.
            job.error = std::get<std::string>(result);
            wallet.WalletLogPrintf("%s %s could not go at height %d: %s\n", job.kind == MeltCastJob::MELT ? "Melt" : "Cast", FormatMoney(job.amount), height, job.error);
        }
        Keep(wallet, job);
    }
    return made;
}

} // namespace wallet
