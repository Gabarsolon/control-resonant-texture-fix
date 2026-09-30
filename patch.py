#!/usr/bin/env python3
"""
Control Resonant Texture Streaming Fix Patcher
Disables aggressive over-budget mipmap purging and forces maximum resolution textures
in Remedy's Northlight Engine (Control Resonant).
"""

import sys
import os
import shutil
import argparse

DEFRAG_ORIGINAL = bytes.fromhex("66 C7 05 21 A8 BC 05 01 01")
DEFRAG_PATCHED  = bytes.fromhex("66 C7 05 21 A8 BC 05 00 00")

FORCE_RES_ORIGINAL = bytes.fromhex("66 C7 05 D9 A8 A5 05 00 00")
FORCE_RES_PATCHED  = bytes.fromhex("66 C7 05 D9 A8 A5 05 01 01")

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
        
    if revert:
        print("\nReverting patches to vanilla defaults...")
        defrag_found = False
        force_res_found = False
        
        idx = data.find(DEFRAG_PATCHED)
        if idx != -1:
            data[idx:idx+len(DEFRAG_PATCHED)] = DEFRAG_ORIGINAL
            print(f"[-] Reverted Tile Defrag to enabled at offset 0x{idx:X}")
            defrag_found = True
        elif data.find(DEFRAG_ORIGINAL) != -1:
            print("[*] Tile Defrag is already in vanilla state.")
            defrag_found = True

        idx2 = data.find(FORCE_RES_PATCHED)
        if idx2 != -1:
            data[idx2:idx2+len(FORCE_RES_PATCHED)] = FORCE_RES_ORIGINAL
            print(f"[-] Reverted Force max res textures to disabled at offset 0x{idx2:X}")
            force_res_found = True
        elif data.find(FORCE_RES_ORIGINAL) != -1:
            print("[*] Force max res textures is already in vanilla state.")
            force_res_found = True

        if defrag_found or force_res_found:
            with open(exe_path, "wb") as f:
                f.write(data)
            print("\nSuccessfully reverted executable to vanilla.")
        else:
            print("\nFailed to find patch locations.")
            
    else:
        print("\nApplying texture streaming fixes...")
        defrag_patched = False
        force_res_patched = False
        
        idx = data.find(DEFRAG_ORIGINAL)
        if idx != -1:
            data[idx:idx+len(DEFRAG_ORIGINAL)] = DEFRAG_PATCHED
            print(f"[+] Patched Tile Defrag:Trigger On Over Budget -> DISABLED at offset 0x{idx:X}")
            defrag_patched = True
        elif data.find(DEFRAG_PATCHED) != -1:
            print("[*] Tile Defrag:Trigger On Over Budget is already patched (DISABLED).")
            defrag_patched = True
        else:
            print("[!] Could not locate Tile Defrag pattern.")

        idx2 = data.find(FORCE_RES_ORIGINAL)
        if idx2 != -1:
            data[idx2:idx2+len(FORCE_RES_ORIGINAL)] = FORCE_RES_PATCHED
            print(f"[+] Patched Force max res textures -> ENABLED at offset 0x{idx2:X}")
            force_res_patched = True
        elif data.find(FORCE_RES_PATCHED) != -1:
            print("[*] Force max res textures is already patched (ENABLED).")
            force_res_patched = True
        else:
            print("[!] Could not locate Force max res pattern.")

        if defrag_patched and force_res_patched:
            with open(exe_path, "wb") as f:
                f.write(data)
            print("\nSuccess! CONTROLResonant.exe patched successfully.")
            print("Textures will no longer drop to low resolution during VRAM spikes.")
        else:
            print("\nError: Failed to patch one or more locations.")

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
