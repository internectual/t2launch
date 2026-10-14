#!/usr/bin/env python3
"""Recover native Torque console registrations from a stripped ELF32 binary."""

from __future__ import annotations

import argparse
import re
import struct
import subprocess
from collections import Counter, defaultdict
from pathlib import Path

REGISTRATION_TYPES = {
    0x080E85C0: "S / string",
    0x080E8618: "V / void",
    0x080E8670: "I / integer",
    0x080E8720: "B / boolean",
}


def output(command):
    return subprocess.run(command, check=True, capture_output=True, text=True).stdout


class Elf32:
    def __init__(self, path):
        self.path = Path(path)
        self.data = self.path.read_bytes()
        if self.data[:4] != b"\x7fELF" or self.data[4] != 1 or self.data[5] != 1:
            raise ValueError("Expected a little-endian ELF32 executable")
        phoff = struct.unpack_from("<I", self.data, 28)[0]
        phentsize, phnum = struct.unpack_from("<HH", self.data, 42)
        self.loads = []
        self.executable_ranges = []
        for index in range(phnum):
            fields = struct.unpack_from("<IIIIIIII", self.data, phoff + index * phentsize)
            p_type, p_offset, p_vaddr, _paddr, p_filesz, p_memsz, p_flags, _align = fields
            if p_type == 1:
                self.loads.append((p_vaddr, p_vaddr + p_filesz, p_offset))
                if p_flags & 1:
                    self.executable_ranges.append((p_vaddr, p_vaddr + p_memsz))

    def bytes_at(self, address, limit=4096):
        for start, end, file_offset in self.loads:
            if start <= address < end:
                offset = file_offset + address - start
                return self.data[offset:min(offset + limit, file_offset + end - start)]
        return b""

    def cstring(self, address):
        raw = self.bytes_at(address)
        if not raw or b"\0" not in raw:
            return None
        raw = raw.split(b"\0", 1)[0]
        if not raw or any(byte < 0x20 and byte not in (9, 10, 13) for byte in raw):
            return None
        try:
            return raw.decode("ascii")
        except UnicodeDecodeError:
            return raw.decode("latin-1")

    def is_executable(self, address):
        return any(start <= address < end for start, end in self.executable_ranges)


def parse_disassembly(binary):
    disassembly = output(["objdump", "-d", "-M", "intel", "--no-show-raw-insn", str(binary)])
    instructions = []
    by_address = {}
    for line in disassembly.splitlines():
        match = re.match(r"\s*([0-9a-fA-F]+):\s+(.+)$", line)
        if match:
            address = int(match.group(1), 16)
            text = match.group(2).strip()
            instructions.append((address, text))
            by_address[address] = text
    return instructions, by_address


def parse_plt(binary):
    text = output(["objdump", "-d", "-j", ".plt", "-M", "intel", "--no-show-raw-insn", str(binary)])
    symbols = {}
    for line in text.splitlines():
        match = re.match(r"\s*([0-9a-fA-F]+)\s+<([^>]+)@plt>:", line)
        if match:
            symbols[int(match.group(1), 16)] = match.group(2)
    return symbols


def callback_cfg(start, instructions, by_address, positions, elf, callback_addresses):
    """Recover instructions reachable from a callback entry using branch flow."""
    pending = [start]
    visited = set()
    while pending and len(visited) < 2048:
        address = pending.pop()
        if address in visited:
            continue
        index = positions.get(address)
        if index is None:
            continue
        visited.add(address)
        asm = instructions[index][1]
        mnemonic, _, operands = asm.partition(" ")
        mnemonic = mnemonic.lower()
        next_address = instructions[index + 1][0] if index + 1 < len(instructions) else None
        target_match = re.match(r"(?:0x)?([0-9a-fA-F]+)", operands.strip())
        target = int(target_match.group(1), 16) if target_match else None

        if mnemonic.startswith(("ret", "iret")):
            continue
        if mnemonic == "jmp":
            # Follow local control flow, but don't mistake distant tail calls for
            # part of this callback's implementation.
            if (target is not None and elf.is_executable(target)
                    and abs(target - start) <= 0x4000
                    and target not in callback_addresses - {start}):
                pending.append(target)
            continue
        if mnemonic.startswith("j") or mnemonic.startswith("loop"):
            if target is not None and elf.is_executable(target):
                pending.append(target)
            if next_address is not None:
                pending.append(next_address)
            continue
        if next_address is not None:
            pending.append(next_address)

    return [(address, by_address[address]) for address in sorted(visited) if address in by_address]


