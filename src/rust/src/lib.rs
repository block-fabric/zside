// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

//! The Orchard shielded pool of Zcash, for zSide, behind a C interface (see `zside/orchard.h`).
//!
//! Everything crosses the interface as bytes. Bundles are encoded as the Orchard part of a
//! Zcash v5 transaction (ZIP 225) and committed to as in ZIP 244. Bundles are always
//! `BundleVersion::orchard_v2` (spends and outputs enabled, cross-address transfers allowed),
//! proved and verified with the `FixedPostNu6_2` circuit: the original Orchard circuit was
//! unsound, and is never used.
//!
//! No function panics on bad input; failures are reported by the return value. The crate is
//! built with `panic = "abort"`, so a bug that panics stops the node rather than unwinding
//! into C++.

use std::slice;
use std::sync::OnceLock;

use incrementalmerkletree::{Hashable, Level};
use nonempty::NonEmpty;
use orchard::{
    builder::{Builder, BundleType, InProgress, Unauthorized, Unproven},
    bundle::{Authorized, BatchValidator, BundleVersion, Flags, TxVersion},
    circuit::{OrchardCircuitVersion, ProvingKey, VerifyingKey},
    keys::{FullViewingKey, Scope, SpendAuthorizingKey, SpendingKey},
    note::{ExtractedNoteCommitment, Nullifier, RandomSeed, Rho, TransmittedNoteCiphertext},
    primitives::redpallas::{self, Binding, SpendAuth},
    tree::{Anchor, MerkleHashOrchard, MerklePath},
    value::{NoteValue, ValueCommitment},
    Action, Address, Bundle, Note, Proof,
};
use rand_core::OsRng;

/// The bundles zSide uses.
const VERSION: BundleVersion = BundleVersion::orchard_v2();
const CIRCUIT: OrchardCircuitVersion = OrchardCircuitVersion::FixedPostNu6_2;
/// Most actions a bundle may have (the C++ side checks it too).
pub const MAX_ACTIONS: usize = 256;
/// Bytes of an action in a bundle: cv, nf, rk, cmx, epk, enc_ciphertext, out_ciphertext.
const ACTION_SIZE: usize = 32 * 5 + 580 + 80;
/// Bytes of a note as the wallet keeps it: recipient, value, rho, rseed.
pub const NOTE_SIZE: usize = 43 + 8 + 32 + 32;
/// Depth of the note commitment tree.
pub const TREE_DEPTH: usize = 32;

fn verifying_key() -> &'static VerifyingKey {
    static VK: OnceLock<VerifyingKey> = OnceLock::new();
    VK.get_or_init(|| VerifyingKey::build(CIRCUIT))
}

fn proving_key() -> &'static ProvingKey {
    static PK: OnceLock<ProvingKey> = OnceLock::new();
    PK.get_or_init(|| ProvingKey::build(CIRCUIT))
}

// Results.
pub const ZO_OK: i32 = 0;
pub const ZO_ERR_ENCODING: i32 = 1;
pub const ZO_ERR_INVALID: i32 = 2;
pub const ZO_ERR_KEY: i32 = 3;
pub const ZO_ERR_BUILD: i32 = 4;
pub const ZO_ERR_PROOF: i32 = 5;
pub const ZO_ERR_ANCHOR: i32 = 6;

unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if len == 0 {
        &[]
    } else {
        slice::from_raw_parts(ptr, len)
    }
}

unsafe fn array<const N: usize>(ptr: *const u8) -> [u8; N] {
    let mut out = [0u8; N];
    out.copy_from_slice(slice::from_raw_parts(ptr, N));
    out
}

unsafe fn write(out: *mut u8, data: &[u8]) {
    slice::from_raw_parts_mut(out, data.len()).copy_from_slice(data);
}

//
// Encoding (ZIP 225, the Orchard fields of a v5 transaction).
//

struct Reader<'a> {
    data: &'a [u8],
}

