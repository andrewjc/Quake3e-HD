#!/usr/bin/env python3
"""
Script to remove OpenGL code paths from the renderer.
Removes code blocks wrapped in #ifndef USE_VULKAN and keeps Vulkan code.
"""

import os
import re
import sys

def process_file(filepath):
    """Process a single file to remove OpenGL code."""
    
    with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
        lines = f.readlines()
    
    output_lines = []
    skip_depth = 0
    keep_depth = 0
    i = 0
    
    while i < len(lines):
        line = lines[i]
        
        # Check for preprocessor directives
        if line.strip().startswith('#'):
            stripped = line.strip()
            
            # Start of OpenGL-only block
            if '#ifndef USE_VULKAN' in stripped:
                skip_depth += 1
                i += 1
                continue
                
            # Start of Vulkan-only block
            elif '#ifdef USE_VULKAN' in stripped:
                keep_depth += 1
                # Don't output the ifdef line itself
                i += 1
                continue
                
            # Handle else clauses
            elif '#else' in stripped and (skip_depth > 0 or keep_depth > 0):
                if skip_depth > 0:
                    # We were skipping OpenGL, now keep Vulkan
                    skip_depth -= 1
                    keep_depth += 1
                elif keep_depth > 0:
                    # We were keeping Vulkan, now skip OpenGL
                    keep_depth -= 1
                    skip_depth += 1
                i += 1
                continue
                
            # Handle endif
            elif '#endif' in stripped and (skip_depth > 0 or keep_depth > 0):
                if skip_depth > 0:
                    skip_depth -= 1
                elif keep_depth > 0:
                    keep_depth -= 1
                i += 1
                continue
        
        # Output the line if we're not skipping
        if skip_depth == 0:
            output_lines.append(line)
        
        i += 1
    
    return output_lines

def clean_renderer_files(base_path):
    """Clean all renderer files."""
    
    renderer_path = os.path.join(base_path, 'src', 'engine', 'renderer')
    
    # Files to process
    files_to_process = []
    
    for root, dirs, files in os.walk(renderer_path):
        # Skip vulkan directory - it's already Vulkan-specific
        if 'vulkan' in root:
            continue
            
        for file in files:
            if file.endswith(('.c', '.h')):
                filepath = os.path.join(root, file)
                files_to_process.append(filepath)
    
    print(f"Found {len(files_to_process)} files to process")
    
    modified_count = 0
    for filepath in files_to_process:
        original_lines = []
        with open(filepath, 'r', encoding='utf-8', errors='ignore') as f:
            original_lines = f.readlines()
        
        processed_lines = process_file(filepath)
        
        # Only write if changed
        if processed_lines != original_lines:
            with open(filepath, 'w', encoding='utf-8') as f:
                f.writelines(processed_lines)
            print(f"Modified: {filepath}")
            modified_count += 1
    
    print(f"\nModified {modified_count} files")
    print("OpenGL code removal complete!")

if __name__ == "__main__":
    # Run from the root of the project
    base_path = r"F:\Development\Quake3e-HD"
    clean_renderer_files(base_path)