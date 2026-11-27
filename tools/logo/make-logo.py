import math
import pathlib
import sys

SLOPE = 0.48
BAND_WIDTH = 80.0
VALLEY_Y = 162.0
CORNER = 24.0
VALLEYS = (122.0, 278.0)
VIEW_BOX = (-2, 0, 404, 202)
GLOW_MARGIN = 60
GLOW_BLUR = 20

PALETTES = {
    "logo": {
        "g1": ((20, 0, 130, 202), ((0, "#78F4FC"), (0.3, "#2BB8FD"), (0.62, "#0880FD"), (1, "#22C0FD"))),
        "g2": ((130, 190, 200, 10), ((0, "#004CD2"), (1, "#0032A2"))),
        "g3": ((200, 0, 280, 202), ((0, "#0054F4"), (0.62, "#0074FD"), (1, "#10B2FD"))),
        "g4": ((285, 170, 395, 0), ((0, "#0042C4"), (0.4, "#0066EA"), (0.72, "#1DA6FC"), (1, "#7AEFFD"))),
    },
    "logo-alt": {
        "g1": ((20, 0, 130, 202), ((0, "#A6F7FF"), (0.3, "#22D3EE"), (0.62, "#2F6BFF"), (1, "#5A8CFF"))),
        "g2": ((130, 190, 200, 10), ((0, "#2440D8"), (1, "#1A2A9C"))),
        "g3": ((200, 0, 280, 202), ((0, "#3048FF"), (0.62, "#0A84FF"), (1, "#22D3EE"))),
        "g4": ((285, 170, 395, 0), ((0, "#2238C8"), (0.4, "#1F5BF0"), (0.72, "#22C4F0"), (1, "#B0F8FF"))),
    },
}


def number(value):
    return f"{value:.2f}".rstrip("0").rstrip(".")


def band(valley_x, direction):
    norm = math.sqrt(1 + SLOPE * SLOPE)
    half = BAND_WIDTH * norm / 2
    radius = BAND_WIDTH / 2
    top_x = valley_x - SLOPE * VALLEY_Y
    edge_x, edge_y = SLOPE * CORNER / norm, CORNER / norm
    points = [
        (top_x - half + CORNER, 0),
        (top_x + half - CORNER, 0),
        (top_x + half, 0),
        (top_x + half + edge_x, edge_y),
        (valley_x + radius / norm, VALLEY_Y - radius * SLOPE / norm),
        (valley_x - radius / norm, VALLEY_Y + radius * SLOPE / norm),
        (top_x - half + edge_x, edge_y),
        (top_x - half, 0),
        (top_x - half + CORNER, 0),
    ]
    p = [(valley_x + direction * (x - valley_x), y) for x, y in points]
    sweep = 1 if direction > 0 else 0
    return (
        f"M{number(p[0][0])} 0L{number(p[1][0])} 0Q{number(p[2][0])} 0 {number(p[3][0])} {number(p[3][1])}"
        f"L{number(p[4][0])} {number(p[4][1])}A{number(radius)} {number(radius)} 0 0 {sweep} {number(p[5][0])} {number(p[5][1])}"
        f"L{number(p[6][0])} {number(p[6][1])}Q{number(p[7][0])} 0 {number(p[8][0])} 0Z"
    )


def outline(valley_x, direction, corner_steps=6, arc_steps=24):
    norm = math.sqrt(1 + SLOPE * SLOPE)
    half = BAND_WIDTH * norm / 2
    radius = BAND_WIDTH / 2
    top_x = valley_x - SLOPE * VALLEY_Y
    edge_x, edge_y = SLOPE * CORNER / norm, CORNER / norm

    def quad(a, control, b):
        return [
            (
                (1 - s) ** 2 * a[0] + 2 * (1 - s) * s * control[0] + s * s * b[0],
                (1 - s) ** 2 * a[1] + 2 * (1 - s) * s * control[1] + s * s * b[1],
            )
            for s in (i / corner_steps for i in range(1, corner_steps + 1))
        ]

    points = [(top_x - half + CORNER, 0.0), (top_x + half - CORNER, 0.0)]
    points += quad(points[-1], (top_x + half, 0.0), (top_x + half + edge_x, edge_y))
    start = math.atan2(-SLOPE, 1)
    for i in range(arc_steps + 1):
        angle = start + math.pi * i / arc_steps
        points.append((valley_x + radius * math.cos(angle), VALLEY_Y + radius * math.sin(angle)))
    points.append((top_x - half + edge_x, edge_y))
    points += quad(points[-1], (top_x - half, 0.0), (top_x - half + CORNER, 0.0))[:-1]
    return [(valley_x + direction * (x - valley_x), y) for x, y in points]


