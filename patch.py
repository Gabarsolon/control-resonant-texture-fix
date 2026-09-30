#!/usr/bin/env python3
"""
Control Resonant Texture Streaming Fix Patcher
Complete reverse-engineered fix for blurry textures in Remedy's Northlight Engine (Control Resonant).

Patches:
1. Tile Defrag: Trigger On Over Budget -> DISABLED (prevents aggressive mip tile dropping)
2. Tile Defrag: Trigger On Failed Allocation -> DISABLED (prevents panic defrag on allocation spikes)
3. Force max res textures -> ENABLED (instructs engine to prioritize maximum resolution mipmaps)
4. Fit to pool: Bias limit -> 0.0f (eliminates the +20.0 positive mip bias downscaling)
5. Tile Heap: Reserve -> 2048 MB (expands sparse tile heap from default 512 MB)
"""

import sys
import os
import shutil
import argparse

PATCHES = [
    {
        "name": "Tile Defrag: Trigger On Over Budget",
        "orig": bytes.fromhex("66 C7 05 21 A8 BC 05 01 01"),
        "patch": bytes.fromhex("66 C7 05 21 A8 BC 05 00 00"),
    },
    {
        "name": "Tile Defrag: Trigger On Failed Allocation",
        "orig": bytes.fromhex("66 C7 05 59 A8 BC 05 01 01"),
        "patch": bytes.fromhex("66 C7 05 59 A8 BC 05 00 00"),
    },
    {
        "name": "Force max res textures",
        "orig": bytes.fromhex("66 C7 05 D9 A8 A5 05 00 00"),
        "patch": bytes.fromhex("66 C7 05 D9 A8 A5 05 01 01"),
    },
    {
        "name": "Fit to pool: Bias limit (20.0f -> 0.0f)",
        "orig": bytes.fromhex("00 00 A0 41 00 00 20 41 00 00 80 41"),
        "patch": bytes.fromhex("00 00 00 00 00 00 20 41 00 00 80 41"),
    },
    {
        "name": "Tile Heap: Reserve (512 MB -> 2048 MB)",
        "orig": bytes.fromhex("00 02 00 00 00 00 00 00 00 64 00 00 00 02 00 00"),
        "patch": bytes.fromhex("00 08 00 00 00 00 00 00 00 64 00 00 00 08 00 00"),
    }
]

def find_target_exe(specified_path=None):
    if specified_path and os.path.isfile(specified_path):
        return specified_path
    
    candidates = [
        "CONTROLResonant.exe",
        os.path.join("..", "CONTROLResonant.exe"),
        r"E:\torrente\CONTROL.Resonant-InsaneRamZes\CONTROLResonant.exe"
    ]
    for c in candidates:
        if os.path.isfile(c):
            return os.path.abspath(c)
    return None

def patch_exe(exe_path, revert=False):
    print(f"Target executable: {exe_path}")
    
    bak_path = exe_path + ".bak"
    if not os.path.exists(bak_path):
        print(f"Creating backup: {bak_path}")
        shutil.copy2(exe_path, bak_path)
    else:
        print(f"Backup already exists: {bak_path}")
        
    with open(exe_path, "rb") as f:
        data = bytearray(f.read())
        
    all_ok = True
    if revert:
        print("\nReverting patches to vanilla defaults...")
        for p in PATCHES:
            idx = data.find(p["patch"])
            if idx != -1:
                data[idx:idx+len(p["patch"])] = p["orig"]
                print(f"[-] Reverted {p['name']} at offset 0x{idx:X}")
            elif data.find(p["orig"]) != -1:
                print(f"[*] {p['name']} is already in vanilla state.")
            else:
                print(f"[!] Could not locate patch signature for {p['name']}.")
                all_ok = False
        if all_ok:
            with open(exe_path, "wb") as f:
                f.write(data)
            print("\nSuccessfully reverted executable to vanilla.")
    else:
        print("\nApplying complete texture streaming fix suite...")
        for p in PATCHES:
            idx = data.find(p["orig"])
            if idx != -1:
                data[idx:idx+len(p["orig"])] = p["patch"]
                print(f"[+] Patched {p['name']} at offset 0x{idx:X}")
            elif data.find(p["patch"]) != -1:
                print(f"[*] {p['name']} is already patched.")
            else:
                print(f"[!] Could not locate signature for {p['name']}.")
                all_ok = False
        if all_ok:
            with open(exe_path, "wb") as f:
                f.write(data)
            print("\nSuccess! All 5 texture streaming patches applied cleanly.")
            print("Sparse tile heap expanded and defragmentation locks removed.")

def main():
    parser = argparse.ArgumentParser(description="Control Resonant Texture Streaming Fix Patcher")
    parser.add_argument("exe", nargs="?", help="Path to CONTROLResonant.exe", default=None)
    parser.add_argument("--revert", action="store_true", help="Revert patches to original vanilla defaults")
    args = parser.parse_args()
    
    target = find_target_exe(args.exe)
    if not target:
        print("Error: Could not find CONTROLResonant.exe. Please pass the full path as an argument.")
        sys.exit(1)
        
    patch_exe(target, revert=args.revert)

if __name__ == "__main__":
    main()
