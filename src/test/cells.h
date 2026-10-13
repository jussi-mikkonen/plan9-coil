/*
 * cells.h: rune width function shared by the view and editor test drivers.
 * arg points to an int mode: 0 = ascii (every rune 1 cell), 1 = eaw (East Asian wide ranges are 2 cells).
 */
enum { Wascii, Weaw };

static int
tcells(void *arg, Rune r)
{
	int mode;

	mode = *(int*)arg;
	if(mode != Weaw)
		return 1;
	if((r >= 0x1100 && r <= 0x115F) || (r >= 0x2E80 && r <= 0xA4CF) ||
	   (r >= 0xAC00 && r <= 0xD7A3) || (r >= 0xF900 && r <= 0xFAFF) ||
	   (r >= 0xFE30 && r <= 0xFE4F) || (r >= 0xFF00 && r <= 0xFF60) ||
	   (r >= 0xFFE0 && r <= 0xFFE6) || (r >= 0x20000 && r <= 0x3FFFD))
		return 2;
	return 1;
}
