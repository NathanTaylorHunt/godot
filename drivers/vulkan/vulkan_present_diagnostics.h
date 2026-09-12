/**************************************************************************/
/*  vulkan_present_diagnostics.h                                          */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/string/ustring.h"
#include "core/templates/safe_refcount.h"
#include "core/templates/vector.h"

// Makes a stalled frame say so, instead of hanging silently.
//
// The two GPU waits in the Vulkan driver used to pass UINT64_MAX, so a semaphore that is never
// signalled is a permanent freeze by construction, with nothing in the log and no way to learn
// anything about it except to capture a dump of the wedged process by hand. The waits are now
// bounded by a threshold and resumed after every expiry: the wait behaves exactly as it did, and a
// stall that lasts longer than the threshold reports what it is waiting for while it is still
// waiting.
//
// The report is worth little without the configuration that produced it, so the same object logs
// one presentation fingerprint at startup: the present mode actually granted (not the one asked
// for), the swap chain, the driver, and the capture and overlay layers that inject themselves into
// the presentation path.
class VulkanPresentDiagnostics {
public:
	// Which of the two waits a report is about.
	enum WaitKind {
		WAIT_FENCE,
		WAIT_SWAP_CHAIN_ACQUIRE,
	};

	// What one launch's presentation path is made of, as the driver knows it once a swap chain
	// exists.
	struct Presentation {
		String vsync_mode;
		String present_mode;
		uint32_t image_count = 0;
		String format;
		String device_name;
		String driver_name;
		String driver_info;
		uint32_t vendor_id = 0;
		uint32_t driver_version = 0;
		uint32_t api_version = 0;
		Vector<String> loader_layers;
	};

	// Reads the stall threshold from the project settings. Called once the device is up, so a
	// feature-tag override can lower it for a QA build without a template rebuild.
	void configure();

	// How long a wait may run before it is reported, in microseconds. Zero restores the single
	// infinite wait and reports nothing.
	uint64_t stall_threshold_usec() const { return stall_threshold; }

	// Logs the presentation fingerprint: once at startup, and again if a later swap chain is
	// granted a different present mode, which is how the mode a run actually uses gets into the
	// log. A resize that changes nothing logs nothing.
	void log_presentation(const Presentation &p_presentation);

	// Reports a wait that has not come back. p_report_index counts the expiries within one wait,
	// from 1; a wait that never returns reports at the first four expiries and then once a minute,
	// so a permanent stall stays in the log without flooding it.
	void report_stall(WaitKind p_wait, uint64_t p_elapsed_usec, uint32_t p_report_index, const String &p_detail);

	// Reports that a wait that had been reported as stalled finally returned.
	void report_recovered(WaitKind p_wait, uint64_t p_elapsed_usec);

	// Arms a stall of p_msec on the next fence wait, so the whole path -- report, capture request,
	// recovery -- can be proved on demand in a shipped QA build. Called from another thread than
	// the one that waits.
	void request_forced_stall(uint32_t p_msec);

	// Takes the armed stall, leaving nothing armed behind it.
	uint64_t take_forced_stall_usec();

	static const char *wait_name(WaitKind p_wait);

private:
	// Asks an out-of-process watcher for a capture, by dropping a request file in the directory
	// GODOT_GPU_STALL_CAPTURE_DIR names. Nothing is written when the variable is unset, so a run
	// with no watcher -- the editor, a player's build -- leaves nothing behind.
	void write_capture_request(const String &p_message);

	// The modules in this process that sit between the engine and the screen, named as the
	// presentation fingerprint reports them.
	static String describe_interceptors();

	uint64_t stall_threshold = 0;
	String present_mode_name = "unknown";
	bool presentation_logged = false;
	uint32_t capture_requests = 0;
	SafeNumeric<uint64_t> forced_stall_usec;
};
