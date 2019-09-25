/*
 * Copyright 1998 by Albert Cahalan; all rights resered.
 * This file may be used subject to the terms and conditions of the
 * GNU Library General Public License Version 2, or any later version
 * at your option, as published by the Free Software Foundation.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Library General Public License for more details.
 */

/* return -1 on failure */
extern int signal_name_to_number(char *name);

extern int print_given_signals(int argc, char *argv[], int max_line);

extern void pretty_print_signals(void);

extern void unix_print_signals(void);
