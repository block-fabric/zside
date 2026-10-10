// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/signmessage.h>
#include <key.h>
#include <key_io.h>
#include <test/util/common.h>
#include <test/util/setup_common.h>
#include <script/solver.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/wallet.h>
#include <wallet/test/util.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(scriptpubkeyman_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(DescriptorScriptPubKeyManTests)
{
    std::unique_ptr<interfaces::Chain>& chain = m_node.chain;

    CWallet keystore(chain.get(), "", CreateMockableWalletDatabase());
    auto key_scriptpath = GenerateRandomKey();

    // Verify that a SigningProvider for a pubkey is only returned if its corresponding private key is available
    auto key_internal = GenerateRandomKey();
    std::string desc_str = "tr(" + EncodeSecret(key_internal) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man1 = CreateDescriptor(keystore, desc_str, true);
    BOOST_CHECK(spk_man1 != nullptr);
    auto signprov_keypath_spendable = spk_man1->GetSigningProvider(key_internal.GetPubKey());
    BOOST_CHECK(signprov_keypath_spendable != nullptr);

    desc_str = "tr(" + HexStr(XOnlyPubKey::NUMS_H) + ",pk(" + HexStr(key_scriptpath.GetPubKey()) + "))";
    auto spk_man2 = CreateDescriptor(keystore, desc_str, true);
    BOOST_CHECK(spk_man2 != nullptr);
    auto signprov_keypath_nums_h = spk_man2->GetSigningProvider(XOnlyPubKey::NUMS_H.GetEvenCorrespondingCPubKey());
    BOOST_CHECK(signprov_keypath_nums_h == nullptr);
}

BOOST_AUTO_TEST_CASE(desc_spkm_topup_fail)
{
    // Attempting to construct a DescriptorSPKM that cannot be topped up (hardened derivation without private keys)
    // should throw even though it is valid and can be parsed
    CExtKey extkey;
    extkey.SetSeed(std::array<std::byte, 32>{});
    CWallet keystore(m_node.chain.get(), "", CreateMockableWalletDatabase());
    BOOST_CHECK_EXCEPTION(
        CreateDescriptor(keystore, "wpkh(" + EncodeExtPubKey(extkey.Neuter()) + "/*h)", /*success=*/true),
        std::runtime_error, HasReason("Could not top up scriptPubKeys"));
}

BOOST_AUTO_TEST_CASE(sign_message_with_taproot_key)
{
    // A message is signed with the key of a P2TR address by its key path: only with its private key.
    const auto output_of{[](const DescriptorScriptPubKeyMan& spk_man) {
        const auto scripts{spk_man.GetScriptPubKeys()};
        BOOST_REQUIRE_EQUAL(scripts.size(), 1U);
        CTxDestination dest;
        BOOST_REQUIRE(ExtractDestination(*scripts.begin(), dest));
        return std::get<WitnessV1Taproot>(dest);
    }};
    const std::string message{"a message"};
    const CKey key{GenerateRandomKey()};
    std::string signature;

    CWallet keystore(m_node.chain.get(), "", CreateMockableWalletDatabase());
    DescriptorScriptPubKeyMan* spendable{CreateDescriptor(keystore, "tr(" + EncodeSecret(key) + ")", true)};
    BOOST_REQUIRE(spendable);
    const WitnessV1Taproot output{output_of(*spendable)};
    BOOST_CHECK(spendable->SignMessage(message, output, signature) == SigningResult::OK);
    BOOST_CHECK(MessageVerify(EncodeDestination(output), signature, message) == MessageVerificationResult::OK);

    // An output that is not the descriptor's.
    const WitnessV1Taproot other{XOnlyPubKey{GenerateRandomKey().GetPubKey()}};
    signature.clear();
    BOOST_CHECK(spendable->SignMessage(message, other, signature) == SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    BOOST_CHECK(signature.empty());

    // The same descriptor without the private key: the address is the wallet's, the key is not.
    CWallet watch_only(m_node.chain.get(), "", CreateMockableWalletDatabase());
    watch_only.SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    DescriptorScriptPubKeyMan* public_only{CreateDescriptor(watch_only, "tr(" + HexStr(key.GetPubKey()) + ")", true)};
    BOOST_REQUIRE(public_only);
    BOOST_REQUIRE(output_of(*public_only) == output);
    BOOST_CHECK(public_only->SignMessage(message, output, signature) == SigningResult::PRIVATE_KEY_NOT_AVAILABLE);
    BOOST_CHECK(signature.empty());

    // A ScriptPubKeyMan that knows no keys at all signs nothing.
    ScriptPubKeyMan none{keystore};
    BOOST_CHECK(none.SignMessage(message, output, signature) == SigningResult::SIGNING_FAILED);
    BOOST_CHECK(none.SignMessage(message, PKHash{key.GetPubKey()}, signature) == SigningResult::SIGNING_FAILED);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
