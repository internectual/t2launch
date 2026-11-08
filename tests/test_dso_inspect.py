import struct
import unittest

from dso_inspect import DsoFormatError, parse_dso


def pack_u32(value):
    return struct.pack("<I", value)


def make_dso(
    *, version=140, global_strings=b"", global_floats=(),
    function_strings=b"", function_floats=(), tokens=(),
    line_break_pairs=(), identifier_fixups=(),
):
    parts = [pack_u32(version), pack_u32(len(global_strings)), global_strings]
    parts.append(pack_u32(len(global_floats)))
    parts.extend(struct.pack("<d", value) for value in global_floats)
    parts.extend((pack_u32(len(function_strings)), function_strings))
    parts.append(pack_u32(len(function_floats)))
    parts.extend(struct.pack("<d", value) for value in function_floats)
    parts.extend((pack_u32(len(tokens)), pack_u32(len(line_break_pairs))))
    for token in tokens:
        if token <= 0xFE:
            parts.append(bytes((token,)))
        else:
            parts.extend((b"\xff", pack_u32(token)))
    for first, second in line_break_pairs:
        parts.extend((pack_u32(first), pack_u32(second)))
    parts.append(pack_u32(len(identifier_fixups)))
    for string_offset, code_positions in identifier_fixups:
        parts.extend((pack_u32(string_offset), pack_u32(len(code_positions))))
        parts.extend(pack_u32(position) for position in code_positions)
    return b"".join(parts)


class DsoInspectTests(unittest.TestCase):
    def test_parses_old_assignment_sample_structure(self):
        image = parse_dso(make_dso(
            global_strings=b"$ClientInstalledVersion\0",
            global_floats=(1.51,),
            tokens=(0x42, 0, 0x25, 0, 0x2C, 0x3D, 0x0D),
            identifier_fixups=((0, (3,)),),
        ))

        self.assertEqual(image.version, 140)
        self.assertEqual(image.global_strings, b"$ClientInstalledVersion\0")
        self.assertEqual(image.global_floats, (1.51,))
        self.assertEqual(image.code_tokens, (0x42, 0, 0x25, 0, 0x2C, 0x3D, 0x0D))
        self.assertEqual(image.identifier_fixups[0].code_positions, (3,))

    def test_parses_extended_tokens_and_line_break_pairs(self):
        image = parse_dso(make_dso(
            tokens=(0xFF01, 0x42),
            line_break_pairs=((1, 2),),
            identifier_fixups=((4, (0, 1)),),
        ))

        self.assertEqual(image.code_tokens, (0xFF01, 0x42))
        self.assertEqual(image.line_break_pairs, ((1, 2),))
        self.assertEqual(image.identifier_fixups[0].string_offset, 4)

    def test_rejects_truncated_extended_token(self):
        data = b"".join((
            pack_u32(140), pack_u32(0), pack_u32(0), pack_u32(0), pack_u32(0),
            pack_u32(1), pack_u32(0), b"\xff",
        ))
        with self.assertRaises(DsoFormatError):
            parse_dso(data)

    def test_rejects_identifier_site_outside_code(self):
        data = make_dso(tokens=(0x42,), identifier_fixups=((0, (1,)),))
        with self.assertRaisesRegex(DsoFormatError, "outside the code token stream"):
            parse_dso(data)

    def test_rejects_trailing_bytes(self):
        with self.assertRaisesRegex(DsoFormatError, "trailing DSO bytes"):
            parse_dso(make_dso() + b"x")


if __name__ == "__main__":
    unittest.main()
