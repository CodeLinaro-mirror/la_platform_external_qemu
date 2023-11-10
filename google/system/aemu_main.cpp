/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2020 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

 // Note this file is almost a duplicate of system/main.c
 // The main difference is that this will initialize the
 // crashpad crash engine upon launch.
#include "android/crashreport/crash-initializer.h"

extern "C" {
void qemu_init(int argc, char **argv);
int qemu_main_loop(void);
void qemu_cleanup(int);
};

int qemu_default_main(void) {
  int status;

  status = qemu_main_loop();
  qemu_cleanup(status);

  return status;
}

int (*qemu_main)(void) = qemu_default_main;

int main(int argc, char **argv) {
  crashhandler_init(argc, argv);

  qemu_init(argc, argv);
  return qemu_main();
}
