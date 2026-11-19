/* ui_blocks.h - contract between ui_view.c (layout, scrolling, page chrome) and the files
 * that paint the individual page blocks. One file owns each block:
 *   ui_view.c       BLK_HERO, BLK_CONTROLS, BLK_STATS, BLK_EFFECTS, BLK_FOOT
 *   ui_view_time.c  BLK_TIMELINE, BLK_LEDGER (laid out under BLK_FOOT, only while open)
 *   ui_view_cal.c   BLK_CALENDAR, BLK_DAY
 *
 * blk_measure(which, w): height in px the block needs at width w (px). Must not draw.
 * blk_paint(which, x, y, w, h): draw into g_tgt at back-buffer coordinates (already
 *   scrolled) and register hit regions with hot_add() in the same coordinates. A block may
 *   be partly or wholly off-screen; clip sensibly and stay cheap.
 * view_content_y(screen_y): converts a back-buffer y to content (unscrolled) coordinates,
 *   for the EDIT-control rects in Ui (rc_title, rc_details, rc_notes).
 */
#ifndef IWD_UI_BLOCKS_H
#define IWD_UI_BLOCKS_H
#include "ui.h"

int  blk_time_measure(int which, int w);                    /* ui_view_time.c */
void blk_time_paint(int which, int x, int y, int w, int h);
void blk_time_free(void);                                   /* drop cached strips */

int  blk_cal_measure(int which, int w);                     /* ui_view_cal.c */
void blk_cal_paint(int which, int x, int y, int w, int h);
void blk_cal_free(void);

int  view_content_y(int screen_y);                          /* ui_view.c */
/* The viewed day's clock "HH:MM" from alm_minute_now() (never iwd_fmt_clock). ui_view.c */
void view_clock_text(WCHAR out[8]);

#endif
