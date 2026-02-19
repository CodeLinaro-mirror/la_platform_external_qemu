#!/usr/bin/env python3
import sys
from pathlib import Path

def setup():
    script_dir = Path(__file__).resolve().parent
    # Assuming source_root is 4 levels up from this script
    source_root = script_dir.parents[3]
    skills_src_dir = script_dir / "skills"
    gemini_skills_dir = source_root / ".gemini" / "skills"

    print("-" * 50)
    print("Setting up Android Emulator UI Agent Skills (Symlink mode)")
    print("-" * 50)

    if gemini_skills_dir.is_symlink():
        print(f"Removing existing symlink for skills directory: {gemini_skills_dir}")
        gemini_skills_dir.unlink()
    if not gemini_skills_dir.exists():
        print(f"Creating directory: {gemini_skills_dir}")
        gemini_skills_dir.mkdir(parents=True, exist_ok=True)

    # List of skills to link
    skills = ["aemu-ui-qt-expert"]

    for skill_name in skills:
        src = skills_src_dir / skill_name
        dst = gemini_skills_dir / skill_name

        if not src.exists():
            print(f"Warning: Skill source not found at {src}")
            continue

        if dst.is_symlink():
            print(f"Removing existing symlink: {dst}")
            dst.unlink()
        elif dst.exists():
            print(f"Error: {dst} exists and is not a symlink. Please remove it manually.")
            continue

        print(f"Linking {skill_name}...")
        try:
            # Create a relative symlink for better portability
            import os
            rel_src = os.path.relpath(src, dst.parent)
            dst.symlink_to(rel_src)
            print(f"Successfully linked {skill_name}")
        except OSError as e:
            print(f"Error creating symlink for {skill_name}: {e}")

    print("-" * 50)
    print("Setup complete.")
    print("The skills are now live. No reload required if already using them.")
    print("-" * 50)

if __name__ == "__main__":
    setup()
