# bloub, ported

The character. Ported from **[jeremy-prt/bloub](https://github.com/jeremy-prt/bloub)**
(MIT, Copyright (c) 2026 Jeremy Perret - full text in `LICENSE` beside this
file). The screen design is ours; the character is not, and this folder is the
line between them.

Why port instead of drawing it again: every constant in bloub was measured
frame by frame off a reference video. The eyes are painted on a **sphere** and
projected orthographically, and that projection is what gives them volume - the
eye nearest the edge comes out narrower and leaning without anyone coding a
"lean". A flat two-ellipse face, which is what you get by redrawing it in a
screenshot tool, is a different character.

`bloub_face.c` is the projection and the idle life (blink schedule, gaze
drift). Shapes, decor and the state catalogue follow the same rule as they land.

Check: `scripts/tests/bloub_face_check.c` (plain C, no LVGL).