impl<'a> Reader<'a> {
    fn take(&mut self, n: usize) -> Option<&'a [u8]> {
        if self.data.len() < n {
            return None;
        }
        let (head, rest) = self.data.split_at(n);
        self.data = rest;
        Some(head)
    }
    fn array<const N: usize>(&mut self) -> Option<[u8; N]> {
        self.take(N).map(|b| {
            let mut a = [0u8; N];
            a.copy_from_slice(b);
            a
        })
    }
    /// Bitcoin's CompactSize, canonical only.
    fn compact_size(&mut self) -> Option<u64> {
        let first = self.take(1)?[0];
        let value = match first {
            0..=0xfc => first as u64,
            0xfd => u16::from_le_bytes(self.array()?) as u64,
            0xfe => u32::from_le_bytes(self.array()?) as u64,
            0xff => u64::from_le_bytes(self.array()?),
        };
        let canonical = match first {
            0xfd => value >= 0xfd,
            0xfe => value > 0xffff,
            0xff => value > 0xffff_ffff,
            _ => true,
        };
        canonical.then_some(value)
    }
}

fn write_compact_size(out: &mut Vec<u8>, n: u64) {
    if n < 0xfd {
        out.push(n as u8);
    } else if n <= 0xffff {
        out.push(0xfd);
        out.extend_from_slice(&(n as u16).to_le_bytes());
    } else if n <= 0xffff_ffff {
        out.push(0xfe);
        out.extend_from_slice(&(n as u32).to_le_bytes());
    } else {
        out.push(0xff);
        out.extend_from_slice(&n.to_le_bytes());
    }
}

fn read_bundle(data: &[u8]) -> Option<Bundle<Authorized, i64>> {
    let mut r = Reader { data };
    let n = r.compact_size()? as usize;
    if n == 0 || n > MAX_ACTIONS || r.data.len() < n * ACTION_SIZE {
        return None;
    }
    let mut parts = Vec::with_capacity(n);
    for _ in 0..n {
        let cv = Option::from(ValueCommitment::from_bytes(&r.array()?))?;
        let nf = Option::from(Nullifier::from_bytes(&r.array()?))?;
        let rk = redpallas::VerificationKey::<SpendAuth>::try_from(r.array::<32>()?).ok()?;
        let cmx = Option::from(ExtractedNoteCommitment::from_bytes(&r.array()?))?;
        let encrypted = TransmittedNoteCiphertext {
            epk_bytes: r.array()?,
            enc_ciphertext: r.array()?,
            out_ciphertext: r.array()?,
        };
        parts.push((nf, rk, cmx, encrypted, cv));
    }
    let flags = Flags::from_byte(r.take(1)?[0], VERSION)?;
    let value_balance = i64::from_le_bytes(r.array()?);
    let anchor = Option::from(Anchor::from_bytes(r.array()?))?;
    let proof_size = r.compact_size()? as usize;
    let proof = Proof::new(r.take(proof_size)?.to_vec());
    let mut actions = Vec::with_capacity(n);
    for (nf, rk, cmx, encrypted, cv) in parts {
        let signature = redpallas::Signature::<SpendAuth>::from(r.array::<64>()?);
        actions.push(Action::from_parts(nf, rk, cmx, encrypted, cv, signature).ok()?);
    }
    let binding = redpallas::Signature::<Binding>::from(r.array::<64>()?);
    if !r.data.is_empty() {
        return None;
    }
    let actions = NonEmpty::from_vec(actions)?;
    // Checks that the proof has its canonical size for the number of actions.
    Bundle::try_from_parts(actions, flags, value_balance, anchor, Authorized::from_parts(proof, binding), VERSION).ok()
}

fn write_bundle(bundle: &Bundle<Authorized, i64>) -> Vec<u8> {
    let mut out = Vec::new();
    write_compact_size(&mut out, bundle.actions().len() as u64);
    for action in bundle.actions().iter() {
        out.extend_from_slice(&action.cv_net().to_bytes());
        out.extend_from_slice(&action.nullifier().to_bytes());
        out.extend_from_slice(&<[u8; 32]>::from(action.rk()));
        out.extend_from_slice(&action.cmx().to_bytes());
        out.extend_from_slice(&action.encrypted_note().epk_bytes);
        out.extend_from_slice(&action.encrypted_note().enc_ciphertext);
        out.extend_from_slice(&action.encrypted_note().out_ciphertext);
    }
    out.push(bundle.flag_byte());
    out.extend_from_slice(&bundle.value_balance().to_le_bytes());
    out.extend_from_slice(&bundle.anchor().to_bytes());
    let proof = bundle.authorization().proof().as_ref();
    write_compact_size(&mut out, proof.len() as u64);
    out.extend_from_slice(proof);
    for action in bundle.actions().iter() {
        out.extend_from_slice(&<[u8; 64]>::from(action.authorization()));
    }
    out.extend_from_slice(&<[u8; 64]>::from(bundle.authorization().binding_signature()));
    out
}

