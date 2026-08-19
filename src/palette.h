/* pid351 - DawnBringer 32, and the only place a colour is chosen
 *
 * Every colour in the interface comes from this file. Not because a palette
 * is precious, but because thirty-two named constants scattered through the
 * drawing code is how a UI ends up with four nearly-identical greys and no
 * one able to say which is the right one.
 *
 * DB32 is 8 bits a channel and the panel is RGB565, so some of these move:
 * #222034 lands on #202034, #9badb7 on #98ADB7. The largest error is one part
 * in thirty-two of blue, which is a quarter of a step on a 320 pixel panel
 * viewed at arm's length. Stated once here rather than rediscovered later.
 *
 * The DB_ names are the palette. The UI_ names are the decisions. Drawing
 * code uses UI_; if a screen wants a colour that has no UI_ name yet, the
 * answer is a new UI_ name and not a raw DB_ reference at the call site.
 */
#ifndef PALETTE_H
#define PALETTE_H

#include "pid351.h"

/* DawnBringer's 32, in his order. */
#define DB_BLACK      RGB565(0x00, 0x00, 0x00)
#define DB_VALHALLA   RGB565(0x22, 0x20, 0x34)
#define DB_LOULOU     RGB565(0x45, 0x28, 0x3c)
#define DB_OILED_CEDAR RGB565(0x66, 0x39, 0x31)
#define DB_ROPE       RGB565(0x8f, 0x56, 0x3b)
#define DB_TAHITI_GOLD RGB565(0xdf, 0x71, 0x26)
#define DB_TWINE      RGB565(0xd9, 0xa0, 0x66)
#define DB_PANCHO     RGB565(0xee, 0xc3, 0x9a)
#define DB_GOLDEN_FIZZ RGB565(0xfb, 0xf2, 0x36)
#define DB_ATLANTIS   RGB565(0x99, 0xe5, 0x50)
#define DB_CHRISTI    RGB565(0x6a, 0xbe, 0x30)
#define DB_ELF_GREEN  RGB565(0x37, 0x94, 0x6e)
#define DB_DELL       RGB565(0x4b, 0x69, 0x2f)
#define DB_VERDIGRIS  RGB565(0x52, 0x4b, 0x24)
#define DB_OPAL       RGB565(0x32, 0x3c, 0x39)
#define DB_DEEP_KOAMARU RGB565(0x3f, 0x3f, 0x74)
#define DB_VENICE_BLUE RGB565(0x30, 0x60, 0x82)
#define DB_ROYAL_BLUE RGB565(0x5b, 0x6e, 0xe1)
#define DB_CORNFLOWER RGB565(0x63, 0x9b, 0xff)
#define DB_VIKING     RGB565(0x5f, 0xcd, 0xe4)
#define DB_LIGHT_STEEL RGB565(0xcb, 0xdb, 0xfc)
#define DB_WHITE      RGB565(0xff, 0xff, 0xff)
#define DB_HEATHER    RGB565(0x9b, 0xad, 0xb7)
#define DB_TOPAZ      RGB565(0x84, 0x7e, 0x87)
#define DB_DIM_GRAY   RGB565(0x69, 0x6a, 0x6a)
#define DB_SMOKEY_ASH RGB565(0x59, 0x56, 0x52)
#define DB_CLAIRVOYANT RGB565(0x76, 0x42, 0x8a)
#define DB_BROWN      RGB565(0xac, 0x32, 0x32)
#define DB_MANDY      RGB565(0xd9, 0x57, 0x63)
#define DB_PLUM       RGB565(0xd7, 0x7b, 0xba)
#define DB_RAINFOREST RGB565(0x8f, 0x97, 0x4a)
#define DB_STINGER    RGB565(0x8a, 0x6f, 0x30)

/* The decisions.
 *
 * Ground is Valhalla rather than black: a true black next to a lit panel edge
 * reads as a dead pixel field, and DB32's darkest neutral is the colour the
 * rest of the palette was mixed against.
 *
 * The selected row is Venice Blue with Light Steel on it - the only pairing
 * in the palette that holds its contrast at both ends of the backlight range,
 * which matters here because the backlight is now a control. */
#define UI_GROUND     DB_VALHALLA
#define UI_PANEL      DB_OPAL
#define UI_EDGE       DB_SMOKEY_ASH
#define UI_TEXT       DB_HEATHER
#define UI_DIM        DB_DIM_GRAY
#define UI_BRIGHT     DB_LIGHT_STEEL
#define UI_ACCENT     DB_CHRISTI
#define UI_SEL_BAR    DB_VENICE_BLUE
#define UI_SEL_TEXT   DB_LIGHT_STEEL
#define UI_WARN       DB_MANDY
#define UI_GOOD       DB_ATLANTIS

/* The game sits on black and nothing else. Any colour in the pillarbox is a
 * colour the eye adapts to and then reads back out of the picture. */
#define UI_LETTERBOX  DB_BLACK

#endif /* PALETTE_H */
