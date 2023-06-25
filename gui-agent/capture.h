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

#define COBJMACROS
#include <D3D11.h>
#include <dxgi1_2.h>
#include <initguid.h>

#include "MemoryLockPublic.h"

typedef ULONG64 PFN_NUMBER;

#define ALIGN(x, a)	(((x) + (a) - 1) & ~((a) - 1))
#define	FRAMEBUFFER_PAGE_COUNT(width, height)	(ALIGN(((width)*(height)*4), PAGE_SIZE) / PAGE_SIZE)


// TODO: these structs should be opaque with functions to access public parts
// or rewrite everything in C++ ;)
typedef struct _CAPTURE_FRAME
{
	IDXGIResource* texture;
	DXGI_OUTDUPL_FRAME_INFO info;
	DXGI_MAPPED_RECT rect;
} CAPTURE_FRAME;

typedef struct _CAPTURE_CONTEXT
{
	UINT width;
	UINT height;
	IDXGIAdapter* adapter;
	ID3D11Device* device;
	IDXGIOutput1* output;
	IDXGIOutputDuplication* duplication;
	HANDLE thread; // capture loop
	HANDLE mlock; // MemoryLock device
	// mapped framebuffer location is constant as long as the capture interface is valid
	// this gets initialized when the first frame is acquired
	MEMORYLOCK_GET_PFNS_OUT* framebuffer_pfns;
	HANDLE frame_event; // capture thread -> main loop: new frame
	HANDLE ready_event; // main loop -> capture thread: frame processed
	HANDLE error_event; // capture thread -> main loop: capture error
	CAPTURE_FRAME* frame; // current frame data
} CAPTURE_CONTEXT;

// initialize capture interfaces and map framebuffer
CAPTURE_CONTEXT* CaptureInitialize(HANDLE frame_event, HANDLE error_event);

// start the capture thread
HRESULT CaptureStart(IN OUT CAPTURE_CONTEXT* ctx);

void CaptureTeardown(IN OUT CAPTURE_CONTEXT* ctx);
