#!/usr/bin/env python3
import os
import subprocess
import sys
from pathlib import Path

def get_mach_o_files(directory):
    """Finds all Mach-O files (executables and dylibs) in a directory."""
    mach_o_files = []
    for root, _, files in os.walk(directory):
        for file in files:
            path = Path(root) / file
            if path.is_symlink():
                continue
            # Use 'file' command to check if it's a Mach-O file
            try:
                output = subprocess.check_output(['file', str(path)], stderr=subprocess.STDOUT).decode('utf-8')
                if 'Mach-O' in output:
                    mach_o_files.append(path)
            except subprocess.CalledProcessError:
                continue
    return mach_o_files

def verify_signature(file_path):
    """Verifies the code signature and checks for entitlements in executables."""
    print(f"Verifying: {file_path}")
    try:
        # Check basic signature
        subprocess.check_output(['codesign', '-dv', str(file_path)], stderr=subprocess.STDOUT)
        
        # Check if it's an executable (not a dylib) to verify entitlements
        file_info = subprocess.check_output(['file', str(file_path)]).decode('utf-8')
        if 'executable' in file_info:
            print(f"  Checking entitlements for executable...")
            entitlements = subprocess.check_output(['codesign', '-dv', '--entitlements', '-', str(file_path)], 
                                                  stderr=subprocess.STDOUT).decode('utf-8')
            if 'com.apple.security.hypervisor' not in entitlements:
                print(f"FAILED: Missing hypervisor entitlement in {file_path}")
                return False
            if 'com.apple.security.cs.allow-jit' not in entitlements:
                print(f"FAILED: Missing JIT entitlement in {file_path}")
                return False
        return True
    except subprocess.CalledProcessError as e:
        print(f"FAILED: Signature verification failed for {file_path}")
        print(e.output.decode('utf-8') if e.output else "")
        return False

def main():
    # Base directory for the build objects
    base_dir = Path("objs/distribution-fishtank/fishtank")
    if not base_dir.exists():
        print(f"Error: Distribution directory {base_dir} not found. Run the build first.")
        sys.exit(1)

    mach_o_files = get_mach_o_files(base_dir)
    if not mach_o_files:
        print("Error: No Mach-O files found to verify.")
        sys.exit(1)

    print(f"Found {len(mach_o_files)} Mach-O files. Verifying signatures...")
    
    failed_files = []
    for file_path in mach_o_files:
        if not verify_signature(file_path):
            failed_files.append(file_path)

    if failed_files:
        print("\nVerification FAILED for the following files:")
        for f in failed_files:
            print(f"  - {f}")
        sys.exit(1)
    else:
        print("\nAll files are correctly signed with the required entitlements.")
        sys.exit(0)

if __name__ == "__main__":
    main()
