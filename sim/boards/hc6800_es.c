/* hc6800_es.c - see hc6800_es.h
 * Copyright 2025 Thomas Reidemeister, MIT License (see ../devices/hc573.h)
 */
#include "hc6800_es.h"
#include "emu8051.h"

const pin_t HC6800_ES_P0[8] = {
    {REG_P0, 0}, {REG_P0, 1}, {REG_P0, 2}, {REG_P0, 3},
    {REG_P0, 4}, {REG_P0, 5}, {REG_P0, 6}, {REG_P0, 7}};

const hc573_pins_t HC6800_ES_SEG_LATCH = {
    .data = {{REG_P0, 0}, {REG_P0, 1}, {REG_P0, 2}, {REG_P0, 3},
             {REG_P0, 4}, {REG_P0, 5}, {REG_P0, 6}, {REG_P0, 7}},
    .le = {REG_P1, 0}};

const hc138_pins_t HC6800_ES_SEG_SELECT = {
    .a = {REG_P2, 2},
    .b = {REG_P2, 3},
    .c = {REG_P2, 4},
    .g1 = {0xFF, 0xFF},
    .g2a = {0xFF, 0xFF},
    .g2b = {0xFF, 0xFF}};

const hd44780_pins_t HC6800_ES_LCD = {
    .data = {{REG_P0, 0}, {REG_P0, 1}, {REG_P0, 2}, {REG_P0, 3},
             {REG_P0, 4}, {REG_P0, 5}, {REG_P0, 6}, {REG_P0, 7}},
    .rs = {REG_P2, 6},
    .rw = {REG_P2, 5},
    .e = {REG_P2, 7}};

const ds1302_pins_t HC6800_ES_DS1302 = {
    .ce = {REG_P3, 5},
    .sclk = {REG_P3, 6},
    .io = {REG_P3, 4}};

const xpt2046_pins_t HC6800_ES_XPT2046 = {
    .cs = {REG_P3, 5},
    .clk = {REG_P3, 6},
    .din = {REG_P3, 4},
    .dout = {REG_P3, 7}};
