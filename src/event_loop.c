/* vifm
 * Copyright (C) 2001 Ken Steen.
 * Copyright (C) 2011 xaizek.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "event_loop.h"

#include <curses.h>
#include <unistd.h>

#include <assert.h> /* assert() */
#include <signal.h> /* signal() */
#include <stddef.h> /* NULL size_t wchar_t */
#include <stdlib.h> /* free() */
#include <string.h> /* memmove() strncpy() */
#include <wchar.h> /* wint_t wcslen() wcscmp() */

#include "cfg/config.h"
#include "compat/curses.h"
#include "engine/completion.h"
#include "engine/keys.h"
#include "engine/mode.h"
#include "modes/dialogs/msg_dialog.h"
#include "modes/modes.h"
#include "modes/wk.h"
#include "ui/color_manager.h"
#include "ui/fileview.h"
#include "ui/statusbar.h"
#include "ui/statusline.h"
#include "ui/ui.h"
#include "utils/log.h"
#include "utils/macros.h"
#include "utils/test_helpers.h"
#include "utils/utf8.h"
#include "utils/utils.h"
#include "background.h"
#include "bracket_notation.h"
#include "filelist.h"
#include "ipc.h"
#include "registers.h"
#include "status.h"
#include "vifm.h"

static int ensure_term_is_ready(void);
static int get_char_async_loop(WINDOW *win, wint_t *c, int timeout);
static void process_scheduled_updates(void);
TSTATIC int process_scheduled_updates_of_view(view_t *view);
static void update_hardware_cursor(void);
static int should_check_views_for_changes(void);
static void check_view_for_changes(view_t *view);
static void reset_input_buf(wchar_t curr_input_buf[],
		size_t *curr_input_buf_pos);
static void display_suggestion_box(const wchar_t input[]);
static void process_suggestion(const wchar_t lhs[], const wchar_t rhs[],
		const char descr[]);
static void draw_suggestion_box(void);
static WINDOW * prepare_suggestion_box(int *height);
static void hide_suggestion_box(void);
static int should_display_suggestion_box(void);

/* Current input buffer. */
static const wchar_t *curr_input_buf;
/* Current position in current input buffer. */
static const size_t *curr_input_buf_pos;

/* Whether suggestion box is active. */
static int suggestions_are_visible;

