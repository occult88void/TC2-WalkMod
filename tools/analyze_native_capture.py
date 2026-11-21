"""Resolve direct calls against captured bytes without treating gaps as zeroes."""
import argparse
import hashlib
import json
import re
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_IMM


def analyze(root):
    windows = {}
    conflicts = set()
    for path in sorted(root.glob('*.bin')):
        match = re.search(r'_rva_([0-9a-fA-F]{8})\.bin$', path.name)
        if not match:
            continue
        start = int(match[1], 16)
        data = path.read_bytes()
        previous = windows.get(start)
        if previous:
            common = min(len(data), len(previous['data']))
            if data[:common] != previous['data'][:common]:
                conflicts.add(start)
            if len(data) <= len(previous['data']):
                continue
        windows[start] = {'data': data, 'file': path.name}

    decoder = Cs(CS_ARCH_X86, CS_MODE_64)
    decoder.detail = True
    functions = []
    for start, window in sorted(windows.items()):
        if start in conflicts:
            continue
        instructions = []
        transfers = []
        for instruction in decoder.disasm(window['data'], start):
            if instruction.mnemonic == 'int3':
                break
            instructions.append({
                'rva': instruction.address,
                'bytes': instruction.bytes.hex(),
                'instruction': instruction.mnemonic + ' ' + instruction.op_str,
            })
            if instruction.mnemonic not in ('call', 'jmp'):
                continue
            operand = instruction.operands[0]
            transfer = {'rva': instruction.address, 'kind': instruction.mnemonic,
                        'operand': instruction.op_str}
            if operand.type == X86_OP_IMM:
                target = operand.imm
                covering = [
                    {'file': row['file'], 'window_rva': address,
                     'byte_offset': target - address}
                    for address, row in windows.items()
                    if address not in conflicts and address <= target < address + len(row['data'])
                ]
                transfer.update(target_rva=target, captured_windows=covering)
            transfers.append(transfer)
        functions.append({
            'window_rva': start, 'bytes': len(window['data']), 'file': window['file'],
            'sha256': hashlib.sha256(window['data']).hexdigest(),
            'instructions': instructions, 'transfers': transfers,
        })
    return {
        'format': 1,
        'address_unit': 'RVA, decimal',
        'interpretation': 'Linear disassembly from capture start. A window may start inside a function. '
                          'Captured target bytes alone do not establish a function boundary or ABI. '
                          'Check FACTORY_HASH_REFERENCE unwind ranges in the log for function starts.',
        'conflicting_window_rvas': sorted(conflicts),
        'windows': functions,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path, help='One capture directory from one game session')
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if not args.capture.is_dir():
        parser.error('Capture directory does not exist')
    result = analyze(args.capture)
    if not result['windows']:
        parser.error('No unambiguous RVA code windows were found')
    with args.output.open('x', encoding='utf-8') as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write('\n')
    print(f"Decoded {len(result['windows'])} windows, "
          f"excluded {len(result['conflicting_window_rvas'])} conflicting starts")


if __name__ == '__main__':
    main()
