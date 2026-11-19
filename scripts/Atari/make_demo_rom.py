#!/usr/bin/env python3
"""Assemble t2demo.bin: a tiny 4K homebrew for the Atari 2600 server gametype (written for this project, so it can
be shipped). A diamond sprite moves with the joystick, fire cycles its colour, and there is a playfield border and a
rainbow-coloured playfield bars on a black background. Run: python3 make_demo_rom.py [output.bin]"""
import sys

# 6502 encodings this program needs: mnemonic -> {mode: opcode}
OPS = {
    "sei": {"imp": 0x78}, "cld": {"imp": 0xD8}, "txa": {"imp": 0x8A}, "dex": {"imp": 0xCA}, "txs": {"imp": 0x9A},
    "pha": {"imp": 0x48}, "sec": {"imp": 0x38}, "tay": {"imp": 0xA8}, "rts": {"imp": 0x60}, "asl": {"imp": 0x0A},
    "ldx": {"imm": 0xA2}, "lda": {"imm": 0xA9, "zp": 0xA5, "abs": 0xAD, "absy": 0xB9},
    "sta": {"zp": 0x85, "zpx": 0x95, "abs": 0x8D}, "stx": {"zp": 0x86},
    "and": {"imm": 0x29}, "cmp": {"imm": 0xC9}, "eor": {"imm": 0x49},
    "sbc": {"imm": 0xE9, "zp": 0xE5}, "inc": {"zp": 0xE6}, "dec": {"zp": 0xC6},
    "bne": {"rel": 0xD0}, "bcs": {"rel": 0xB0}, "bcc": {"rel": 0x90}, "bmi": {"rel": 0x30},
    "jmp": {"abs": 0x4C}, "jsr": {"abs": 0x20},
}
ORG = 0x1000
SYM = {"VSYNC": 0x00, "VBLANK": 0x01, "WSYNC": 0x02, "INPT4": 0x0C, "COLUP0": 0x06, "COLUPF": 0x08,
       "COLUBK": 0x09, "PF0": 0x0D, "RESP0": 0x10, "GRP0": 0x1B, "HMP0": 0x20, "HMOVE": 0x2A, "SWCHA": 0x280,
       "XPOS": 0x80, "YPOS": 0x81, "COL": 0x82}

PROGRAM = """
Start:  sei
        cld
        ldx #0
        txa
Clear:  dex
        txs
        pha
        bne Clear
        lda #70
        sta XPOS
        lda #96
        sta YPOS
        lda #$1E
        sta COL
Frame:  lda #2
        sta VSYNC
        sta WSYNC
        sta WSYNC
        sta WSYNC
        lda #0
        sta VSYNC
        lda #2
        sta VBLANK
        ldx #33
VB:     sta WSYNC
        dex
        bne VB
        lda SWCHA
        and #$80
        bne NoR
        lda XPOS
        cmp #140
        bcs NoR
        inc XPOS
NoR:    lda SWCHA
        and #$40
        bne NoL
        lda XPOS
        cmp #6
        bcc NoL
        dec XPOS
NoL:    lda SWCHA
        and #$10
        bne NoU
        lda YPOS
        cmp #180
        bcs NoU
        inc YPOS
NoU:    lda SWCHA
        and #$20
        bne NoD
        lda YPOS
        cmp #12
        bcc NoD
        dec YPOS
NoD:    lda INPT4
        bmi NoFire
        inc COL
        inc COL
NoFire: lda COL
        sta COLUP0
        lda #$F0
        sta PF0
        lda #$44
        sta COLUPF
        lda XPOS
        ldx #0
        jsr PosObj
        sta WSYNC
        sta HMOVE
        lda #0
        sta VBLANK
        ldx #192
Line:   sta WSYNC
        txa
        sec
        sbc YPOS
        cmp #8
        bcs NoSpr
        tay
        lda Sprite,y
        jmp Store
NoSpr:  lda #0
Store:  sta GRP0
        stx COLUPF
        dex
        bne Line
        lda #2
        sta VBLANK
        ldx #30
OS:     sta WSYNC
        dex
        bne OS
        jmp Frame
PosObj: sec
        sta WSYNC
Div:    sbc #15
        bcs Div
        eor #7
        asl
        asl
        asl
        asl
        sta HMP0,x
        sta RESP0,x
        rts
Sprite: .byte $18,$3C,$7E,$FF,$FF,$7E,$3C,$18
"""


def value(token, labels):
    token = token.strip()
    if token.startswith("$"):
        return int(token[1:], 16)
    if token.isdigit():
        return int(token)
    if token in SYM:
        return SYM[token]
    return labels[token]


def assemble(text):
    lines = [l.split(";")[0].rstrip() for l in text.splitlines()]
    labels = {}
    for final in (False, True):
        out = bytearray()
        for raw in lines:
            if not raw.strip():
                continue
            line = raw
            if ":" in line.split()[0] if line.split() else False:
                name, line = line.split(":", 1)
                labels[name.strip()] = ORG + len(out)
            line = line.strip()
            if not line:
                continue
            if line.startswith(".byte"):
                for tok in line[5:].split(","):
                    out.append(value(tok, labels))
                continue
            parts = line.split(None, 1)
            mnem = parts[0].lower()
            operand = parts[1].strip() if len(parts) > 1 else ""
            modes = OPS[mnem]
            if not operand:
                out.append(modes["imp"])
            elif operand.startswith("#"):
                out += bytes([modes["imm"], value(operand[1:], labels) & 0xFF])
            elif "rel" in modes:
                target = labels.get(operand, ORG + len(out)) if not final else labels[operand]
                offset = target - (ORG + len(out) + 2)
                if final and not -128 <= offset <= 127:
                    raise SystemExit(f"branch to {operand} out of range")
                out += bytes([modes["rel"], offset & 0xFF])
            elif operand.lower().endswith(",x"):
                v = value(operand[:-2], labels) if final or operand[:-2] in SYM else 0
                out += bytes([modes["zpx"], v & 0xFF])
            elif operand.lower().endswith(",y"):
                v = value(operand[:-2], labels) if final or operand[:-2] in SYM else 0x1000
                out += bytes([modes["absy"], v & 0xFF, (v >> 8) & 0xFF])
            else:
                v = value(operand, labels) if final or operand in SYM or operand.startswith("$") else 0x1000
                if "zp" in modes and v < 0x100 and "abs" not in modes or ("zp" in modes and v < 0x100):
                    out += bytes([modes["zp"], v])
                else:
                    out += bytes([modes["abs"], v & 0xFF, (v >> 8) & 0xFF])
    return bytes(out)


def main():
    code = assemble(PROGRAM)
    rom = bytearray([0xEA] * 4096)
    rom[:len(code)] = code
    rom[0xFFC:0xFFE] = bytes([ORG & 0xFF, ORG >> 8])
    rom[0xFFE:0x1000] = bytes([ORG & 0xFF, ORG >> 8])
    path = sys.argv[1] if len(sys.argv) > 1 else "t2demo.bin"
    open(path, "wb").write(rom)
    print(f"{path}: {len(code)} bytes of code in a 4096-byte ROM")


if __name__ == "__main__":
    main()
