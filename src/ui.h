/* pid351 - the interface: the ROM list, the brightness, the side column
 *
 * Everything the player sees that is not a game. It is one file because it is
 * one screen and two ornaments, and splitting three hundred lines across a
 * menu system, a widget layer and a theme is how a console ends up with more
 * frontend than emulator.
 *
 * What is deliberately not here: any notion of a settings page. There is one
 * adjustable thing on this machine, the backlight, and it is on a shoulder
 * button rather than behind a menu. Volume is a potentiometer on the side of
 * the case and software has nothing to say about it.
 *
 * ui_list owns a loop, which is unusual for this project - main.c owns the
 * loops - but the list's loop is the same shape as the game's and putting it
 * in main.c meant a second copy of the pacing, the input edges and the flip.
 */
#ifndef UI_H
#define UI_H

#include "pid351.h"

#define UI_MAX_ROMS 128

struct ui_rom {
    char path[512];      /* what core_open is given */
    char name[64];       /* what the player is shown */
    int  has_state;      /* a savestate exists, so A resumes rather than starts */
};

/* Everything the machine remembers between power cycles that is not a
 * savestate. Two integers, and see ui_state_save for why this is state and
 * not the configuration file CLAUDE.md forbids. */
struct ui_state {
    int cursor;          /* where the list was left */
    int bright;          /* backlight step, 0 .. UI_BRIGHT_STEPS - 1 */
};

#define UI_BRIGHT_STEPS 8

/* The card's mount point, or NULL when there is no card - in which case the
 * state is defaults and saving is a no-op rather than an error. */
void ui_state_load(const char *boot, struct ui_state *st);
void ui_state_save(const char *boot, const struct ui_state *st);

/* Applies a step to the panel. ui_bright_off is the shutdown path and does
 * not disturb the stored level. */
void ui_bright_apply(int step);
void ui_bright_off(void);

/* Every ROM in dir that a core will claim, sorted by display name. Returns
 * how many were found. */
int ui_scan(const char *dir, struct ui_rom *out, int max);

/* The list. Draws into fb at panel resolution, presents, and does not return
 * until the player chooses. The index of the chosen ROM, or -1 to power off.
 * st->cursor and st->bright are updated in place. */
int ui_list(px_t *fb, const struct ui_rom *roms, int n, struct ui_state *st);

/* The column beside the game. name may be NULL. Called once a frame from the
 * game loop, so it reads the gauge on its own schedule rather than per frame. */
void ui_bar(px_t *bar, const char *name, int bright_shown);

#endif /* UI_H */