void
event_loop(const int *quit)
{
	/* TODO: refactor this function event_loop(). */

	LOG_FUNC_ENTER;

	const wchar_t *const prev_input_buf = curr_input_buf;
	const size_t *const prev_input_buf_pos = curr_input_buf_pos;

	wchar_t input_buf[128];
	size_t input_buf_pos;

	int last_result = 0;
	int wait_for_enter = 0;
	int wait_for_suggestion = 0;
	int timeout = cfg.timeout_len;

	input_buf[0] = L'\0';
	input_buf_pos = 0;
	curr_input_buf = &input_buf[0];
	curr_input_buf_pos = &input_buf_pos;

	while(!*quit)
	{
		wint_t c;
		size_t counter;
		int got_input;

		lwin.user_selection = 1;
		rwin.user_selection = 1;

		modes_pre();

		/* Waits for timeout then skips if no key press.  Short-circuit if we're not
		 * waiting for the next key after timeout. */
		do
		{
			const int actual_timeout = wait_for_suggestion
			                         ? MIN(timeout, cfg.sug.delay)
			                         : timeout;

			if(!ensure_term_is_ready())
			{
				wait_for_enter = 0;
				continue;
			}

			modes_periodic();

			bg_check();

			got_input = (get_char_async_loop(status_bar, &c, actual_timeout) != ERR);

			/* If suggestion delay timed out, reset it and wait the rest of the
			 * timeout. */
			if(!got_input && wait_for_suggestion)
			{
				wait_for_suggestion = 0;
				timeout -= actual_timeout;
				display_suggestion_box(input_buf);
				continue;
			}
			wait_for_suggestion = 0;

			if(!got_input && (input_buf_pos == 0 || last_result == KEYS_WAIT))
			{
				timeout = cfg.timeout_len;
				continue;
			}

			if(got_input && c == K(KEY_RESIZE))
			{
				modes_redraw();
				continue;
			}

			break;
		}
		while(1);

		suggestions_are_visible = 0;

		/* Ensure that current working directory is set correctly (some pieces of
		 * code rely on this, e.g. %c macro in current directory). */
		(void)vifm_chdir(flist_get_dir(curr_view));

		if(got_input)
		{
			if(wait_for_enter)
			{
				wait_for_enter = 0;
				curr_stats.save_msg = 0;
				ui_sb_clear();
				if(c == WC_CR)
				{
					continue;
				}
			}

			if(c == WC_C_z)
			{
				ui_shutdown();
				stop_process();
				continue;
			}

			if(input_buf_pos < ARRAY_LEN(input_buf) - 2)
			{
				input_buf[input_buf_pos++] = c;
				input_buf[input_buf_pos] = L'\0';
			}
			else
			{
				/* Recover from input buffer overflow by resetting its contents. */
				reset_input_buf(input_buf, &input_buf_pos);
				clear_input_bar();
				continue;
			}
		}

		counter = vle_keys_counter();
		if(!got_input && last_result == KEYS_WAIT_SHORT)
		{
			hide_suggestion_box();

			last_result = vle_keys_exec_timed_out(input_buf);
			counter = vle_keys_counter() - counter;
			assert(counter <= input_buf_pos);
			if(counter > 0)
			{
				memmove(input_buf, input_buf + counter,
						(wcslen(input_buf) - counter + 1)*sizeof(wchar_t));
			}
		}
		else
		{
			if(got_input)
			{
				curr_stats.save_msg = 0;
			}

			if(last_result == KEYS_WAIT || last_result == KEYS_WAIT_SHORT)
			{
				hide_suggestion_box();
			}

			last_result = vle_keys_exec(input_buf);

			counter = vle_keys_counter() - counter;
			assert(counter <= input_buf_pos);
			if(counter > 0)
			{
				input_buf_pos -= counter;
				memmove(input_buf, input_buf + counter,
						(wcslen(input_buf) - counter + 1)*sizeof(wchar_t));
			}

			if(last_result == KEYS_WAIT || last_result == KEYS_WAIT_SHORT)
			{
				if(should_display_suggestion_box())
				{
					wait_for_suggestion = 1;
				}

				if(got_input)
				{
					modupd_input_bar(input_buf);
				}

				if(last_result == KEYS_WAIT_SHORT && wcscmp(input_buf, L"\033") == 0)
				{
					timeout = 1;
				}

				if(counter > 0)
				{
					clear_input_bar();
				}

				if(!curr_stats.save_msg && curr_view->selected_files &&
						!vle_mode_is(CMDLINE_MODE))
				{
					print_selected_msg();
				}
				continue;
			}
		}

		timeout = cfg.timeout_len;

		process_scheduled_updates();

		reset_input_buf(input_buf, &input_buf_pos);
		clear_input_bar();

		if(ui_sb_multiline())
		{
			wait_for_enter = 1;
			update_all_windows();
			continue;
		}

		/* Ensure that current working directory is set correctly (some pieces of
		 * code rely on this).  PWD could be changed during command execution, but
		 * it should be correct for modes_post() in case of preview modes. */
		(void)vifm_chdir(flist_get_dir(curr_view));
		modes_post();
	}

	curr_input_buf = prev_input_buf;
	curr_input_buf_pos = prev_input_buf_pos;
}

/* Ensures that terminal is in proper state for a loop iteration.  Returns
 * non-zero if so, otherwise zero is returned. */
static int
ensure_term_is_ready(void)
{
	ui_update_term_state();

	update_terminal_settings();

	if(curr_stats.term_state == TS_TOO_SMALL)
	{
		ui_display_too_small_term_msg();
		wait_for_signal();
		return 0;
	}

	if(curr_stats.term_state == TS_BACK_TO_NORMAL)
	{
		wint_t c;

		wtimeout(status_bar, 0);
		while(compat_wget_wch(status_bar, &c) != ERR);
		curr_stats.term_state = TS_NORMAL;
		modes_redraw();
		wtimeout(status_bar, cfg.timeout_len);

		curr_stats.save_msg = 0;
		ui_sb_msg("");
	}

	return 1;
}

/* Sub-loop of the main loop that "asynchronously" queries for the input
 * performing the following tasks while waiting for input:
 *  - checks for new IPC messages;
 *  - checks whether contents of displayed directories changed;
 *  - redraws UI if requested.
 * Returns KEY_CODE_YES for functional keys (preprocesses *c in this case), OK
 * for wide character and ERR otherwise (e.g. after timeout). */
