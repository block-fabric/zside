#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""The drivechain messages and outputs (src/drivechain/sidechain.h), for tests that build blocks by hand.

Hashes are given as the RPCs show them (hex, most significant byte first) and put in scripts in
the byte order of the uint256 (least significant byte first).
"""
import hashlib

from .script import CScript, OP_RETURN

OP_DRIVECHAIN = 0xb4

PROPOSAL_TAG = bytes.fromhex("d5e0c4af")
ACK_TAG = bytes.fromhex("d6e1c5df")
BUNDLE_TAG = bytes.fromhex("d45aa943")
VOTE_TAG = bytes.fromhex("d77d1776")
BMM_ACCEPT_TAG = bytes.fromhex("d1617368")
BMM_REQUEST_TAG = bytes.fromhex("00bf00")

VOTE_REPEAT_PREVIOUS = 0x00
VOTE_ONE_BYTE = 0x01
VOTE_TWO_BYTES = 0x02
VOTE_LEADING_BY_50 = 0x03
VOTE_ABSTAIN = 0xffff
VOTE_DOWNVOTE = 0xfffe


def hash_bytes(hex_hash):
    """The bytes of a uint256 given as an RPC shows it."""
    return bytes.fromhex(hex_hash)[::-1]


def message(tag, payload):
    return CScript([OP_RETURN, tag + payload])


def escrow_script(slot):
    return CScript(bytes([OP_DRIVECHAIN, 1, slot, 0x51]))


def destination_script(destination):
    return CScript([OP_RETURN, destination.encode() if isinstance(destination, str) else destination])


def description(title, text="", hash_id1="00" * 32, hash_id2="00" * 20, version=0):
    title = title.encode()
    return bytes([version, len(title)]) + title + text.encode() + bytes.fromhex(hash_id1)[::-1] + bytes.fromhex(hash_id2)[::-1]


def proposal_hash(title, **kwargs):
    """The hash of a proposal, as the RPCs show it (sha256d of its description)."""
    return hashlib.sha256(hashlib.sha256(description(title, **kwargs)).digest()).digest()[::-1].hex()


def proposal_script(slot, title, **kwargs):
    return message(PROPOSAL_TAG, bytes([slot]) + description(title, **kwargs))


def ack_script(slot, hex_hash):
    return message(ACK_TAG, bytes([slot]) + hash_bytes(hex_hash))


def bundle_script(slot, hex_hash):
    return message(BUNDLE_TAG, bytes([slot]) + hash_bytes(hex_hash))


def vote_script(votes, form=None):
    """One vote per active sidechain: a bundle index, VOTE_ABSTAIN or VOTE_DOWNVOTE."""
    if form is None:
        form = VOTE_TWO_BYTES if any(0xfd < v < VOTE_DOWNVOTE for v in votes) else VOTE_ONE_BYTE
    payload = bytes([form])
    for v in votes:
        if form == VOTE_ONE_BYTE:
            payload += bytes([0xff if v == VOTE_ABSTAIN else 0xfe if v == VOTE_DOWNVOTE else v])
        elif form == VOTE_TWO_BYTES:
            payload += v.to_bytes(2, "little")
    return message(VOTE_TAG, payload)


def bmm_accept_script(slot, side_hash):
    return message(BMM_ACCEPT_TAG, bytes([slot]) + hash_bytes(side_hash))


def bmm_request_script(slot, side_hash, prev_main_hash):
    return message(BMM_REQUEST_TAG, bytes([slot]) + hash_bytes(side_hash) + hash_bytes(prev_main_hash))