def immediate_push(instructions, index):
    """Read a push operand, resolving simple immediate-loaded registers."""
    text = instructions[index][1]
    match = re.match(r"push\s+(?:0x([0-9a-fA-F]+)|(-?\d+))\s*$", text)
    if match:
        return int(match.group(1), 16) if match.group(1) else int(match.group(2), 10)
    register = re.fullmatch(r"push\s+(e(?:ax|bx|cx|dx|si|di|bp|sp))", text)
    if not register:
        return None
    reg = register.group(1)
    for previous in range(index - 1, max(-1, index - 35), -1):
        asm = instructions[previous][1]
        if re.match(rf"(?:mov|lea)\s+{reg}\s*,", asm):
            source = asm.split(",", 1)[1].strip()
            imm = re.fullmatch(r"0x([0-9a-fA-F]+)", source)
            if imm:
                return int(imm.group(1), 16)
            # `lea reg, [absolute_address]` appears in some initializer thunks.
            lea = re.fullmatch(r"\[0x([0-9a-fA-F]+)\]", source)
            if lea:
                return int(lea.group(1), 16)
            return None
        if re.match(r"(?:call|ret|jmp)\b", asm):
            return None
    return None


def decode_calls(instructions, elf):
    results = []
    failed = []
    for index, (site, text) in enumerate(instructions):
        call = re.match(r"call\s+(?:0x)?([0-9a-fA-F]+)", text)
        if not call:
            continue
        target = int(call.group(1), 16)
        if target not in REGISTRATION_TYPES:
            continue
        pushes = []
        cursor = index - 1
        while cursor >= 0 and len(pushes) < 6:
            asm = instructions[cursor][1]
            if re.match(r"(?:call|ret|jmp)\b", asm):
                break
            if re.match(r"push\b", asm):
                pushes.append(immediate_push(instructions, cursor))
            cursor -= 1
        if len(pushes) != 6 or any(value is None for value in pushes):
            failed.append((site, REGISTRATION_TYPES[target], text, pushes))
            continue
        maximum, minimum, usage_ptr, callback, name_ptr, namespace_ptr = reversed(pushes)
        record = {
            "call_site": site,
            "registration": target,
            "return_type": REGISTRATION_TYPES[target],
            "namespace_ptr": namespace_ptr,
            "name_ptr": name_ptr,
            "callback": callback,
            "usage_ptr": usage_ptr,
            "minimum": minimum,
            "maximum": maximum,
            "namespace": elf.cstring(namespace_ptr),
            "name": elf.cstring(name_ptr),
            "usage": elf.cstring(usage_ptr),
            "callback_executable": elf.is_executable(callback),
        }
        if not record["namespace"] or not record["name"] or record["usage"] is None:
            failed.append((site, REGISTRATION_TYPES[target], text, pushes))
        else:
            # Torque method callbacks receive a command token and an object
            # token before user-supplied parameters; global commands receive
            # only the command token. Keep raw constructor bounds as well.
            implicit = 2 if record["namespace"] else 1
            record["explicit_minimum"] = max(0, minimum - implicit)
            record["explicit_maximum"] = max(0, maximum - implicit)
            record["implicit_prefix_args"] = implicit
            results.append(record)
    return results, failed