static int
get_char_async_loop(WINDOW *win, wint_t *c, int timeout)
{
	const int IPC_F = ipc_enabled() ? 10 : 1;

	do
	{
		int i;

		int delay_slice = DIV_ROUND_UP(MIN(cfg.min_timeout_len, timeout), IPC_F);
#ifdef __PDCURSES__
		/* pdcurses performs delays in 50 ms intervals (1/20 of a second). */
		delay_slice = MAX(50, delay_slice);
#endif

		if(should_check_views_for_changes())
		{
			check_view_for_changes(curr_view);
			check_view_for_changes(other_view);
		}

		process_scheduled_updates();

		for(i = 0; i < IPC_F && timeout > 0; ++i)
		{
			int result;

			ipc_check(curr_stats.ipc);
			wtimeout(win, delay_slice);
			timeout -= delay_slice;

			if(suggestions_are_visible)
			{
				/* Redraw suggestion box as it might have been hidden due to other
				 * redraws. */
				display_suggestion_box(curr_input_buf);
			}

			/* Update cursor before waiting for input.  Modes set cursor correctly
			 * within corresponding windows, but we need to call refresh on one of
			 * them to make it active. */
			update_hardware_cursor();

			result = compat_wget_wch(win, c);
			if(result != ERR)
			{
				if(result == KEY_CODE_YES)
				{
					*c = K(*c);
				}
				return result;
			}

			process_scheduled_updates();
		}
	}
	while(timeout > 0);

	return ERR;
}

/* Updates TUI or its elements if something is scheduled. */
static void
process_scheduled_updates(void)
{
	int need_redraw = 0;

	ui_stat_job_bar_check_for_updates();

	if(vle_mode_is(CMDLINE_MODE))
	{
		/* Redrawing implicitly updates hardware cursor, so temporary disable it. */
		curs_set(0);
	}

	if(vle_mode_get_primary() != MENU_MODE)
	{
		need_redraw += (process_scheduled_updates_of_view(curr_view) != 0);
		need_redraw += (process_scheduled_updates_of_view(other_view) != 0);
	}

	need_redraw += (stats_redraw_fetch() != 0);

	if(need_redraw)
	{
		modes_redraw();
	}

	if(vle_mode_is(CMDLINE_MODE))
	{
		curs_set(1);
	}
}

/* Performs postponed updates for the view, if any.  Returns non-zero if
 * something was indeed updated, and zero otherwise. */
TSTATIC int
process_scheduled_updates_of_view(view_t *view)
{
	if(!window_shows_dirlist(view))
	{
		return 0;
	}

	switch(ui_view_query_scheduled_event(view))
	{
		case UUE_NONE:
			/* Nothing to do. */
			return 0;
		case UUE_REDRAW:
			redraw_view(view);
			return 1;
		case UUE_RELOAD:
			load_saving_pos(view);
			return 1;
	}

	assert(0 && "Unexpected type of scheduled UI event.");
	return 0;
}

/* Updates hardware cursor to be on currently active area of the interface,
 * which depends mainly on current mode.. */
static void
update_hardware_cursor(void)
{
	if(ui_sb_multiline())
	{
		checked_wmove(status_bar, 0, 0);
		wrefresh(status_bar);
		return;
	}

	switch(vle_mode_get())
	{
		case MENU_MODE:
		case FILE_INFO_MODE:
		case MORE_MODE: wrefresh(menu_win);       break;
		case CHANGE_MODE:
		case ATTR_MODE: wrefresh(change_win);     break;
		case MSG_MODE:  wrefresh(error_win);      break;
		case VIEW_MODE: wrefresh(curr_view->win); break;
		case SORT_MODE: wrefresh(sort_win);       break;

		case NORMAL_MODE:
		case VISUAL_MODE:
			if(should_check_views_for_changes())
			{
				wrefresh(curr_view->win);
			}
			break;
	}
}

/* Checks whether views should be checked against external changes.  Returns
 * non-zero is so, otherwise zero is returned. */
static int
should_check_views_for_changes(void)
{
	return !ui_sb_multiline()
	    && !is_in_menu_like_mode()
	    && NONE(vle_mode_is, CMDLINE_MODE, MSG_MODE);
}

/* Updates view in case directory it displays was changed externally. */
static void
check_view_for_changes(view_t *view)
{
	if(window_shows_dirlist(view))
	{
		check_if_filelist_has_changed(view);
	}
}

void
update_input_buf(void)
{
	if(curr_stats.use_input_bar)
	{
		werase(input_win);
		wprintw(input_win, "%ls", (curr_input_buf == NULL) ? L"" : curr_input_buf);
		wrefresh(input_win);
	}
}

int
is_input_buf_empty(void)
{
	return curr_input_buf_pos == NULL || *curr_input_buf_pos == 0;
}

/* Empties input buffer and resets input position. */
static void
reset_input_buf(wchar_t curr_input_buf[], size_t *curr_input_buf_pos)
{
	*curr_input_buf_pos = 0;
	curr_input_buf[0] = L'\0';
}

