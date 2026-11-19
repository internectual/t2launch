"""Write MatrixDonkey.mis: the Matrix mission plus the Donkey Kong boards as real game models.

Models are placed in the mission file (TSStatic) because shape datablocks defined by a server script make stock clients
drop about 12 seconds after joining; mission-file shapes need no datablock. Rotations here are in degrees.
"""

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TEMPLATE = ROOT / "missions" / "MatrixInvaders.mis"
OUTPUT = ROOT / "missions" / "MatrixDonkey.mis"

Z0 = 300.0
SEAT_X = [0, 40, -40, 80]               # board centres (Donkey.cs SeatOffset * Spacing)
GIRDERS = [(-15, 15), (-15, 11), (-11, 15), (-15, 11), (-11, 15), (-15, 11), (-6, 2)]
GAP = 4.6
V0 = 1.0
THICK = 0.6
LADDERS = [(0, -9), (0, 4), (1, -6), (1, 8), (2, -7), (2, 4), (3, -4), (3, 9), (4, -8), (4, 6), (5, -1)]
LADDER_Y = 1.6
BARRELS = 7
PARK = (0, -300, 316)       # behind the camera: in scope but out of sight


def obj(name, shape, pos, rot, scale):
    return (
        f'\tnew TSStatic({name}) {{\n\t\tshapeName = "{shape}";\n\t\tposition = "{pos[0]:.3f} {pos[1]:.3f} {pos[2]:.3f}";\n'
        f'\t\trotation = "{rot}";\n\t\tscale = "{scale[0]:.4f} {scale[1]:.4f} {scale[2]:.4f}";\n\t}};\n'
    )


def main() -> None:
    text = TEMPLATE.read_text()
    text = text.replace("Matrix Invaders", "Matrix Donkey").replace(
        "Space Invaders, one board per player", "Donkey Kong, one board per player built from game models"
    )
    parts = []
    for seat, bx in enumerate(SEAT_X):
        for k, (x0, x1) in enumerate(GIRDERS):
            sv = V0 + k * GAP
            # a cargo container (5.1 x 2.1 x 2.05, origin at the bottom centre) stretched out and flattened
            parts.append(obj(f"DkG_{seat}_{k}", "stackable1l.dts", (bx + (x0 + x1) / 2, 0, Z0 + sv - THICK), "1 0 0 0",
                             ((x1 - x0) / 5.1, 1, THICK / 2.05)))
        for j, (k, u) in enumerate(LADDERS):
            z0 = Z0 + V0 + k * GAP
            h = GAP - THICK
            for side in (-0.55, 0.55):
                # a pole is 21 tall and 1.5 across
                parts.append(obj(f"DkP_{seat}_{j}_{int(side > 0)}", "silver_pole.dts", (bx + u + side, LADDER_Y, z0), "1 0 0 0",
                                 (0.1, 0.1, h / 21)))
            for r in (1, 2, 3):
                # a rung is a pole lying on its side; two halves turned opposite ways meet in the middle
                for d, deg in ((0, 90), (1, -90)):
                    parts.append(obj(f"DkR_{seat}_{j}_{r}_{d}", "silver_pole.dts", (bx + u, LADDER_Y, z0 + r * h / 4),
                                     f"0 1 0 {deg}", (0.07, 0.07, 0.55 / 21)))
        parts.append(obj(f"DkD_{seat}", "stackable2m.dts", (bx - 13.2, 0, Z0 + V0), "1 0 0 0", (1.4, 1.4, 1.5)))
    end = text.rindex("};")
    OUTPUT.write_text(text[:end] + "".join(parts) + text[end:])


if __name__ == "__main__":
    main()