def summarize_callback(body, elf, plt_symbols):
    calls = []
    literals = []
    for _address, text in body:
        call = re.fullmatch(r"call\s+(?:(?:0x)?([0-9a-fA-F]{6,8})(?:\s+<([^>]+)>)?|(\w+))", text)
        if call and call.group(1):
            target = int(call.group(1), 16)
            label = plt_symbols.get(target, "")
            if label:
                label = "PLT " + label
            else:
                label = "internal"
            calls.append((target, label))
        elif call:
            calls.append((None, "indirect via " + call.group(3)))
        for pointer_text in re.findall(r"0x([0-9a-fA-F]{6,8})", text):
            value = int(pointer_text, 16)
            literal = elf.cstring(value)
            if literal and literal not in literals:
                literals.append(literal)

    call_labels = []
    for target, label in calls:
        item = (f"{target:#010x} " if target is not None else "") + f"({label})"
        if item not in call_labels:
            call_labels.append(item)
    summary = [f"{len(body)} instructions; {len(calls)} call(s)."]
    if call_labels:
        summary.append("Call targets: " + ", ".join(call_labels[:12]) + (" …" if len(call_labels) > 12 else "."))
    if literals:
        summary.append("Embedded string references: " + " | ".join(literals[:12]) + (" …" if len(literals) > 12 else "."))
    if not call_labels and not literals:
        summary.append("No direct call labels or ASCII literals recovered; see disassembly for low-level behavior.")
    return " ".join(summary), body