OUTLINES = (outline(VALLEYS[0], -1), outline(VALLEYS[1], -1), outline(VALLEYS[1], 1), outline(VALLEYS[0], 1))

BANDS = (
    ("g2", band(VALLEYS[0], -1)),
    ("g4", band(VALLEYS[1], -1)),
    ("g3", band(VALLEYS[1], 1)),
    ("g1", band(VALLEYS[0], 1)),
)


def gradients(palette):
    out = []
    for name, ((x1, y1, x2, y2), stops) in PALETTES[palette].items():
        stop_tags = "".join(f'<stop offset="{offset}" stop-color="{color}"/>' for offset, color in stops)
        out.append(
            f'<linearGradient id="{name}" gradientUnits="userSpaceOnUse" x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}">{stop_tags}</linearGradient>'
        )
    return "\n".join(out)


def paths(attributes=""):
    return "\n".join(f'<path d="{d}" fill="url(#{gradient})"{attributes}/>' for gradient, d in BANDS)


def logo(palette):
    x, y, width, height = VIEW_BOX
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x} {y} {width} {height}" width="{width}" height="{height}">\n'
        f"<defs>\n{gradients(palette)}\n</defs>\n{paths()}\n</svg>\n"
    )


def glow():
    x, y, width, height = VIEW_BOX
    m = GLOW_MARGIN
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x - m} {y - m} {width + 2 * m} {height + 2 * m}" '
        f'width="{width + 2 * m}" height="{height + 2 * m}">\n'
        f'<defs>\n{gradients("logo")}\n'
        f'<filter id="blur" x="-50%" y="-50%" width="200%" height="200%"><feGaussianBlur stdDeviation="{GLOW_BLUR}"/></filter>\n'
        f'</defs>\n<g filter="url(#blur)">\n{paths()}\n</g>\n</svg>\n'
    )


def header():
    x, y, width, height = VIEW_BOX
    lines = [
        f"#define LOGO_VIEW_X {x}",
        f"#define LOGO_VIEW_WIDTH {width}",
        f"#define LOGO_VIEW_HEIGHT {height}",
        f"#define LOGO_GLOW_BLUR {GLOW_BLUR}",
        f"#define LOGO_BANDS {len(OUTLINES)}",
        "",
        "static const float logo_points[] = {",
    ]
    offsets = [0]
    for points in OUTLINES:
        lines.append("\t" + " ".join(f"{px:.3f}f, {py:.3f}f," for px, py in points))
        offsets.append(offsets[-1] + len(points))
    lines.append("};")
    lines.append("")
    lines.append("static const int logo_offsets[] = { " + ", ".join(str(o) for o in offsets) + " };")
    lines.append("")
    lines.append("static const struct logo_gradient logo_gradients[2][LOGO_BANDS] = {")
    for palette in ("logo", "logo-alt"):
        lines.append("\t{")
        for gradient, _ in BANDS:
            (x1, y1, x2, y2), stops = PALETTES[palette][gradient]
            offsets_text = ", ".join(f"{float(offset)}f" for offset, _ in stops)
            colors_text = ", ".join(f"0x{color[1:]}" for _, color in stops)
            lines.append(
                f"\t\t{{ {x1}, {y1}, {x2}, {y2}, {len(stops)}, {{ {offsets_text} }}, {{ {colors_text} }} }},"
            )
        lines.append("\t},")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main():
    target = pathlib.Path(sys.argv[1])
    target.mkdir(parents=True, exist_ok=True)
    (target / "logo.svg").write_text(logo("logo"))
    (target / "logo-alt.svg").write_text(logo("logo-alt"))
    (target / "logo-glow.svg").write_text(glow())
    if len(sys.argv) > 2:
        pathlib.Path(sys.argv[2]).write_text(header())


if __name__ == "__main__":
    main()