/* Composes and draws suggestion box. */
static void
display_suggestion_box(const wchar_t input[])
{
	size_t prefix;

	/* Don't do this for ESC because it's prefix for other keys. */
	if(!should_display_suggestion_box() || wcscmp(input, L"\033") == 0)
	{
		return;
	}

	/* Fill completion list with suggestions of keys and marks. */
	vle_compl_reset();
	vle_keys_suggest(input, &process_suggestion, !(cfg.sug.flags & SF_KEYS),
				(cfg.sug.flags & SF_FOLDSUBKEYS));
	/* Completion grouping removes duplicates.  Because user-defined keys are
	 * reported first, this has an effect of leaving only them in the resulting
	 * list, which is correct as they have higher priority. */
	vle_compl_finish_group();

	/* Handle registers suggestions. */
	prefix = wcsspn(input, L"0123456789");
	if((cfg.sug.flags & SF_REGISTERS) &&
			input[prefix] == L'"' && input[prefix + 1U] == L'\0')
	{
		/* No vle_compl_finish_group() after this to prevent sorting and
		 * deduplication. */
		regs_suggest(&process_suggestion, cfg.sug.maxregfiles);
	}

	if(vle_compl_get_count() != 0)
	{
		draw_suggestion_box();
		suggestions_are_visible = 1;
	}
}

/* Inserts key suggestion into completion list. */
static void
process_suggestion(const wchar_t lhs[], const wchar_t rhs[], const char descr[])
{
	if(rhs[0] != '\0')
	{
		char *const mb_rhs = wstr_to_spec(rhs);
		vle_compl_put_match(wstr_to_spec(lhs), mb_rhs);
		free(mb_rhs);
	}
	else
	{
		vle_compl_put_match(wstr_to_spec(lhs), descr);
	}
}

/* Draws suggestion box and all its items (from completion list). */
static void
draw_suggestion_box(void)
{
	/* TODO: consider possibility of multicolumn display. */

	const vle_compl_t *const items = vle_compl_get_items();
	int height;
	WINDOW *const win = prepare_suggestion_box(&height);
	size_t max_title_width;
	int i;

	max_title_width = 0U;
	for(i = 0; i < height; ++i)
	{
		const size_t width = utf8_strsw(items[i].text);
		if(width > max_title_width)
		{
			max_title_width = width;
		}
	}

	for(i = 0; i < height; ++i)
	{
		checked_wmove(win, i, 0);
		ui_stat_draw_popup_line(win, items[i].text, items[i].descr,
				max_title_width);
	}

	wrefresh(win);
}

/* Picks window to use for suggestion box and prepares it for displaying data.
 * Sets *height to number of suggestions to display.  Returns picked window. */
static WINDOW *
prepare_suggestion_box(int *height)
{
	WINDOW *win;
	const col_attr_t col = cfg.cs.color[SUGGEST_BOX_COLOR];
	const int count = vle_compl_get_count();

	if((cfg.sug.flags & SF_OTHERPANE) && curr_stats.number_of_windows == 2)
	{
		win = other_view->win;
		*height = MIN(count, getmaxy(win));
	}
	else
	{
		const int max_height = getmaxy(stdscr) - getmaxy(status_bar) -
			ui_stat_job_bar_height() - 2;
		*height = MIN(count, max_height);
		wresize(stat_win, *height, getmaxx(stdscr));
		ui_stat_reposition(getmaxy(status_bar), 1);
		win = stat_win;
	}

	wbkgdset(win, COLOR_PAIR(colmgr_get_pair(col.fg, col.bg)) | col.attr);
	werase(win);

	return win;
}

/* Removes suggestion box from the screen. */
static void
hide_suggestion_box(void)
{
	if(should_display_suggestion_box())
	{
		update_screen(UT_REDRAW);
	}
}

/* Checks whether suggestion box should be displayed.  Returns non-zero if so,
 * otherwise zero is returned. */
static int
should_display_suggestion_box(void)
{
	if((cfg.sug.flags & SF_NORMAL) && vle_mode_is(NORMAL_MODE))
	{
		return 1;
	}
	if((cfg.sug.flags & SF_VISUAL) && vle_mode_is(VISUAL_MODE))
	{
		return 1;
	}
	if((cfg.sug.flags & SF_VIEW) && vle_mode_is(VIEW_MODE))
	{
		return 1;
	}
	return 0;
}

/* vim: set tabstop=2 softtabstop=2 shiftwidth=2 noexpandtab cinoptions-=(0 : */
/* vim: set cinoptions+=t0 filetype=c : */