def read_decompilation(path):
    if not path:
        return {}
    sections = {}
    current = None
    body = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").splitlines():
        marker = re.match(r"@@ CALLBACK (0x[0-9a-fA-F]+)", line)
        if marker:
            if current is not None:
                sections[current] = body
            current = int(marker.group(1), 16)
            body = []
        else:
            body.append(line)
    if current is not None:
        sections[current] = body
    return sections


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path.home() / "t2-linux/tribes2.dynamic")
    parser.add_argument("--output", type=Path, default=Path.home() / "t2-linux/console_commands.log")
    parser.add_argument("--addresses-output", type=Path)
    parser.add_argument("--decomp", type=Path, help="Ghidra DumpConsoleCallbacks output, if available")
    args = parser.parse_args()

    elf = Elf32(args.binary)
    instructions, by_address = parse_disassembly(args.binary)
    plt_symbols = parse_plt(args.binary)
    records, failed = decode_calls(instructions, elf)
    decompilation = read_decompilation(args.decomp)

    callback_addresses = sorted({record["callback"] for record in records if record["callback_executable"]})
    ghidra_function_count = sum(
        any(line.startswith("// Ghidra function:") for line in decompilation.get(address, []))
        for address in callback_addresses
    )
    positions = {address: index for index, (address, _text) in enumerate(instructions)}
    callback_set = set(callback_addresses)
    callback_info = {}
    for address in callback_addresses:
        body = callback_cfg(address, instructions, by_address, positions, elf, callback_set)
        if body:
            summary, body = summarize_callback(body, elf, plt_symbols)
            callback_info[address] = ((body[0][0], body[-1][0]), summary, body)
        else:
            callback_info[address] = (None, "No reachable disassembly recovered at callback pointer.", [])

    unique_records = {}
    for record in records:
        key = (record["namespace"], record["name"], record["return_type"], record["callback"], record["usage"], record["minimum"], record["maximum"])
        unique_records.setdefault(key, {**record, "call_sites": []})["call_sites"].append(record["call_site"])
    sorted_records = sorted(unique_records.values(), key=lambda item: (item["namespace"].casefold(), item["name"].casefold(), item["return_type"]))
    counts = Counter(record["return_type"] for record in records)

    lines = [
        "TRIBES 2 NATIVE CONSOLE COMMAND REPORT",
        "========================================",
        f"Executable: {args.binary}",
        "Format: ELF32 i386; analysis is static and version-specific to this executable.",
        "This is a static call-site inventory, not a runtime trace; conditional or late registration paths may not execute on every launch.",
        "",
        "Registration API",
        "-----------------",
        "All four registration overloads use six cdecl arguments in this order:",
        "  namespace pointer, command-name pointer, engine callback pointer, ASCII usage pointer, minimum args, maximum args.",
        "The overload classifies the command return type: S=string, V=void, I=integer, B=boolean.",
        "Callback bodies are recovered by control-flow traversal from each entry. Ghidra C output is heuristic, especially for types in this stripped ELF; the assembly remains the machine-code evidence.",
        "",
        "Static scan results",
        "-------------------",
        f"Registration call sites found: {sum(counts.values()) + len(failed)}",
        f"Fully decoded call sites: {len(records)}",
        f"Unique registration records: {len(sorted_records)}",
        f"Unique executable callbacks: {len(callback_addresses)}",
        f"Ghidra functions decompiled: {ghidra_function_count} / {len(callback_addresses)} callbacks",
        f"Undecoded/invalid call sites: {len(failed)}",
    ]
    for kind in ("S / string", "V / void", "I / integer", "B / boolean"):
        lines.append(f"  {kind}: {counts[kind]} registration(s)")

    lines.extend(["", "Command index", "-------------"])
    for number, record in enumerate(sorted_records, 1):
        callsites = ", ".join(f"{site:#010x}" for site in sorted(record["call_sites"]))
        lines.append(
            f"{number:04d}. {record['namespace']}::{record['name']} [{record['return_type']}] "
            f"raw argc={record['minimum']}..{record['maximum']} "
            f"explicit args≈{record['explicit_minimum']}..{record['explicit_maximum']} "
            f"callback={record['callback']:#010x} "
            f"string ptrs(ns/name/usage)={record['namespace_ptr']:#010x}/{record['name_ptr']:#010x}/{record['usage_ptr']:#010x} "
            f"usage={record['usage']!r} callsite(s)={callsites}"
        )

    lines.extend(["", "Callback analysis and disassembly", "----------------------------------"])
    for number, record in enumerate(sorted_records, 1):
        address = record["callback"]
        lines.extend([
            "",
            f"[{number:04d}] {record['namespace']}::{record['name']} ({record['return_type']})",
            f"Usage: {record['usage']}",
            f"Raw argc bounds: {record['minimum']}..{record['maximum']}",
            f"Estimated explicit argument bounds: {record['explicit_minimum']}..{record['explicit_maximum']} "
            f"(subtracting {record['implicit_prefix_args']} implicit argument(s); usage text is the authoritative human-readable form)",
            f"Registration overload: {record['registration']:#010x}; registration call site(s): "
            + ", ".join(f"{site:#010x}" for site in sorted(record["call_sites"])),
            f"Resolved metadata pointers: namespace={record['namespace_ptr']:#010x} ({record['namespace']!r}), "
            f"name={record['name_ptr']:#010x} ({record['name']!r}), "
            f"usage={record['usage_ptr']:#010x}",
            f"Callback: {address:#010x}",
        ])
        if not record["callback_executable"]:
            lines.append("Behavior: callback pointer is outside the executable PT_LOAD range; likely an external/shared-library function.")
            continue
        bounds, summary, body = callback_info[address]
        if bounds:
            lines.append(f"CFG-reachable instruction span: {bounds[0]:#010x}..{bounds[1]:#010x} (not necessarily contiguous)")
        lines.append(f"Behavior summary (static heuristic): {summary}")
        if address in decompilation:
            lines.extend(["Ghidra decompiler output (inspect critically):", "```c"])
            lines.extend(decompilation[address])
            lines.append("```")
        if body:
            lines.append("Disassembly:")
            lines.extend(f"  {pc:08x}: {asm}" for pc, asm in body)
        else:
            lines.append("Disassembly unavailable: function boundary could not be determined confidently.")

    if failed:
        lines.extend([
            "", "Dynamic-forwarding registration call sites", "-------------------------------------------",
            "These direct calls do not expose six immediate constructor arguments at the call site; they forward values dynamically and are not counted as decoded static command definitions.",
        ])
        for site, kind, instruction, pushes in failed:
            lines.append(f"{site:#010x} {kind}: {instruction}; recovered push values={pushes!r}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    if args.addresses_output:
        args.addresses_output.write_text("".join(f"{address:#010x}\n" for address in callback_addresses), encoding="ascii")
    print(f"Wrote {args.output}: {len(sorted_records)} registrations, {len(callback_addresses)} callbacks, {len(failed)} unresolved sites")


if __name__ == "__main__":
    main()
