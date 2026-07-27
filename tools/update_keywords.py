#!/usr/bin/env python3
"""Keyword update script for binar language.

Changes:
1. continue -> pass (3 test files)
2. Remove volatile from asm (3 stdlib files)
3. iface -> port (7 test files)
"""

import re
import os

TESTS_DIR = os.path.join(os.path.dirname(__file__), '..', 'tests')
STDLIB_DIR = os.path.join(os.path.dirname(__file__), '..', 'std')

def update_file(path, changes):
    with open(path, 'r') as f:
        content = f.read()
    for old, new in changes:
        content = content.replace(old, new)
    with open(path, 'w') as f:
        f.write(content)
    print(f"  Updated: {path}")

def main():
    print("=== Keyword Update Script ===\n")

    # 1. continue -> pass (3 test files)
    print("1. Renaming continue -> pass:")
    continue_files = [
        os.path.join(TESTS_DIR, '11_continue.binar'),
        os.path.join(TESTS_DIR, '40_continue_sum.binar'),
        os.path.join(TESTS_DIR, '70_continue_count.binar'),
    ]
    for f in continue_files:
        if os.path.exists(f):
            update_file(f, [('continue', 'pass')])
        else:
            print(f"  WARNING: {f} not found")

    # 2. Remove volatile from asm (3 stdlib files)
    print("\n2. Removing volatile from asm:")
    volatile_files = [
        os.path.join(STDLIB_DIR, 'fmt', 'fmt.binar'),
        os.path.join(STDLIB_DIR, 'os', 'os.binar'),
    ]
    for f in volatile_files:
        if os.path.exists(f):
            update_file(f, [('asm volatile(', 'asm(')])
        else:
            print(f"  WARNING: {f} not found")

    # 3. iface -> port (7 test files)
    print("\n3. Renaming iface -> port:")
    iface_files = [
        os.path.join(TESTS_DIR, '103_iface_basic.binar'),
        os.path.join(TESTS_DIR, '104_iface_multi_impl.binar'),
        os.path.join(TESTS_DIR, '105_iface_method_call.binar'),
        os.path.join(TESTS_DIR, '106_iface_pointer_receiver.binar'),
        os.path.join(TESTS_DIR, '107_iface_multi_method.binar'),
        os.path.join(TESTS_DIR, '108_iface_multi_type.binar'),
        os.path.join(TESTS_DIR, '109_iface_chained_mono.binar'),
    ]
    for f in iface_files:
        if os.path.exists(f):
            update_file(f, [('iface ', 'port ')])
        else:
            print(f"  WARNING: {f} not found")

    print("\n=== Done ===")
    print("Run 'cd build && cmake .. && make' to rebuild.")
    print("Run './tests/run_tests.sh' to verify.")

if __name__ == '__main__':
    main()
