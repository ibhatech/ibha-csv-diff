#!/usr/bin/env python3
"""Check the Maven Central signing key is usable, before a release depends on it.

Phase J2 of docs/PUBLISHING-PLAN.md. Central verifies every artifact's signature
against a public keyserver, so a release fails late and confusingly if the key is
missing a capability, has expired, or was never actually published. Each of those
is cheap to check now and expensive to discover during a deploy.

**It never asks for, reads, or handles the passphrase.** Everything here is public:
key metadata from the local keyring, and whether the public half is retrievable
from the keyserver. Signing itself is left to Maven.

Run:
    python3 scripts-and-commands/check_signing_key.py
    python3 scripts-and-commands/check_signing_key.py --email you@example.com
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import time

DEFAULT_EMAIL = "manas@ibhatech.com"
KEYSERVER = "keyserver.ubuntu.com"


def run(args, timeout=60):
    return subprocess.run(args, capture_output=True, text=True, check=False, timeout=timeout)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--email", default=DEFAULT_EMAIL,
                    help="the identity the key must carry (default: the pom's developer)")
    ap.add_argument("--skip-keyserver", action="store_true",
                    help="local checks only, for a machine with no network")
    args = ap.parse_args()

    if shutil.which("gpg") is None:
        print("FAIL  gpg is not installed")
        return 1

    problems = 0

    # --- the key exists, with its capabilities ------------------------------
    out = run(["gpg", "--list-secret-keys", "--with-colons", "--keyid-format=long"])
    if out.returncode != 0 or "sec:" not in out.stdout:
        print("FAIL  no secret key in this keyring.")
        print("      Generate one:  gpg --pinentry-mode loopback --full-generate-key")
        return 1

    keyid = fpr = None
    algo = length = None
    expires = None
    caps = ""
    uids = []
    for line in out.stdout.splitlines():
        f = line.split(":")
        if f[0] == "sec" and keyid is None:
            algo, length, keyid, expires, caps = f[3], f[2], f[4], f[6], f[11]
        elif f[0] == "fpr" and fpr is None:
            fpr = f[9]
        elif f[0] == "uid":
            uids.append(f[9])

    print(f"key       {keyid}")
    print(f"fingerprint {fpr}")
    print(f"uids      {', '.join(uids) or '(none)'}")

    # --- it must be able to sign -------------------------------------------
    # Capability letters are lowercase on the key itself and uppercase for the
    # whole keypair including subkeys, which is what actually matters here.
    if "s" in caps.lower():
        print("ok    the key can sign")
    else:
        print(f"FAIL  the key cannot sign (capabilities: {caps or 'none'})")
        problems += 1

    # --- strength ----------------------------------------------------------
    ALGO = {"1": "RSA", "17": "DSA", "18": "ECDH", "19": "ECDSA", "22": "EdDSA"}
    name = ALGO.get(algo, f"algo {algo}")
    if algo == "1" and int(length or 0) >= 2048:
        print(f"ok    {name} {length}")
    elif algo == "22":
        print(f"warn  {name}: valid, but RSA is the more widely exercised choice "
              "for Central and the keyservers")
    else:
        print(f"warn  {name} {length}: check Central still accepts this")

    # --- expiry ------------------------------------------------------------
    if not expires:
        print("warn  the key never expires. Signatures stay valid either way, so an "
              "expiry costs nothing and lets a lost key retire itself")
    else:
        left = (int(expires) - time.time()) / 86400
        if left < 0:
            print(f"FAIL  the key expired {abs(left):.0f} days ago. Extend it with "
                  f"`gpg --edit-key {keyid}` then `expire`, and re-send it to the keyserver")
            problems += 1
        elif left < 30:
            print(f"warn  the key expires in {left:.0f} days. Extend it before releasing")
        else:
            print(f"ok    expires in {left:.0f} days")

    # --- identity ----------------------------------------------------------
    if any(args.email in u for u in uids):
        print(f"ok    carries {args.email}, which matches the pom's developer")
    else:
        print(f"warn  no uid carries {args.email}. Consumers see a signature by an "
              "identity that does not match the published developer block")

    # --- published where Central will look ---------------------------------
    if args.skip_keyserver:
        print("skip  keyserver check")
    elif not fpr:
        print("warn  no fingerprint, cannot check the keyserver")
    else:
        got = run(["gpg", "--keyserver", KEYSERVER, "--recv-keys", fpr], timeout=120)
        blob = got.stdout + got.stderr
        if got.returncode == 0 or "not changed" in blob or "imported" in blob:
            print(f"ok    the public half is on {KEYSERVER}")
        else:
            print(f"FAIL  {KEYSERVER} does not have this key yet.")
            print(f"      Publish it:  gpg --keyserver {KEYSERVER} --send-keys {fpr}")
            print("      Propagation can take minutes; a deploy before then fails "
                  "signature verification for this reason and no other.")
            problems += 1

    print()
    if problems:
        print(f"{problems} problem(s). A Central deploy would fail on these.")
        return 1
    print("the signing key looks ready. Back it up before releasing:")
    print(f"    gpg --armor --export-secret-keys {keyid} > key.asc   # then into a")
    print("    password manager, then shred the file")
    return 0


if __name__ == "__main__":
    sys.exit(main())