//
// Bundles, for consensus.
//

/// A bundle read from a transaction.
pub struct ZoBundle {
    bundle: Bundle<Authorized, i64>,
}

/// Read a bundle. Returns null if the bytes are not exactly one well-formed, canonically encoded
/// bundle (which says nothing yet about its proof and signatures: see `zo_bundle_verify`).
#[no_mangle]
pub unsafe extern "C" fn zo_bundle_parse(data: *const u8, len: usize) -> *mut ZoBundle {
    let input = bytes(data, len);
    match read_bundle(input) {
        // One way to write a bundle: what is read is written back the same.
        Some(bundle) if write_bundle(&bundle) == input => Box::into_raw(Box::new(ZoBundle { bundle })),
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_free(bundle: *mut ZoBundle) {
    if !bundle.is_null() {
        drop(Box::from_raw(bundle));
    }
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_num_actions(bundle: *const ZoBundle) -> usize {
    (*bundle).bundle.actions().len()
}

/// What the bundle takes out of the shielded pool (negative: what it puts in).
#[no_mangle]
pub unsafe extern "C" fn zo_bundle_value_balance(bundle: *const ZoBundle) -> i64 {
    *(*bundle).bundle.value_balance()
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_flags(bundle: *const ZoBundle) -> u8 {
    (*bundle).bundle.flag_byte()
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_anchor(bundle: *const ZoBundle, out: *mut u8) {
    write(out, &(*bundle).bundle.anchor().to_bytes());
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_nullifier(bundle: *const ZoBundle, i: usize, out: *mut u8) {
    write(out, &(*bundle).bundle.actions()[i].nullifier().to_bytes());
}

#[no_mangle]
pub unsafe extern "C" fn zo_bundle_cmx(bundle: *const ZoBundle, i: usize, out: *mut u8) {
    write(out, &(*bundle).bundle.actions()[i].cmx().to_bytes());
}

/// The commitment to what the bundle does, without its proof and signatures (ZIP 244, v5): what
/// the sighash commits to in its place.
#[no_mangle]
pub unsafe extern "C" fn zo_bundle_commitment(bundle: *const ZoBundle, out: *mut u8) -> i32 {
    match (*bundle).bundle.commitment(TxVersion::V5) {
        Ok(c) => {
            write(out, c.0.as_bytes());
            ZO_OK
        }
        Err(_) => ZO_ERR_INVALID,
    }
}

/// The commitment of an empty bundle (ZIP 244, v5), for transactions without one.
#[no_mangle]
pub unsafe extern "C" fn zo_empty_bundle_commitment(out: *mut u8) {
    let hash = orchard::bundle::commitments::hash_bundle_txid_empty(VERSION.value_pool(), TxVersion::V5);
    if let Ok(h) = hash {
        write(out, h.as_bytes());
    }
}

/// Verify the proof and every signature of bundles against their sighashes, in one batch.
#[no_mangle]
pub unsafe extern "C" fn zo_bundles_verify(bundles: *const *const ZoBundle, sighashes: *const u8, count: usize) -> bool {
    let mut validator = BatchValidator::new(verifying_key());
    for i in 0..count {
        let bundle = &(**bundles.add(i)).bundle;
        let sighash = array::<32>(sighashes.add(32 * i));
        if validator.add_bundle(bundle, sighash).is_err() {
            return false;
        }
    }
    validator.validate(OsRng)
}

/// Build the verifying key now, rather than at the first bundle (it takes a moment).
#[no_mangle]
pub extern "C" fn zo_init_verifier() {
    verifying_key();
}

/// Build the proving key now, rather than at the first spend (it takes several seconds).
#[no_mangle]
pub extern "C" fn zo_init_prover() {
    proving_key();
}

//
// The note commitment tree.
//

#[no_mangle]
pub unsafe extern "C" fn zo_tree_empty_leaf(out: *mut u8) {
    write(out, &MerkleHashOrchard::empty_leaf().to_bytes());
}

#[no_mangle]
pub unsafe extern "C" fn zo_tree_empty_root(level: u8, out: *mut u8) {
    write(out, &MerkleHashOrchard::empty_root(Level::from(level)).to_bytes());
}

/// The parent of two nodes at `level` (0: leaves). False if a node is not a valid field element.
#[no_mangle]
pub unsafe extern "C" fn zo_tree_combine(level: u8, left: *const u8, right: *const u8, out: *mut u8) -> bool {
    let l = MerkleHashOrchard::from_bytes(&array(left));
    let r = MerkleHashOrchard::from_bytes(&array(right));
    match (Option::<MerkleHashOrchard>::from(l), Option::<MerkleHashOrchard>::from(r)) {
        (Some(l), Some(r)) => {
            write(out, &MerkleHashOrchard::combine(Level::from(level), &l, &r).to_bytes());
            true
        }
        _ => false,
    }
}

//
// Keys and addresses, for the wallet.
//

/// The spending key of an account (ZIP 32), from a seed of 32 to 252 bytes.
#[no_mangle]
pub unsafe extern "C" fn zo_spending_key_from_seed(seed: *const u8, len: usize, coin_type: u32, account: u32, out: *mut u8) -> i32 {
    let Ok(account) = zip32::AccountId::try_from(account) else { return ZO_ERR_KEY };
    match SpendingKey::from_zip32_seed(bytes(seed, len), coin_type, account) {
        Ok(sk) => {
            write(out, sk.to_bytes());
            ZO_OK
        }
        Err(_) => ZO_ERR_KEY,
    }
}

unsafe fn spending_key(sk: *const u8) -> Option<SpendingKey> {
    Option::from(SpendingKey::from_bytes(array(sk)))
}

unsafe fn full_viewing_key(fvk: *const u8) -> Option<FullViewingKey> {
    FullViewingKey::from_bytes(&array(fvk))
}

/// The full viewing key (96 bytes) of a spending key.
#[no_mangle]
pub unsafe extern "C" fn zo_full_viewing_key(sk: *const u8, out: *mut u8) -> i32 {
    let Some(sk) = spending_key(sk) else { return ZO_ERR_KEY };
    write(out, &FullViewingKey::from(&sk).to_bytes());
    ZO_OK
}

/// The address (43 bytes) at `index` of a full viewing key: external, to be paid at; or internal,
/// for change.
#[no_mangle]
pub unsafe extern "C" fn zo_address(fvk: *const u8, index: u64, internal: bool, out: *mut u8) -> i32 {
    let Some(fvk) = full_viewing_key(fvk) else { return ZO_ERR_KEY };
    let scope = if internal { Scope::Internal } else { Scope::External };
    let mut j = [0u8; 11];
    j[..8].copy_from_slice(&index.to_le_bytes());
    write(out, &fvk.address_at(zip32::DiversifierIndex::from(j), scope).to_raw_address_bytes());
    ZO_OK
}

#[no_mangle]
pub unsafe extern "C" fn zo_address_valid(address: *const u8) -> bool {
    Option::<Address>::from(Address::from_raw_address_bytes(&array(address))).is_some()
}

/// Whether an address belongs to a full viewing key: 0 no, 1 external, 2 internal.
#[no_mangle]
pub unsafe extern "C" fn zo_address_scope(fvk: *const u8, address: *const u8) -> i32 {
    let (Some(fvk), Some(address)) = (full_viewing_key(fvk), Option::<Address>::from(Address::from_raw_address_bytes(&array(address)))) else {
        return 0;
    };
    match fvk.scope_for_address(&address) {
        Some(Scope::External) => 1,
        Some(Scope::Internal) => 2,
        None => 0,
    }
}

//
// Notes, for the wallet.
//

fn write_note(note: &Note) -> [u8; NOTE_SIZE] {
    let mut out = [0u8; NOTE_SIZE];
    out[..43].copy_from_slice(&note.recipient().to_raw_address_bytes());
    out[43..51].copy_from_slice(&note.value().inner().to_le_bytes());
    out[51..83].copy_from_slice(&note.rho().to_bytes());
    out[83..].copy_from_slice(note.rseed().as_bytes());
    out
}

fn read_note(data: &[u8; NOTE_SIZE]) -> Option<Note> {
    let recipient = Option::from(Address::from_raw_address_bytes(data[..43].try_into().ok()?))?;
    let value = NoteValue::from_raw(u64::from_le_bytes(data[43..51].try_into().ok()?));
    let rho = Option::from(Rho::from_bytes(data[51..83].try_into().ok()?))?;
    let rseed = Option::from(RandomSeed::from_bytes(data[83..].try_into().ok()?, &rho))?;
    Option::from(Note::from_parts(recipient, value, rho, rseed, VERSION.note_version()))
}

/// Try to decrypt the output of action `i` with a full viewing key: the note (NOTE_SIZE bytes),
/// its memo (512 bytes) and whether it is to an internal (change) address. False if it is not
/// for this key.
#[no_mangle]
pub unsafe extern "C" fn zo_bundle_decrypt(bundle: *const ZoBundle, i: usize, fvk: *const u8, note_out: *mut u8, memo_out: *mut u8, internal: *mut bool) -> bool {
    let Some(fvk) = full_viewing_key(fvk) else { return false };
    let bundle = &(*bundle).bundle;
    for scope in [Scope::External, Scope::Internal] {
        if let Some((note, _, memo)) = bundle.decrypt_output_with_key(i, &fvk.to_ivk(scope)) {
            write(note_out, &write_note(&note));
            write(memo_out, &memo);
            *internal = scope == Scope::Internal;
            return true;
        }
    }
    false
}

/// Recover what the output of action `i` sent, with the outgoing viewing key of a full viewing
/// key: for payments this wallet made. The note and memo, as zo_bundle_decrypt gives them.
#[no_mangle]
pub unsafe extern "C" fn zo_bundle_recover(bundle: *const ZoBundle, i: usize, fvk: *const u8, note_out: *mut u8, memo_out: *mut u8) -> bool {
    let Some(fvk) = full_viewing_key(fvk) else { return false };
    let bundle = &(*bundle).bundle;
    for scope in [Scope::External, Scope::Internal] {
        if let Some((note, _, memo)) = bundle.recover_output_with_ovk(i, &fvk.to_ovk(scope)) {
            write(note_out, &write_note(&note));
            write(memo_out, &memo);
            return true;
        }
    }
    false
}

/// The value of a note.
#[no_mangle]
pub unsafe extern "C" fn zo_note_value(note: *const u8) -> u64 {
    u64::from_le_bytes(array::<8>(note.add(43)))
}

/// The nullifier of a note, which spending it reveals.
#[no_mangle]
pub unsafe extern "C" fn zo_note_nullifier(note: *const u8, fvk: *const u8, out: *mut u8) -> i32 {
    let (Some(note), Some(fvk)) = (read_note(&array(note)), full_viewing_key(fvk)) else { return ZO_ERR_KEY };
    write(out, &note.nullifier(&fvk).to_bytes());
    ZO_OK
}

/// The commitment of a note, as the tree holds it.
#[no_mangle]
pub unsafe extern "C" fn zo_note_cmx(note: *const u8, out: *mut u8) -> i32 {
    let Some(note) = read_note(&array(note)) else { return ZO_ERR_ENCODING };
    write(out, &ExtractedNoteCommitment::from(note.commitment()).to_bytes());
    ZO_OK
}

//
// Building bundles, for the wallet.
//

/// A bundle being built.
pub struct ZoBuilder {
    builder: Builder,
    keys: Vec<SpendAuthorizingKey>,
}

/// A bundle built and not proved nor signed yet.
pub struct ZoUnsigned {
    bundle: Bundle<InProgress<Unproven, Unauthorized>, i64>,
    keys: Vec<SpendAuthorizingKey>,
}

/// Start a bundle that spends notes of the tree with the root `anchor`.
#[no_mangle]
pub unsafe extern "C" fn zo_builder_new(anchor: *const u8) -> *mut ZoBuilder {
    let Some(anchor) = Option::<Anchor>::from(Anchor::from_bytes(array(anchor))) else { return std::ptr::null_mut() };
    match Builder::new(BundleType::DEFAULT, VERSION, VERSION.default_flags(), anchor) {
        Ok(builder) => Box::into_raw(Box::new(ZoBuilder { builder, keys: vec![] })),
        Err(_) => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn zo_builder_free(builder: *mut ZoBuilder) {
    if !builder.is_null() {
        drop(Box::from_raw(builder));
    }
}

/// Spend a note of the spending key `sk`, at `position` in the tree, with its path (32 nodes of
/// 32 bytes, from the leaves up).
#[no_mangle]
pub unsafe extern "C" fn zo_builder_add_spend(builder: *mut ZoBuilder, sk: *const u8, note: *const u8, position: u32, path: *const u8) -> i32 {
    let Some(sk) = spending_key(sk) else { return ZO_ERR_KEY };
    let Some(note) = read_note(&array(note)) else { return ZO_ERR_ENCODING };
    let mut auth_path = [MerkleHashOrchard::empty_leaf(); TREE_DEPTH];
    for (level, node) in auth_path.iter_mut().enumerate() {
        let Some(hash) = Option::<MerkleHashOrchard>::from(MerkleHashOrchard::from_bytes(&array(path.add(32 * level)))) else {
            return ZO_ERR_ENCODING;
        };
        *node = hash;
    }
    let fvk = FullViewingKey::from(&sk);
    match (*builder).builder.add_spend(fvk, note, MerklePath::from_parts(position, auth_path)) {
        Ok(()) => {
            (*builder).keys.push(SpendAuthorizingKey::from(&sk));
            ZO_OK
        }
        Err(orchard::builder::SpendError::AnchorMismatch) => ZO_ERR_ANCHOR,
        Err(_) => ZO_ERR_BUILD,
    }
}

/// Pay `value` to an address, with a memo (512 bytes). `ovk_fvk`, a full viewing key or null,
/// lets its holder recover the payment later.
#[no_mangle]
pub unsafe extern "C" fn zo_builder_add_output(builder: *mut ZoBuilder, ovk_fvk: *const u8, address: *const u8, value: u64, memo: *const u8) -> i32 {
    let Some(address) = Option::<Address>::from(Address::from_raw_address_bytes(&array(address))) else { return ZO_ERR_ENCODING };
    let ovk = if ovk_fvk.is_null() {
        None
    } else {
        let Some(fvk) = full_viewing_key(ovk_fvk) else { return ZO_ERR_KEY };
        Some(fvk.to_ovk(Scope::External))
    };
    match (*builder).builder.add_output(ovk, address, NoteValue::from_raw(value), array(memo)) {
        Ok(()) => ZO_OK,
        Err(_) => ZO_ERR_BUILD,
    }
}

/// Build the bundle (consuming the builder): null if it has nothing to do or cannot be built.
#[no_mangle]
pub unsafe extern "C" fn zo_builder_build(builder: *mut ZoBuilder) -> *mut ZoUnsigned {
    let ZoBuilder { builder, keys } = *Box::from_raw(builder);
    match builder.build::<i64>(OsRng) {
        Ok(Some((bundle, _))) => Box::into_raw(Box::new(ZoUnsigned { bundle, keys })),
        _ => std::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn zo_unsigned_free(unsigned: *mut ZoUnsigned) {
    if !unsigned.is_null() {
        drop(Box::from_raw(unsigned));
    }
}

#[no_mangle]
pub unsafe extern "C" fn zo_unsigned_value_balance(unsigned: *const ZoUnsigned) -> i64 {
    *(*unsigned).bundle.value_balance()
}

#[no_mangle]
pub unsafe extern "C" fn zo_unsigned_num_actions(unsigned: *const ZoUnsigned) -> usize {
    (*unsigned).bundle.actions().len()
}

/// The commitment the sighash takes for the bundle (as zo_bundle_commitment gives it once signed).
#[no_mangle]
pub unsafe extern "C" fn zo_unsigned_commitment(unsigned: *const ZoUnsigned, out: *mut u8) -> i32 {
    match (*unsigned).bundle.commitment(TxVersion::V5) {
        Ok(c) => {
            write(out, c.0.as_bytes());
            ZO_OK
        }
        Err(_) => ZO_ERR_INVALID,
    }
}

/// Prove and sign the bundle over `sighash` (consuming it), and encode it. The bytes are
/// returned in a buffer to free with zo_buffer_free.
#[no_mangle]
pub unsafe extern "C" fn zo_unsigned_finish(unsigned: *mut ZoUnsigned, sighash: *const u8, out: *mut *mut u8, out_len: *mut usize) -> i32 {
    let ZoUnsigned { bundle, keys } = *Box::from_raw(unsigned);
    let Ok(proven) = bundle.create_proof(proving_key(), OsRng) else { return ZO_ERR_PROOF };
    let Ok(signed) = proven.apply_signatures(OsRng, array(sighash), &keys) else { return ZO_ERR_BUILD };
    let encoded = write_bundle(&signed).into_boxed_slice();
    *out_len = encoded.len();
    *out = Box::into_raw(encoded) as *mut u8;
    ZO_OK
}

#[no_mangle]
pub unsafe extern "C" fn zo_buffer_free(buffer: *mut u8, len: usize) {
    if !buffer.is_null() {
        drop(Box::from_raw(slice::from_raw_parts_mut(buffer, len)));
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn build_verify_round_trip() {
        // A key, an address, and a bundle that pays it out of nothing (value balance -1000:
        // 1000 put into the pool).
        let mut sk = [0u8; 32];
        unsafe { assert_eq!(zo_spending_key_from_seed([7u8; 32].as_ptr(), 32, 1, 0, sk.as_mut_ptr()), ZO_OK) };
        let mut fvk = [0u8; 96];
        let mut addr = [0u8; 43];
        unsafe {
            assert_eq!(zo_full_viewing_key(sk.as_ptr(), fvk.as_mut_ptr()), ZO_OK);
            assert_eq!(zo_address(fvk.as_ptr(), 0, false, addr.as_mut_ptr()), ZO_OK);
            assert_eq!(zo_address_scope(fvk.as_ptr(), addr.as_ptr()), 1);
        }
        let empty = Anchor::empty_tree().to_bytes();
        let builder = unsafe { zo_builder_new(empty.as_ptr()) };
        unsafe { assert_eq!(zo_builder_add_output(builder, fvk.as_ptr(), addr.as_ptr(), 1000, [0u8; 512].as_ptr()), ZO_OK) };
        let unsigned = unsafe { zo_builder_build(builder) };
        assert!(!unsigned.is_null());
        assert_eq!(unsafe { zo_unsigned_value_balance(unsigned) }, -1000);
        let mut commitment = [0u8; 32];
        unsafe { assert_eq!(zo_unsigned_commitment(unsigned, commitment.as_mut_ptr()), ZO_OK) };
        let sighash = [42u8; 32];
        let (mut ptr, mut len) = (std::ptr::null_mut(), 0usize);
        unsafe { assert_eq!(zo_unsigned_finish(unsigned, sighash.as_ptr(), &mut ptr, &mut len), ZO_OK) };
        let encoded = unsafe { slice::from_raw_parts(ptr, len) }.to_vec();
        unsafe { zo_buffer_free(ptr, len) };

        let bundle = unsafe { zo_bundle_parse(encoded.as_ptr(), encoded.len()) };
        assert!(!bundle.is_null());
        let mut c2 = [0u8; 32];
        unsafe { assert_eq!(zo_bundle_commitment(bundle, c2.as_mut_ptr()), ZO_OK) };
        assert_eq!(commitment, c2, "the commitment does not cover the proof and signatures");
        let ptrs = [bundle as *const ZoBundle];
        assert!(unsafe { zo_bundles_verify(ptrs.as_ptr(), sighash.as_ptr(), 1) });
        assert!(!unsafe { zo_bundles_verify(ptrs.as_ptr(), [43u8; 32].as_ptr(), 1) }, "verified over another sighash");
        // The output decrypts to the address, with its value.
        let mut found = false;
        for i in 0..unsafe { zo_bundle_num_actions(bundle) } {
            let (mut note, mut memo, mut internal) = ([0u8; NOTE_SIZE], [0u8; 512], false);
            if unsafe { zo_bundle_decrypt(bundle, i, fvk.as_ptr(), note.as_mut_ptr(), memo.as_mut_ptr(), &mut internal) } {
                assert_eq!(unsafe { zo_note_value(note.as_ptr()) }, 1000);
                let mut cmx = [0u8; 32];
                let mut action_cmx = [0u8; 32];
                unsafe {
                    zo_note_cmx(note.as_ptr(), cmx.as_mut_ptr());
                    zo_bundle_cmx(bundle, i, action_cmx.as_mut_ptr());
                }
                assert_eq!(cmx, action_cmx);
                found = true;
            }
        }
        assert!(found);
        // A changed byte anywhere is refused or fails to verify.
        let mut bad = encoded.clone();
        let last = bad.len() - 1;
        bad[last] ^= 1;
        let b = unsafe { zo_bundle_parse(bad.as_ptr(), bad.len()) };
        if !b.is_null() {
            let p = [b as *const ZoBundle];
            assert!(!unsafe { zo_bundles_verify(p.as_ptr(), sighash.as_ptr(), 1) });
            unsafe { zo_bundle_free(b) };
        }
        // Padding the proof is refused.
        assert!(unsafe { zo_bundle_parse(encoded.as_ptr(), encoded.len() - 1) }.is_null());
        unsafe { zo_bundle_free(bundle) };
    }
}
