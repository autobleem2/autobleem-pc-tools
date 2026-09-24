#!/usr/bin/env python3
"""Draws apps/lastresort/src/pcb_fastboot.png: the corner of the PlayStation Classic's board (LM-11, side A)
where the two FASTBOOT pads are, with the pads marked - the picture LastResortRecovery shows on its
"put the console in fastboot mode" page (640x270, the band the other pages fill with the AutoBleem picture).

A drawing, not a photo: the silkscreen text that tells the user they are looking at the right side of the
right board, the two chips beside it, and the two round pads just right of "LM-11", above "SIDE A".
Drawn at 3x and scaled down for smooth edges. Needs Pillow and a TTF font (--font; the launcher's Open Sans
from a sibling AutoBleem checkout by default, else DejaVu Sans).

Usage: python tools/make_pcb_diagram.py [--font BOLD.ttf] [--out apps/lastresort/src/pcb_fastboot.png]
"""
import argparse
import os
from PIL import Image, ImageDraw, ImageFont

W, H = 640, 270
S = 3  # supersampling

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
FONT_CANDIDATES = [
    os.path.join(REPO, '..', 'AutoBleem2', 'src', 'resources', 'fonts', 'OpenSans-Bold.ttf'),
    os.path.join(REPO, '..', 'autobleem', 'src', 'resources', 'fonts', 'OpenSans-Bold.ttf'),
    '/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf',
    'C:/Windows/Fonts/arialbd.ttf',
]

BACK = (22, 26, 32)
BOARD = (18, 110, 62)
BOARD_EDGE = (12, 84, 46)
TRACE = (28, 128, 74)
SILK = (236, 232, 214)
COPPER = (214, 150, 72)
PAD = (206, 210, 214)
PAD_DARK = (120, 126, 132)
CHIP = (24, 24, 26)
CHIP_TEXT = (120, 120, 124)
RED = (232, 36, 36)
NOTE_BACK = (10, 14, 20, 225)
NOTE_TEXT = (240, 244, 250)


def s(v):
    return int(round(v * S))


def font(path, size):
    return ImageFont.truetype(path, s(size))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--font', default=None)
    ap.add_argument('--out', default=os.path.join(REPO, 'apps', 'lastresort', 'src', 'pcb_fastboot.png'))
    args = ap.parse_args()
    font_path = args.font or next((f for f in FONT_CANDIDATES if os.path.exists(f)), None)
    if not font_path:
        raise SystemExit('no font found - pass --font')

    img = Image.new('RGBA', (s(W), s(H)), BACK + (255,))
    d = ImageDraw.Draw(img, 'RGBA')

    # the board: its top edge across the picture, its right edge with the rounded corner
    d.rounded_rectangle([s(-40), s(22), s(612), s(H + 40)], radius=s(22), fill=BOARD, outline=BOARD_EDGE,
                        width=s(3))
    # a few traces for texture
    for y in (150, 158, 166):
        d.line([s(0), s(y), s(130), s(y), s(170), s(y + 40), s(210), s(y + 40)], fill=TRACE, width=s(2))
    for x in (330, 338, 346):
        d.line([s(x), s(212), s(x), s(240), s(x + 30), s(262)], fill=TRACE, width=s(2))

    # the chips: memory at the left edge, the eMMC in the middle
    d.rectangle([s(18), s(60), s(128), s(142)], fill=CHIP)
    d.text((s(30), s(90)), 'RAM', font=font(font_path, 13), fill=CHIP_TEXT)
    d.rectangle([s(205), s(118), s(300), s(208)], fill=CHIP)
    d.text((s(226), s(152)), 'eMMC', font=font(font_path, 13), fill=CHIP_TEXT)

    # the silkscreen that says which board and which side
    d.text((s(160), s(40)), 'LM-11', font=font(font_path, 30), fill=SILK)
    d.ellipse([s(272), s(46), s(298), s(72)], outline=SILK, width=s(2))
    d.text((s(280), s(47)), '1', font=font(font_path, 16), fill=SILK)
    d.text((s(196), s(80)), '1-984-020-11', font=font(font_path, 17), fill=SILK)
    d.text((s(470), s(64)), 'SIDE A', font=font(font_path, 15), fill=SILK)
    d.text((s(360), s(150)), 'Sony Interactive', font=font(font_path, 15), fill=SILK)
    d.text((s(360), s(170)), 'Entertainment Inc.', font=font(font_path, 15), fill=SILK)

    # the four small test points right of the pads, and a mounting hole at the edge
    for x in (478, 488, 498, 508):
        d.ellipse([s(x - 2.5), s(49.5), s(x + 2.5), s(54.5)], fill=PAD)
    d.ellipse([s(566), s(84), s(598), s(116)], fill=COPPER)
    d.ellipse([s(574), s(92), s(590), s(108)], fill=BACK)

    # the two FASTBOOT pads, and the mark around them
    for cx in (402, 434):
        d.ellipse([s(cx - 11), s(57 - 11), s(cx + 11), s(57 + 11)], fill=PAD, outline=PAD_DARK, width=s(2))
        d.ellipse([s(cx - 4), s(57 - 4), s(cx + 4), s(57 + 4)], fill=PAD_DARK)
    d.rounded_rectangle([s(384), s(38), s(452), s(76)], radius=s(6), outline=RED, width=s(4))

    # the note, with an arrow up to the mark
    d.line([s(418), s(80), s(418), s(214)], fill=RED, width=s(4))
    d.polygon([(s(418), s(78)), (s(410), s(94)), (s(426), s(94))], fill=RED)
    d.rounded_rectangle([s(14), s(214), s(626), s(260)], radius=s(8), fill=NOTE_BACK, outline=RED, width=s(2))
    d.text((s(28), s(219)), 'FASTBOOT pads', font=font(font_path, 16), fill=RED + (255,))
    d.text((s(28), s(238)), 'Hold these two together while you connect the console to this PC',
           font=font(font_path, 13), fill=NOTE_TEXT)

    out = img.resize((W, H), Image.LANCZOS).convert('RGB')
    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    out.save(args.out, optimize=True)
    print(f'{args.out}: {W}x{H}')


if __name__ == '__main__':
    main()
