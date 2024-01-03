/*
 * The Qubes OS Project, http://www.qubes-os.org
 *
 * Copyright (c) Invisible Things Lab
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#pragma once

#define PAGE_SIZE 0x1000

// registry configuration key, user mode and kernel mode names (kernel one currently unused)
#define REG_CONFIG_USER_KEY     L"Software\\Invisible Things Lab\\Qubes Tools"
#define REG_CONFIG_KERNEL_KEY   L"\\Registry\\Machine\\Software\\Invisible Things Lab\\Qubes Tools"

// value names in registry config
#define REG_CONFIG_FPS_VALUE        L"MaxFps"
#define REG_CONFIG_DIRTY_VALUE      L"UseDirtyBits"
#define REG_CONFIG_CURSOR_VALUE     L"DisableCursor"
#define REG_CONFIG_SEAMLESS_VALUE   L"SeamlessMode"

// path to the gui agent, launched by the watchdog service
#define REG_CONFIG_AGENT_PATH_VALUE  L"GuiAgentPath"

// event created by the helper service, trigger to simulate SAS (ctrl-alt-delete)
#define QGA_SAS_EVENT_NAME L"Global\\QGA_SAS_TRIGGER"

// When signaled, causes agent to shutdown gracefully.
#define QGA_SHUTDOWN_EVENT_NAME L"Global\\QGA_SHUTDOWN"

// these are hardcoded
#define	MIN_RESOLUTION_WIDTH	320UL
#define	MIN_RESOLUTION_HEIGHT	200UL

#define	IS_RESOLUTION_VALID(uWidth, uHeight)	((MIN_RESOLUTION_WIDTH <= (uWidth)) && (MIN_RESOLUTION_HEIGHT <= (uHeight)))
