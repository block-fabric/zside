// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZSIDE_ORCHARD_H
#define BITCOIN_ZSIDE_ORCHARD_H

#include <cstddef>
#include <cstdint>

/**
 * The Orchard shielded pool of Zcash: the Rust library in src/rust (zside_orchard), behind a C
 * interface. Bundles are encoded as in a Zcash v5 transaction (ZIP 225); keys, addresses and
 * notes are bytes. See src/rust/src/lib.rs.
 */
extern "C" {

struct ZoBundle;
struct ZoBuilder;
struct ZoUnsigned;

inline constexpr int ZO_OK{0};
inline constexpr int ZO_ERR_ENCODING{1};
inline constexpr int ZO_ERR_INVALID{2};
inline constexpr int ZO_ERR_KEY{3};
inline constexpr int ZO_ERR_BUILD{4};
inline constexpr int ZO_ERR_PROOF{5};
inline constexpr int ZO_ERR_ANCHOR{6};

// Bundles.
ZoBundle* zo_bundle_parse(const uint8_t* data, size_t len);
void zo_bundle_free(ZoBundle* bundle);
size_t zo_bundle_num_actions(const ZoBundle* bundle);
int64_t zo_bundle_value_balance(const ZoBundle* bundle);
uint8_t zo_bundle_flags(const ZoBundle* bundle);
void zo_bundle_anchor(const ZoBundle* bundle, uint8_t* out32);
void zo_bundle_nullifier(const ZoBundle* bundle, size_t i, uint8_t* out32);
void zo_bundle_cmx(const ZoBundle* bundle, size_t i, uint8_t* out32);
int zo_bundle_commitment(const ZoBundle* bundle, uint8_t* out32);
void zo_empty_bundle_commitment(uint8_t* out32);
bool zo_bundles_verify(const ZoBundle* const* bundles, const uint8_t* sighashes, size_t count);
void zo_init_verifier();
void zo_init_prover();

// The note commitment tree.
void zo_tree_empty_leaf(uint8_t* out32);
void zo_tree_empty_root(uint8_t level, uint8_t* out32);
bool zo_tree_combine(uint8_t level, const uint8_t* left32, const uint8_t* right32, uint8_t* out32);

// Keys and addresses.
int zo_spending_key_from_seed(const uint8_t* seed, size_t len, uint32_t coin_type, uint32_t account, uint8_t* out32);
int zo_full_viewing_key(const uint8_t* sk32, uint8_t* out96);
int zo_address(const uint8_t* fvk96, uint64_t index, bool internal, uint8_t* out43);
bool zo_address_valid(const uint8_t* address43);
int zo_address_scope(const uint8_t* fvk96, const uint8_t* address43);

// Notes.
bool zo_bundle_decrypt(const ZoBundle* bundle, size_t i, const uint8_t* fvk96, uint8_t* note_out, uint8_t* memo_out512, bool* internal);
bool zo_bundle_recover(const ZoBundle* bundle, size_t i, const uint8_t* fvk96, uint8_t* note_out, uint8_t* memo_out512);
uint64_t zo_note_value(const uint8_t* note);
int zo_note_nullifier(const uint8_t* note, const uint8_t* fvk96, uint8_t* out32);
int zo_note_cmx(const uint8_t* note, uint8_t* out32);

// Building.
ZoBuilder* zo_builder_new(const uint8_t* anchor32);
void zo_builder_free(ZoBuilder* builder);
int zo_builder_add_spend(ZoBuilder* builder, const uint8_t* sk32, const uint8_t* note, uint32_t position, const uint8_t* path);
int zo_builder_add_output(ZoBuilder* builder, const uint8_t* ovk_fvk96, const uint8_t* address43, uint64_t value, const uint8_t* memo512);
ZoUnsigned* zo_builder_build(ZoBuilder* builder);
void zo_unsigned_free(ZoUnsigned* unsigned_bundle);
int64_t zo_unsigned_value_balance(const ZoUnsigned* unsigned_bundle);
size_t zo_unsigned_num_actions(const ZoUnsigned* unsigned_bundle);
int zo_unsigned_commitment(const ZoUnsigned* unsigned_bundle, uint8_t* out32);
int zo_unsigned_finish(ZoUnsigned* unsigned_bundle, const uint8_t* sighash32, uint8_t** out, size_t* out_len);
void zo_buffer_free(uint8_t* buffer, size_t len);

} // extern "C"

namespace zside {
/** Bytes of a note as the wallet keeps it: recipient, value, rho, rseed. */
inline constexpr size_t NOTE_SIZE{43 + 8 + 32 + 32};
inline constexpr size_t ADDRESS_SIZE{43};
inline constexpr size_t FVK_SIZE{96};
inline constexpr size_t MEMO_SIZE{512};
inline constexpr unsigned TREE_DEPTH{32};
} // namespace zside

#endif // BITCOIN_ZSIDE_ORCHARD_H
